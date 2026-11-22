#pragma once

#include <core/WindowTypes.h>
#include <win32/Window.h>

#include <functional>
#include <optional>
#include <unordered_map>
#include <vector>

namespace canton::win32 {

    // 管理同一应用会话内的多窗口 HWND、Win32 owner 从属、拓扑逆序销毁与非阻塞模态锁计数（EnableWindow + 防焦点跌落）。
    class WindowManager {
       public:
        using Factory = std::function<WindowCallbacks(core::WindowId)>;

        explicit WindowManager(Factory callbacksFactory = {});
        ~WindowManager();
        WindowManager(const WindowManager&) = delete;
        WindowManager& operator=(const WindowManager&) = delete;

        Window& createWindow(core::WindowId id, const core::WindowConfigSpec& config, WindowCallbacks callbacks);
        Window& createWindow(core::WindowId id, const core::WindowConfigSpec& config);
        void updateWindow(core::WindowId id, const core::WindowConfigSpec& config);
        // 严格按：释放模态锁恢复 EnableWindow(TRUE) -> 激活宿主目标窗 -> DestroyWindow 模态窗 执行
        void destroyWindow(core::WindowId id);
        // 按 owner 拓扑逆序销毁一组窗口
        void destroyWindows(const std::vector<core::WindowId>& ids);
        void destroyAll();

        void setDefaultIcons(IconHandle bigIcon, IconHandle smallIcon);
        void setWindowIcons(core::WindowId id, IconHandle bigIcon, IconHandle smallIcon);

        [[nodiscard]] Window* find(core::WindowId id) noexcept;
        [[nodiscard]] const Window* find(core::WindowId id) const noexcept;
        [[nodiscard]] void* lookupHwnd(core::WindowId id) const noexcept;
        [[nodiscard]] int modalLockCount(core::WindowId id) const noexcept;
        [[nodiscard]] std::size_t size() const noexcept { return windows_.size(); }
        [[nodiscard]] bool empty() const noexcept { return windows_.empty(); }
        [[nodiscard]] std::vector<core::WindowId> windowIds() const;

       private:
        struct ManagedWindow {
            core::WindowId id = 0;
            core::WindowConfigSpec config;
            std::unique_ptr<Window> window;
            IconHandle customBigIcon;
            IconHandle customSmallIcon;
            bool hasCustomIcon = false;
            int modalLocks = 0;
        };

        void recomputeModalLocks(std::optional<core::WindowId> closingWindow = std::nullopt);
        [[nodiscard]] std::vector<core::WindowId> ownedSubtree(core::WindowId rootId) const;
        [[nodiscard]] bool isOwnedDescendant(core::WindowId candidate, core::WindowId ancestor) const;
        void refreshWindowIcon(ManagedWindow& entry);

        Factory callbacksFactory_;
        std::vector<core::WindowId> order_;
        std::unordered_map<core::WindowId, ManagedWindow> windows_;
        IconHandle defaultBigIcon_;
        IconHandle defaultSmallIcon_;
    };

}
