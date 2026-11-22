#include <win32/WindowManager.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace canton::win32 {

    WindowManager::WindowManager(Factory callbacksFactory) : callbacksFactory_(std::move(callbacksFactory)) {}

    WindowManager::~WindowManager() {
        destroyAll();
    }

    Window& WindowManager::createWindow(core::WindowId id, const core::WindowConfigSpec& config) {
        return createWindow(id, config, callbacksFactory_ ? callbacksFactory_(id) : WindowCallbacks{});
    }

    Window& WindowManager::createWindow(core::WindowId id, const core::WindowConfigSpec& config, WindowCallbacks callbacks) {
        if (id == 0 || windows_.contains(id)) throw std::invalid_argument("WindowId 无效或已存在");
        void* ownerHwnd = nullptr;
        if (config.ownerWindowId) {
            ownerHwnd = lookupHwnd(*config.ownerWindowId);
            if (!ownerHwnd && config.modality == core::WindowModality::Owner) {
                throw std::invalid_argument("modality='owner' 的目标宿主窗口不存在");
            }
        } else if (config.modality == core::WindowModality::Owner) {
            throw std::invalid_argument("modality='owner' 需要有效的 ownerWindowId");
        }

        ManagedWindow entry;
        entry.id = id;
        entry.config = config;
        entry.window = std::make_unique<Window>(config, ownerHwnd, std::move(callbacks));
        refreshWindowIcon(entry);
        auto* raw = entry.window.get();
        windows_.emplace(id, std::move(entry));
        order_.push_back(id);
        recomputeModalLocks();
        return *raw;
    }

    void WindowManager::updateWindow(core::WindowId id, const core::WindowConfigSpec& config) {
        const auto it = windows_.find(id);
        if (it == windows_.end()) return;
        const bool modalityChanged = it->second.config.modality != config.modality || it->second.config.visible != config.visible;
        it->second.config = config;
        it->second.window->updateConfig(config);
        if (modalityChanged) recomputeModalLocks();
    }

    bool WindowManager::isOwnedDescendant(core::WindowId candidate, core::WindowId ancestor) const {
        auto cur = candidate;
        std::unordered_set<core::WindowId> seen{cur};
        while (true) {
            const auto it = windows_.find(cur);
            if (it == windows_.end() || !it->second.config.ownerWindowId) return false;
            const auto parent = *it->second.config.ownerWindowId;
            if (parent == ancestor) return true;
            if (!seen.insert(parent).second) return false;
            cur = parent;
        }
    }

    std::vector<core::WindowId> WindowManager::ownedSubtree(core::WindowId rootId) const {
        std::vector<core::WindowId> result;
        for (const auto id : order_) {
            if (id == rootId || isOwnedDescendant(id, rootId)) result.push_back(id);
        }
        return result;
    }

    void WindowManager::recomputeModalLocks(std::optional<core::WindowId> excludingModal) {
        std::unordered_map<core::WindowId, int> counts;
        for (const auto id : order_) counts[id] = 0;

        for (const auto modalId : order_) {
            if (excludingModal && modalId == *excludingModal) continue;
            const auto& modal = windows_.at(modalId);
            if (!modal.config.visible || modal.config.modality == core::WindowModality::None) continue;

            if (modal.config.modality == core::WindowModality::Owner) {
                if (!modal.config.ownerWindowId || !windows_.contains(*modal.config.ownerWindowId)) continue;
                const auto ownerId = *modal.config.ownerWindowId;
                for (const auto targetId : ownedSubtree(ownerId)) {
                    if (targetId == modalId || isOwnedDescendant(targetId, modalId)) continue;
                    // 其他活跃模态窗自身不被外层兄弟反向锁死，仅锁定非模态窗口或直接父级链
                    ++counts[targetId];
                }
            } else if (modal.config.modality == core::WindowModality::Application) {
                for (const auto targetId : order_) {
                    if (targetId == modalId || isOwnedDescendant(targetId, modalId)) continue;
                    ++counts[targetId];
                }
            }
        }

        for (const auto id : order_) {
            if (excludingModal && id == *excludingModal) continue;
            auto& entry = windows_.at(id);
            const int nextLocks = counts[id];
            if (entry.modalLocks != nextLocks) {
                entry.modalLocks = nextLocks;
                entry.window->setEnabled(nextLocks == 0);
            }
        }
    }

    void WindowManager::destroyWindow(core::WindowId id) {
        const auto it = windows_.find(id);
        if (it == windows_.end()) return;

        const auto closingHwnd = static_cast<HWND>(it->second.window->handle());
        const bool wasModal = it->second.config.modality != core::WindowModality::None;
        const bool wasForegroundOrActive = GetActiveWindow() == closingHwnd || GetForegroundWindow() == closingHwnd;
        const auto ownerId = it->second.config.ownerWindowId;

        // 严格顺序第 1 步：先释放该窗口持有的模态锁，恢复目标宿主窗 EnableWindow(TRUE)
        recomputeModalLocks(id);

        // 严格顺序第 2 步：在 DestroyWindow 之前激活未上锁的目标宿主窗口，防止焦点跌落到外部进程窗口
        if (wasModal || wasForegroundOrActive) {
            HWND restoreTarget = nullptr;
            if (ownerId) {
                if (const auto ownerIt = windows_.find(*ownerId); ownerIt != windows_.end() && ownerIt->second.modalLocks == 0 && ownerIt->second.config.visible) {
                    restoreTarget = static_cast<HWND>(ownerIt->second.window->handle());
                }
            }
            if (!restoreTarget) {
                for (auto r = order_.rbegin(); r != order_.rend(); ++r) {
                    if (*r == id) continue;
                    const auto& candidate = windows_.at(*r);
                    if (candidate.config.visible && candidate.modalLocks == 0) {
                        restoreTarget = static_cast<HWND>(candidate.window->handle());
                        break;
                    }
                }
            }
            if (restoreTarget && IsWindow(restoreTarget)) {
                SetForegroundWindow(restoreTarget);
                SetActiveWindow(restoreTarget);
            }
        }

        // 严格顺序第 3 步：最后 DestroyWindow 销毁模态/目标窗
        std::erase(order_, id);
        windows_.erase(it);
    }

    void WindowManager::destroyWindows(const std::vector<core::WindowId>& ids) {
        if (ids.empty()) return;
        const auto depthOf = [&](core::WindowId id) {
            int depth = 0;
            auto cur = id;
            std::unordered_set<core::WindowId> seen{id};
            while (true) {
                const auto it = windows_.find(cur);
                if (it == windows_.end() || !it->second.config.ownerWindowId || !seen.insert(*it->second.config.ownerWindowId).second) break;
                ++depth;
                cur = *it->second.config.ownerWindowId;
            }
            return depth;
        };
        auto sorted = ids;
        std::stable_sort(sorted.begin(), sorted.end(), [&](core::WindowId a, core::WindowId b) {
            return depthOf(a) > depthOf(b);
        });
        for (const auto id : sorted) destroyWindow(id);
    }

    void WindowManager::destroyAll() {
        const auto all = order_;
        destroyWindows(all);
    }

    void WindowManager::refreshWindowIcon(ManagedWindow& entry) {
        if (!entry.window) return;
        if (entry.hasCustomIcon) {
            entry.window->setIcons(entry.customBigIcon.get(), entry.customSmallIcon.get());
        } else {
            entry.window->setIcons(defaultBigIcon_.get(), defaultSmallIcon_.get());
        }
    }

    void WindowManager::setDefaultIcons(IconHandle bigIcon, IconHandle smallIcon) {
        defaultBigIcon_ = std::move(bigIcon);
        defaultSmallIcon_ = std::move(smallIcon);
        for (auto& [_, entry] : windows_) {
            if (!entry.hasCustomIcon) refreshWindowIcon(entry);
        }
    }

    void WindowManager::setWindowIcons(core::WindowId id, IconHandle bigIcon, IconHandle smallIcon) {
        const auto it = windows_.find(id);
        if (it == windows_.end()) return;
        it->second.hasCustomIcon = static_cast<bool>(bigIcon) || static_cast<bool>(smallIcon);
        it->second.customBigIcon = std::move(bigIcon);
        it->second.customSmallIcon = std::move(smallIcon);
        refreshWindowIcon(it->second);
    }

    Window* WindowManager::find(core::WindowId id) noexcept {
        const auto it = windows_.find(id);
        return it == windows_.end() ? nullptr : it->second.window.get();
    }

    const Window* WindowManager::find(core::WindowId id) const noexcept {
        const auto it = windows_.find(id);
        return it == windows_.end() ? nullptr : it->second.window.get();
    }

    void* WindowManager::lookupHwnd(core::WindowId id) const noexcept {
        const auto* win = find(id);
        return win ? win->handle() : nullptr;
    }

    int WindowManager::modalLockCount(core::WindowId id) const noexcept {
        const auto it = windows_.find(id);
        return it == windows_.end() ? 0 : it->second.modalLocks;
    }

    std::vector<core::WindowId> WindowManager::windowIds() const {
        return order_;
    }

}
