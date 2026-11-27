#include <runtime/AppSession.h>

#include <core/Clock.h>
#include <core/InputIntent.h>
#include <core/Layout.h>
#include <runtime/Digest.h>
#include <runtime/ErrorScreenModel.h>
#include <runtime/FileSystem.h>
#include <runtime/PainterResources.h>
#include <runtime/UiPackage.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <unordered_set>
#include <utility>

namespace canton::runtime {
    namespace {
        constexpr std::string_view TAG = "AppSession";

        AppResolver::MountPackage makeMounter(const AppSessionOptions& options) {
            return options.mountPackage ? options.mountPackage : AppResolver::MountPackage(mountUiPackage);
        }

        IconBitmap scaleBitmapBilinear(const core::BitmapContent& src, int dstW, int dstH) {
            IconBitmap out{dstW, dstH, std::vector<std::uint32_t>(static_cast<std::size_t>(dstW * dstH), 0)};
            if (src.width <= 0 || src.height <= 0 || src.pixels.empty()) return out;
            for (int y = 0; y < dstH; ++y) {
                const float sy = (static_cast<float>(y) + 0.5f) * static_cast<float>(src.height) / static_cast<float>(dstH) - 0.5f;
                const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, src.height - 1);
                const int y1 = std::clamp(y0 + 1, 0, src.height - 1);
                const float fy = std::clamp(sy - static_cast<float>(y0), 0.0f, 1.0f);
                for (int x = 0; x < dstW; ++x) {
                    const float sx = (static_cast<float>(x) + 0.5f) * static_cast<float>(src.width) / static_cast<float>(dstW) - 0.5f;
                    const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, src.width - 1);
                    const int x1 = std::clamp(x0 + 1, 0, src.width - 1);
                    const float fx = std::clamp(sx - static_cast<float>(x0), 0.0f, 1.0f);

                    const auto p00 = src.pixels[static_cast<std::size_t>(y0 * src.width + x0)];
                    const auto p10 = src.pixels[static_cast<std::size_t>(y0 * src.width + x1)];
                    const auto p01 = src.pixels[static_cast<std::size_t>(y1 * src.width + x0)];
                    const auto p11 = src.pixels[static_cast<std::size_t>(y1 * src.width + x1)];

                    const auto channel = [&](unsigned shift) -> std::uint32_t {
                        const float c00 = static_cast<float>((p00 >> shift) & 0xffu);
                        const float c10 = static_cast<float>((p10 >> shift) & 0xffu);
                        const float c01 = static_cast<float>((p01 >> shift) & 0xffu);
                        const float c11 = static_cast<float>((p11 >> shift) & 0xffu);
                        const float top = c00 + (c10 - c00) * fx;
                        const float bot = c01 + (c11 - c01) * fx;
                        return static_cast<std::uint32_t>(std::clamp(std::lround(top + (bot - top) * fy), 0l, 255l));
                    };
                    out.pixels[static_cast<std::size_t>(y * dstW + x)] = (channel(24) << 24u) | (channel(16) << 16u) | (channel(8) << 8u) | channel(0);
                }
            }
            return out;
        }

        std::optional<IconBitmap> renderPainterToIcon(const core::PainterContent& content, int size) {
            if (const auto* bmp = dynamic_cast<const core::BitmapContent*>(&content)) {
                return scaleBitmapBilinear(*bmp, size, size);
            }
            if (const auto* vec = dynamic_cast<const core::VectorContent*>(&content)) {
                const auto whole = vec->intrinsicSize.value_or(core::Size{static_cast<float>(size), static_cast<float>(size)});
                IconBitmap out{size, size, vec->rasterize({0, 0, whole.width, whole.height}, size, size)};
                if (out.pixels.size() == static_cast<std::size_t>(size * size)) return out;
            }
            return std::nullopt;
        }
    }

    AppSession::AppSession(const core::TextMeasurer& measurer, std::shared_ptr<core::FontRegistry> fonts, AppSessionOptions options)
        : fonts_(std::move(fonts)),
          textLayout_(measurer, 256, 8 * 1024 * 1024, fonts_.get()),
          options_(std::move(options)),
          resourceWorkers_(createResourceWorkers()),
          ioPool_(std::make_shared<IoThreadPool>(4)),
          appPaths_(options_.pathsConfig),
          graphicsEnv_(options_.initialGraphicsEnv),
          launchRequest_(options_.launchRequest),
          platformBridge_(std::make_shared<quickjs::PlatformBridge>()),
          controlRuntime_(core::SceneFramePipeline(core::LayoutEngine(textLayout_))),
          source_(
              AppResolver(makeMounter(options_)),
              RuntimePackageLoader(
                  options_.distResources ? options_.distResources : [workers = resourceWorkers_](std::shared_ptr<const core::UiPackage> package) {
                      return RuntimeLoadResources{
                          packagePainterLoader(package, workers),
                          packageAssetLoader(package, workers),
                          packageAssetExistsChecker(package, workers),
                      };
                  },
                  fonts_,
                  options_.painters,
                  options_.shaderCompiler,
                  platformBridge_),
              options_.live),
          fallbackPipeline_(core::SceneFramePipeline(core::LayoutEngine(textLayout_))) {
        initPlatformBridge();
        if (options_.hudFactory) {
            globalDiagnostics_.setHud(options_.hudFactory());
            globalDiagnostics_.setNotifySink([this](core::LogLevel level, std::string_view tag, const std::string& title, const std::string& message, bool coalesce) {
                enqueueMirroredToast(level, tag, title, message, coalesce);
            });
        }
        controlRuntime_.setExternalSurfaceHost(true);
        controlRuntime_.setEventSlotValidator([this](const core::EventSlotId& slot) {
            for (const auto& [_, win] : windows_) {
                if (win->pipeline.scene().hasEventSlot(slot)) return true;
            }
            return false;
        });
    }

    AppSession::~AppSession() {
        controlRuntime_.setWorkAvailable({});
        cancelAllNetworkRequests();
        timerQueue_.clear();
        activeTrays_.clear();
        if (options_.platform.onTrayClearAll) options_.platform.onTrayClearAll();
        if (activeMediaSession_.active) {
            activeMediaSession_ = {};
            if (options_.platform.onMediaSessionUpdated) options_.platform.onMediaSessionUpdated(activeMediaSession_, {});
        }
        teardownAllWindows();
        if (ioPool_) ioPool_->shutdown();
    }

    void AppSession::setWorkAvailable(std::function<void()> callback) {
        controlRuntime_.setWorkAvailable(std::move(callback));
    }

    void AppSession::configure(AppSourceConfig config) {
        reloadRequested_ = false;
        exitRequested_ = false;
        applyLoadOutcome(source_.configure(std::move(config)));
    }

    void AppSession::setStartupError(const std::string& message) {
        sessionLoaded_ = false;
        globalDiagnostics_.setError(makeErrorScreenModel(ErrorSource::AppPackage, message, {}, {}, false));
        fallbackPipeline_.enqueueIntent(core::InputIntent::diagnostics());
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::requestReload() noexcept {
        reloadRequested_ = true;
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::notifyToast(core::LogLevel level, std::string_view tag, std::string title, std::string message, bool coalesce) {
        (void)globalDiagnostics_.notify(level, tag, std::move(title), std::move(message), coalesce);
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::triggerSimulatedError(std::string summary, std::string detail) {
        controlRuntime_.suspend();
        sessionLoaded_ = false;
        globalDiagnostics_.setError(makeErrorScreenModel(ErrorSource::ScriptRuntime, std::move(summary), std::move(detail), {}, true));
        for (auto& [_, win] : windows_) {
            win->pipeline.enqueueIntent(core::InputIntent::diagnostics());
        }
        fallbackPipeline_.enqueueIntent(core::InputIntent::diagnostics());
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::enqueueMirroredToast(core::LogLevel level, std::string_view tag, std::string title, std::string message, bool coalesce) {
        if (coalesce) {
            for (auto& existing : pendingToasts_) {
                if (existing.title == title) {
                    existing.level = level;
                    existing.tag = std::string(tag);
                    existing.message = std::move(message);
                    return;
                }
            }
        }
        pendingToasts_.push_back({level, std::string(tag), std::move(title), std::move(message), coalesce});
        while (pendingToasts_.size() > 6) pendingToasts_.erase(pendingToasts_.begin());
    }

    namespace {
        bool isHudFriendlyWindow(const WindowSurfaceState& win, const core::WindowConfigSpec& cfg) noexcept {
            if (!cfg.visible || cfg.placement == core::WindowPlacement::Minimized) return false;
            if (cfg.transparent || cfg.role != core::WindowRole::Standard) return false;
            const double scale = win.dpiScale > 0.0 ? win.dpiScale : 1.0;
            return (static_cast<double>(win.widthPx) / scale) >= 300.0 && (static_cast<double>(win.heightPx) / scale) >= 180.0;
        }
    }

    core::WindowId AppSession::selectToastTargetWindow(const std::vector<core::WindowId>& order,
        const std::function<const WindowSurfaceState&(core::WindowId)>& resolveWindow,
        const std::function<const core::WindowConfigSpec&(const WindowSurfaceState&)>& resolveConfig) const {
        if (order.empty()) return 0;
        for (const auto id : order) {
            const auto& win = resolveWindow(id);
            if (win.focused && isHudFriendlyWindow(win, resolveConfig(win))) return id;
        }
        if (lastFocusedWindowId_ != 0 && std::find(order.begin(), order.end(), lastFocusedWindowId_) != order.end()) {
            const auto& win = resolveWindow(lastFocusedWindowId_);
            if (isHudFriendlyWindow(win, resolveConfig(win))) return lastFocusedWindowId_;
        }
        for (const auto id : order) {
            const auto& win = resolveWindow(id);
            if (isHudFriendlyWindow(win, resolveConfig(win))) return id;
        }
        for (const auto id : order) {
            const auto& win = resolveWindow(id);
            const auto& cfg = resolveConfig(win);
            if (cfg.visible && cfg.placement != core::WindowPlacement::Minimized && !cfg.transparent) return id;
        }
        for (const auto id : order) {
            const auto& win = resolveWindow(id);
            if (resolveConfig(win).visible) return id;
        }
        return 0;
    }

    core::WindowId AppSession::selectPrimaryErrorWindowId() const {
        if (windowOrder_.empty()) return 0;
        for (const auto id : windowOrder_) {
            const auto& win = *windows_.at(id);
            if (win.focused && isHudFriendlyWindow(win, win.config)) return id;
        }
        if (lastFocusedWindowId_ != 0 && windows_.contains(lastFocusedWindowId_)) {
            const auto& win = *windows_.at(lastFocusedWindowId_);
            if (isHudFriendlyWindow(win, win.config)) return lastFocusedWindowId_;
        }
        core::WindowId bestFriendly = 0;
        std::int64_t bestFriendlyArea = -1;
        core::WindowId bestOpaque = 0;
        std::int64_t bestOpaqueArea = -1;
        core::WindowId firstVisible = 0;
        for (const auto id : windowOrder_) {
            const auto& win = *windows_.at(id);
            if (!win.config.visible || win.config.placement == core::WindowPlacement::Minimized) continue;
            if (firstVisible == 0) firstVisible = id;
            const auto area = static_cast<std::int64_t>(win.widthPx) * static_cast<std::int64_t>(win.heightPx);
            if (isHudFriendlyWindow(win, win.config) && area > bestFriendlyArea) {
                bestFriendly = id;
                bestFriendlyArea = area;
            }
            if (!win.config.transparent && area > bestOpaqueArea) {
                bestOpaque = id;
                bestOpaqueArea = area;
            }
        }
        if (bestFriendly != 0) return bestFriendly;
        if (bestOpaque != 0) return bestOpaque;
        return firstVisible != 0 ? firstVisible : windowOrder_.front();
    }

    void AppSession::shutdownSession() {
        exitRequested_ = true;
        sessionLoaded_ = false;
        cancelAllNetworkRequests();
        timerQueue_.clear();
        activeTrays_.clear();
        windowTaskbarBadges_.clear();
        windowTaskbarActions_.clear();
        if (options_.platform.onTrayClearAll) options_.platform.onTrayClearAll();
        if (activeMediaSession_.active) {
            activeMediaSession_ = {};
            if (options_.platform.onMediaSessionUpdated) options_.platform.onMediaSessionUpdated(activeMediaSession_, {});
        }
        teardownAllWindows();
        controlRuntime_.reset();
    }

    std::vector<core::WindowId> AppSession::sortDestroyOrder(
        const std::vector<core::WindowId>& toDestroy,
        const std::unordered_map<core::WindowId, std::optional<core::WindowId>>& owners) {
        const std::unordered_set<core::WindowId> targetSet(toDestroy.begin(), toDestroy.end());
        const auto depthOf = [&](core::WindowId id) {
            int depth = 0;
            auto cur = id;
            std::unordered_set<core::WindowId> seen{id};
            while (true) {
                const auto it = owners.find(cur);
                if (it == owners.end() || !it->second || !seen.insert(*it->second).second) break;
                ++depth;
                cur = *it->second;
            }
            return depth;
        };
        auto sorted = toDestroy;
        std::stable_sort(sorted.begin(), sorted.end(), [&](core::WindowId a, core::WindowId b) {
            return depthOf(a) > depthOf(b);
        });
        return sorted;
    }

    void AppSession::teardownAllWindows() {
        if (windows_.empty()) return;
        std::unordered_map<core::WindowId, std::optional<core::WindowId>> owners;
        for (const auto& [id, win] : windows_) owners[id] = win->config.ownerWindowId;
        const auto ordered = sortDestroyOrder(windowOrder_, owners);
        for (const auto id : ordered) {
            if (options_.platform.onWindowDestroyed) options_.platform.onWindowDestroyed(id);
            windows_.erase(id);
        }
        windowOrder_.clear();
        lastFocusedWindowId_ = 0;
    }

    void AppSession::applyLoadOutcome(PackageLoadOutcome outcome) {
        if (outcome.pending) return;
        teardownAllWindows();
        cancelAllNetworkRequests();
        timerQueue_.clear();
        activeTrays_.clear();
        windowTaskbarBadges_.clear();
        windowTaskbarActions_.clear();
        if (options_.platform.onTrayClearAll) options_.platform.onTrayClearAll();
        if (activeMediaSession_.active) {
            activeMediaSession_ = {};
            if (options_.platform.onMediaSessionUpdated) options_.platform.onMediaSessionUpdated(activeMediaSession_, {});
        }
        sessionLoaded_ = false;
        everOpenedWindow_ = false;
        pendingWindowEvents_.clear();
        iconCache_.clear();
        pendingIcons_.clear();
        appConfig_ = {};
        controlRuntime_.reset();
        globalDiagnostics_.clearError();
        fallbackPipeline_.reset();

        if (outcome.error) globalDiagnostics_.setError(std::move(*outcome.error));
        for (const auto& log : outcome.logs) core::Log::write(log.level, TAG, log.message);
        for (const auto& toast : outcome.toasts) {
            (void)globalDiagnostics_.notify(toast.level, TAG, toast.title, toast.message, toast.coalesce);
        }

        if (!outcome.loaded) {
            fallbackPipeline_.enqueueIntent(core::InputIntent::diagnostics());
            return;
        }

        activePainterLoader_ = outcome.scriptHost ? outcome.scriptHost->painterLoader() : core::PainterLoader{};
        fontGeneration_ = fonts_ ? fonts_->generation() : 0;
        controlRuntime_.setScriptHost(std::move(outcome.scriptHost));
        sessionLoaded_ = true;
    }

    void AppSession::applyRuntimeFailure(const std::string& message, RuntimeFrameErrorPhase phase) {
        const auto src = phase == RuntimeFrameErrorPhase::Pipeline ? ErrorSource::NativeTransaction : ErrorSource::ScriptRuntime;
        const auto related = source_.activeSource() == PackageSource::Dist ? source_.config().distOverride : std::filesystem::path{};
        controlRuntime_.suspend();
        sessionLoaded_ = false;
        globalDiagnostics_.setError(makeErrorScreenModel(src, message, {}, related));
        (void)globalDiagnostics_.notify(core::LogLevel::Error, TAG, "运行时执行失败", message);
        for (auto& [_, win] : windows_) {
            win->pipeline.enqueueIntent(core::InputIntent::diagnostics());
        }
        fallbackPipeline_.enqueueIntent(core::InputIntent::diagnostics());
    }

    std::unique_ptr<WindowSurfaceState> AppSession::makeWindowSurface(const quickjs::WindowMutation& mutation) {
        auto surface = std::make_unique<WindowSurfaceState>(textLayout_);
        const auto windowId = mutation.windowId;
        surface->windowId = windowId;
        surface->rootNodeId = mutation.rootNodeId;
        surface->config = mutation.config;
        surface->widthPx = std::max(1, static_cast<int>(std::lround(mutation.config.sizeDp.width)));
        surface->heightPx = std::max(1, static_cast<int>(std::lround(mutation.config.sizeDp.height)));
        if (options_.hudFactory) surface->diagnostics.setHud(options_.hudFactory());
        surface->text.setCancelCompositionHook([this, windowId] {
            if (options_.platform.onCancelPlatformIme) options_.platform.onCancelPlatformIme(windowId);
        });
        return surface;
    }

    void AppSession::commitWindowChanges(const std::optional<core::AppConfigSpec>& appSpec,
        const std::vector<quickjs::WindowMutation>& mutations,
        std::unordered_map<core::WindowId, std::unique_ptr<WindowSurfaceState>>& newWindows,
        const std::vector<core::WindowId>& destroyOrder) {
        const bool appIconChanged = appSpec && appSpec->icon != appConfig_.icon;
        if (appSpec) {
            appConfig_ = *appSpec;
            if (options_.platform.onAppConfigChanged) options_.platform.onAppConfigChanged(appConfig_);
        }
        for (const auto& mutation : mutations) {
            const auto id = mutation.windowId;
            if (mutation.kind == quickjs::WindowMutationKind::Create) {
                auto pending = newWindows.extract(id);
                if (pending.empty()) continue;
                auto& win = *pending.mapped();
                windows_.insert(std::move(pending));
                windowOrder_.push_back(id);
                everOpenedWindow_ = true;
                if (options_.platform.onWindowCreated) options_.platform.onWindowCreated(id, win.config);
                requestIconIfNeeded(win);
            } else if (mutation.kind == quickjs::WindowMutationKind::Update) {
                if (auto* win = window(id)) {
                    win->config = mutation.config;
                    if (options_.platform.onWindowUpdated) options_.platform.onWindowUpdated(id, win->config);
                    requestIconIfNeeded(*win);
                }
            }
        }
        auto* host = controlRuntime_.scriptHost();
        for (const auto id : destroyOrder) {
            host->unpublishWindowScene(id);
            if (options_.platform.onWindowDestroyed) options_.platform.onWindowDestroyed(id);
            windows_.erase(id);
            std::erase(windowOrder_, id);
            if (lastFocusedWindowId_ == id) lastFocusedWindowId_ = 0;
        }
        if (appIconChanged) {
            for (auto& [_, win] : windows_) requestIconIfNeeded(*win);
            for (const auto& [trayId, _] : activeTrays_) refreshTrayIcon(trayId);
        }
    }

    void AppSession::dispatchWindowCommands() {
        for (const auto& cmd : controlRuntime_.scriptHost()->takeWindowCommands()) {
            if (!windows_.contains(cmd.windowId)) continue;
            if (cmd.kind == core::WindowCommandKind::RequestClose) {
                notifyWindowCloseRequest(cmd.windowId);
            } else if (options_.platform.onWindowCommand) {
                options_.platform.onWindowCommand(cmd.windowId, cmd.kind);
            }
        }
    }

    void AppSession::requestIconAsset(const std::string& path) {
        if (path.empty() || iconCache_.contains(path) || !activePainterLoader_) return;
        if (std::any_of(pendingIcons_.begin(), pendingIcons_.end(), [&](const PendingIcon& item) { return item.path == path; })) return;
        pendingIcons_.push_back({path, activePainterLoader_(path, [this] { controlRuntime_.notifyWorkAvailable(); })});
    }

    void AppSession::requestIconIfNeeded(WindowSurfaceState& win) {
        const auto effective = !win.config.icon.empty() ? win.config.icon : appConfig_.icon;
        if (effective == win.appliedIconPath) return;
        win.appliedIconPath = effective;
        if (effective.empty()) {
            if (options_.platform.onWindowIconChanged) options_.platform.onWindowIconChanged(win.windowId, nullptr, nullptr);
            return;
        }
        if (const auto cached = iconCache_.find(effective); cached != iconCache_.end()) {
            if (options_.platform.onWindowIconChanged) options_.platform.onWindowIconChanged(win.windowId, &cached->second.bigIcon, &cached->second.smallIcon);
            return;
        }
        requestIconAsset(effective);
    }

    void AppSession::pumpPendingIcons() {
        for (auto it = pendingIcons_.begin(); it != pendingIcons_.end();) {
            if (it->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            const auto path = it->path;
            const auto loaded = it->future.get();
            it = pendingIcons_.erase(it);
            if (!loaded.content) {
                core::Log::w(TAG, "窗口图标解码失败", path, loaded.error);
                continue;
            }
            const auto bigIcon = renderPainterToIcon(*loaded.content, 32);
            const auto smallIcon = renderPainterToIcon(*loaded.content, 16);
            if (!bigIcon || !smallIcon) continue;
            auto& entry = iconCache_[path] = DecodedIcons{*bigIcon, *smallIcon};
            for (auto& [_, win] : windows_) {
                const auto effective = !win->config.icon.empty() ? win->config.icon : appConfig_.icon;
                if (effective == path && options_.platform.onWindowIconChanged) {
                    options_.platform.onWindowIconChanged(win->windowId, &entry.bigIcon, &entry.smallIcon);
                }
            }
            for (const auto& [trayId, spec] : activeTrays_) {
                const auto effective = !spec.icon.empty() ? spec.icon : appConfig_.icon;
                if (effective == path) refreshTrayIcon(trayId);
            }
            for (const auto& [winId, badge] : windowTaskbarBadges_) {
                if (badge && badge->icon == path) refreshWindowTaskbarIcons(winId);
            }
            for (const auto& [winId, actions] : windowTaskbarActions_) {
                for (const auto& act : actions) {
                    if (act.icon == path) {
                        refreshWindowTaskbarIcons(winId);
                        break;
                    }
                }
            }
        }
    }

    void AppSession::prepareDiagnosticsTextOps(core::PublishedFrame& frame) const {
        auto& content = frame.content;
        for (auto* ops : {&content.overlayDrawOps, &content.diagnosticsErrorDrawOps, &content.diagnosticsBadgeDrawOps, &content.diagnosticsFpsDrawOps, &content.diagnosticsToastDrawOps}) {
            for (auto& op : *ops) {
                if (op.type != core::DrawOpType::DrawText) continue;
                if (ops != &content.overlayDrawOps && op.textStyle.fontFamily.empty() && !options_.diagnosticsFontFamily.empty()) {
                    op.textStyle.fontFamily = options_.diagnosticsFontFamily;
                    op.textLayout.reset();
                }
                core::DrawOpsBuilder::prepareText(op, textLayout_);
            }
        }
    }

    void AppSession::pumpFallbackFrame(double nowMillis) {
        (void)globalDiagnostics_.tick(controlRuntime_, nowMillis);
        const core::Rect bounds{0.0f, 0.0f, static_cast<float>(fallbackWidthPx_), static_cast<float>(fallbackHeightPx_)};
        const float scale = static_cast<float>(fallbackDpiScale_ > 0.0 ? fallbackDpiScale_ : 1.0);
        (void)fallbackPipeline_.publishRetained([&](const core::NativeScene&, core::PublishedFrame& frame) {
            frame.content.viewport = {bounds.width, bounds.height};
            globalDiagnostics_.prepareFrame(bounds, options_.detailedErrorScreen, source_.activeSource(), source_.liveUnavailable(), textLayout_, frame.content, scale);
            frame.content.errorFrame = globalDiagnostics_.hasError() ? std::optional<std::string>(globalDiagnostics_.error()->summary) : std::nullopt;
            prepareDiagnosticsTextOps(frame);
        });
    }

    void AppSession::drainPendingWindowEvents() {
        if (pendingWindowEvents_.empty() || !sessionLoaded_ || globalDiagnostics_.hasError()) return;
        auto* host = controlRuntime_.scriptHost();
        if (!host) return;
        auto batch = std::move(pendingWindowEvents_);
        pendingWindowEvents_.clear();
        for (const auto& ev : batch) {
            if (!windows_.contains(ev.windowId)) continue;
            quickjs::CallbackInvokeResult res{true, {}};
            switch (ev.kind) {
                case PendingWindowEvent::Kind::Geometry:
                    res = host->invokeWindowGeometryChanged(ev.windowId, ev.geometry);
                    break;
                case PendingWindowEvent::Kind::Focus:
                    res = host->invokeWindowFocusChanged(ev.windowId, ev.focused);
                    break;
                case PendingWindowEvent::Kind::Dpi:
                    res = host->invokeWindowDpiChanged(ev.windowId, ev.dpiScale);
                    break;
                case PendingWindowEvent::Kind::CloseRequest:
                    res = host->invokeWindowCloseRequest(ev.windowId);
                    break;
            }
            if (!res.ok) {
                applyRuntimeFailure(res.error, RuntimeFrameErrorPhase::Event);
                return;
            }
        }
    }

    std::vector<core::WindowId> AppSession::pumpFrame(double nowMillis) {
        std::vector<core::WindowId> changedWindows;
        if (exitRequested_ || pumpingFrame_) return changedWindows;
        struct FrameReentrancyGuard {
            bool& flag;
            explicit FrameReentrancyGuard(bool& f) : flag(f) { flag = true; }
            ~FrameReentrancyGuard() { flag = false; }
        } guard(pumpingFrame_);

        source_.pumpLive(controlRuntime_, globalDiagnostics_, [this](PackageLoadOutcome outcome) { applyLoadOutcome(std::move(outcome)); });
        if (controlRuntime_.consumeReloadRequest() || std::exchange(reloadRequested_, false)) {
            applyLoadOutcome(source_.reload());
        }
        pumpPendingIcons();

        if (!sessionLoaded_ || globalDiagnostics_.hasError()) {
            if (!windows_.empty()) {
                const auto primaryErrorId = selectPrimaryErrorWindowId();
                if (!pendingToasts_.empty() && primaryErrorId != 0) {
                    auto& target = *windows_.at(primaryErrorId);
                    if (!target.config.transparent) {
                        for (const auto& toast : pendingToasts_) {
                            if (target.diagnostics.notify(toast.level, toast.tag, toast.title, toast.message, toast.coalesce)) {
                                target.pipeline.enqueueIntent(core::InputIntent::diagnostics());
                            }
                        }
                    }
                    pendingToasts_.clear();
                }
                for (const auto windowId : windowOrder_) {
                    auto& win = *windows_.at(windowId);
                    const auto prevRev = win.pipeline.publishedFrame().revision;
                    const core::Rect bounds{0.0f, 0.0f, static_cast<float>(win.widthPx), static_cast<float>(win.heightPx)};
                    const float scale = static_cast<float>(win.dpiScale > 0.0 ? win.dpiScale : 1.0);
                    if (win.diagnostics.tickToasts(nowMillis)) {
                        win.pipeline.enqueueIntent(core::InputIntent::diagnostics());
                    }
                    const bool isPrimary = windowId == primaryErrorId;
                    const auto* appErr = globalDiagnostics_.error();
                    (void)win.pipeline.publishRetained([&](const core::NativeScene&, core::PublishedFrame& frame) {
                        frame.content.viewport = {bounds.width, bounds.height};
                        win.diagnostics.prepareFrame(bounds, options_.detailedErrorScreen, source_.activeSource(), source_.liveUnavailable(), textLayout_, frame.content, scale, isPrimary, appErr);
                        if (win.config.transparent) {
                            frame.content.diagnosticsBadgeDrawOps.clear();
                            frame.content.diagnosticsFpsDrawOps.clear();
                            frame.content.diagnosticsToastDrawOps.clear();
                        }
                        frame.content.errorFrame = appErr ? std::optional<std::string>(appErr->summary) : std::nullopt;
                        prepareDiagnosticsTextOps(frame);
                    });
                    bool overlayChanged = false;
                    if (auto* hud = win.diagnostics.hud()) {
                        overlayChanged = hud->updateOverlay(win.pipeline.timeline(), win.pipeline.publishedFrame(), win.pipeline.scene().tree(), win.rootNodeId, textLayout_, nowMillis, scale);
                        win.overlayDirty = false;
                    }
                    if (win.pipeline.publishedFrame().revision != prevRev || overlayChanged) changedWindows.push_back(windowId);
                }
            }
            if (needsFallbackWindow()) {
                pendingToasts_.clear();
                const auto prevRev = fallbackPipeline_.publishedFrame().revision;
                pumpFallbackFrame(nowMillis);
                if (fallbackPipeline_.publishedFrame().revision != prevRev) changedWindows.push_back(0);
            }
            return changedWindows;
        }

        drainPendingWindowEvents();
        drainBackgroundEvents();
        pumpTimers(nowMillis);
        if (!sessionLoaded_ || globalDiagnostics_.hasError()) return changedWindows;

        for (const auto windowId : windowOrder_) {
            if (auto* win = window(windowId)) {
                while (const auto step = win->wheelSmoother.tick(nowMillis)) {
                    dispatchWheelSample(*win, step->first, step->second);
                }
            }
        }

        const auto semantic = controlRuntime_.semanticCheckpoint(nowMillis);
        if (!semantic.ok) {
            applyRuntimeFailure(semantic.error, RuntimeFrameErrorPhase::Event);
            if (needsFallbackWindow()) {
                pumpFallbackFrame(nowMillis);
                changedWindows.push_back(0);
            }
            return changedWindows;
        }

        if (controlRuntime_.checkFontPreloadHold()) return changedWindows;

        auto* host = controlRuntime_.scriptHost();
        if (!host) return changedWindows;

        bool fontChanged = false;
        if (const auto gen = fonts_ ? fonts_->generation() : 0; gen != fontGeneration_) {
            fontGeneration_ = gen;
            fontChanged = true;
        }

        bool needsScriptVisual = host->hasPendingVisualWork() || host->hasPendingFocusCommands() || host->hasPendingWindowWork();
        for (const auto& [_, win] : windows_) {
            if (win->pipeline.scene().tree().activeAnimationCount() > 0) needsScriptVisual = true;
        }

        std::shared_ptr<core::RearrangeSubmission> rearrangeSubmission;
        if (needsScriptVisual) {
            const auto prepared = host->prepareVisualFrame(nowMillis);
            if (!prepared.ok) {
                applyRuntimeFailure(prepared.error, RuntimeFrameErrorPhase::Animation);
                if (needsFallbackWindow()) {
                    pumpFallbackFrame(nowMillis);
                    changedWindows.push_back(0);
                }
                return changedWindows;
            }
            rearrangeSubmission = host->pendingRearrangeSubmission();
        }

        const auto appSpec = host->takeAppConfig();
        const auto mutations = host->takeWindowMutations();
        auto frameOrder = windowOrder_;
        std::unordered_map<core::WindowId, std::unique_ptr<WindowSurfaceState>> newWindows;
        std::unordered_map<core::WindowId, const core::WindowConfigSpec*> updatedConfigs;
        std::vector<core::WindowId> destroyOrder;
        if (!mutations.empty()) {
            std::unordered_map<core::WindowId, std::optional<core::WindowId>> owners;
            for (const auto& [id, win] : windows_) owners[id] = win->config.ownerWindowId;
            for (const auto& mutation : mutations) {
                const auto id = mutation.windowId;
                switch (mutation.kind) {
                    case quickjs::WindowMutationKind::Create:
                        newWindows.emplace(id, makeWindowSurface(mutation));
                        frameOrder.push_back(id);
                        owners[id] = mutation.config.ownerWindowId;
                        break;
                    case quickjs::WindowMutationKind::Update:
                        updatedConfigs[id] = &mutation.config;
                        owners[id] = mutation.config.ownerWindowId;
                        break;
                    case quickjs::WindowMutationKind::Destroy:
                        std::erase(frameOrder, id);
                        if (windows_.contains(id)) destroyOrder.push_back(id);
                        newWindows.erase(id);
                        break;
                }
            }
            destroyOrder = sortDestroyOrder(destroyOrder, owners);
        }
        const auto frameWindow = [&](core::WindowId id) -> WindowSurfaceState& {
            if (const auto found = newWindows.find(id); found != newWindows.end()) return *found->second;
            return *windows_.at(id);
        };
        const auto frameConfig = [&](const WindowSurfaceState& win) -> const core::WindowConfigSpec& {
            const auto found = updatedConfigs.find(win.windowId);
            return found == updatedConfigs.end() ? win.config : *found->second;
        };

        // 分发焦点命令到所属窗口（未带窗口身份的命令按请求器匹配各窗口）
        const auto focusCommands = host->takeScopedFocusCommands();
        std::unordered_map<core::WindowId, std::vector<core::FocusCommand>> focusByWindow;
        for (const auto& scoped : focusCommands) {
            if (scoped.windowId != 0) {
                focusByWindow[scoped.windowId].push_back(scoped.command);
            } else {
                for (const auto id : frameOrder) focusByWindow[id].push_back(scoped.command);
            }
        }

        // 将候选事务入队至对应窗口
        for (const auto id : host->pendingTransactionWindows()) {
            auto tx = host->takeWindowTransaction(id);
            if (tx && std::find(frameOrder.begin(), frameOrder.end(), id) != frameOrder.end()) frameWindow(id).pipeline.enqueue(std::move(*tx));
        }

        // 分发诊断气泡与动作（统一路由至最近聚焦的合适常规窗）
        const auto scriptToasts = controlRuntime_.takeDiagnosticToasts();
        const auto actions = controlRuntime_.takeDiagnosticActions();
        for (const auto& action : actions) {
            if (action.kind == quickjs::QuickJsDiagnosticActionKind::RequestReload) reloadRequested_ = true;
        }
        if ((!pendingToasts_.empty() || !scriptToasts.empty()) && !frameOrder.empty()) {
            const auto targetId = selectToastTargetWindow(
                frameOrder,
                [&](core::WindowId id) -> const WindowSurfaceState& { return frameWindow(id); },
                [&](const WindowSurfaceState& win) -> const core::WindowConfigSpec& { return frameConfig(win); });
            if (targetId != 0) {
                auto& target = frameWindow(targetId);
                for (const auto& toast : pendingToasts_) {
                    if (target.diagnostics.notify(toast.level, toast.tag, toast.title, toast.message, toast.coalesce)) {
                        target.pipeline.enqueueIntent(core::InputIntent::diagnostics());
                    }
                }
                for (const auto& toast : scriptToasts) {
                    if (target.diagnostics.notify(toast.level, toast.tag, toast.title, toast.content, toast.coalesce)) {
                        target.pipeline.enqueueIntent(core::InputIntent::diagnostics());
                    }
                }
                pendingToasts_.clear();
            }
        }

        struct PreparedWindow {
            WindowSurfaceState& window;
            std::uint64_t previousRevision;
            std::optional<core::InputSession> input;
            std::optional<InputTextSession> text;
            bool cancelIme = false;
            bool transparencyChanged = false;
        };
        std::vector<PreparedWindow> candidates;
        candidates.reserve(frameOrder.size());
        struct DiscardCandidates {
            std::vector<PreparedWindow>& candidates;
            void discard() {
                for (auto& candidate : candidates) candidate.window.pipeline.discard();
            }
            ~DiscardCandidates() { discard(); }
        } rollback{candidates};
        std::vector<std::pair<core::EventSlotId, core::ScrollResult>> scrollFeedbacks;
        const auto prepareDiagnostics = [&](WindowSurfaceState& win, core::PublishedFrame& frame) {
            const float scale = static_cast<float>(win.dpiScale > 0.0 ? win.dpiScale : 1.0);
            const float topSafe = frameConfig(win).chrome == core::WindowChrome::CustomTitleBar ? 32.0f * scale : 0.0f;
            const core::Rect hudBounds{0.0f, topSafe, static_cast<float>(win.widthPx), std::max(0.0f, static_cast<float>(win.heightPx) - topSafe)};
            win.diagnostics.prepareFrame(hudBounds, options_.detailedErrorScreen, source_.activeSource(), source_.liveUnavailable(), textLayout_, frame.content, scale);
            if (frameConfig(win).transparent) {
                frame.content.diagnosticsBadgeDrawOps.clear();
                frame.content.diagnosticsFpsDrawOps.clear();
                frame.content.diagnosticsToastDrawOps.clear();
            }
            frame.content.errorFrame = std::nullopt;
            prepareDiagnosticsTextOps(frame);
        };

        for (const auto windowId : frameOrder) {
            auto& win = frameWindow(windowId);
            win.pipeline.setFrameTime(nowMillis);
            const bool forceRun = fontChanged || win.pipeline.scene().tree().activeAnimationCount() > 0 || !focusByWindow[windowId].empty();
            if (win.diagnostics.tickToasts(nowMillis)) {
                win.pipeline.enqueueIntent(core::InputIntent::diagnostics());
            }

            auto& candidate = candidates.emplace_back(PreparedWindow{win, win.pipeline.publishedFrame().revision, {}, {}});
            candidate.transparencyChanged = frameConfig(win).transparent != win.config.transparent;
            const core::Constraints constraints{0.0f, static_cast<float>(win.widthPx), 0.0f, static_cast<float>(win.heightPx)};
            const auto finalize = [&](const core::NativeScene& scene, core::PublishedFrame& frame) {
                auto& input = candidate.input.emplace(win.input);
                auto& text = candidate.text.emplace(win.text);
                text.setCancelCompositionHook([&candidate] { candidate.cancelIme = true; });
                input.synchronizePublishedInput(scene.tree(), true, text);
                input.synchronizePublishedFocus(scene.tree(), true, text, focusByWindow[windowId]);
                text.populatePublishedFrame(scene.tree(), true, frame.content);
                prepareDiagnostics(win, frame);
            };

            const core::FramePreparation preparation{
                .materialize = [&](core::NodeId id, const std::vector<int>& indices) {
                    const auto materialized = host->materializeLayout(id, indices);
                    if (!materialized.ok) throw std::runtime_error(materialized.error);
                    auto tx = host->takeWindowTransaction(windowId);
                    if (!tx) throw std::runtime_error("Layout 材料化未提交原生候选");
                    return std::move(*tx);
                },
                .draw = [&](core::NativeScene& candidate) {
                    const auto prepared = host->prepareDrawModifiers(candidate);
                    if (!prepared.ok) throw std::runtime_error(prepared.error);
                },
                .interaction = [&](core::LayoutTree& tree) {
                    win.input.prepareLazyInteraction(tree);
                },
                .positioned = [&](core::NativeScene& candidate, const std::vector<core::PositionedUpdate>& updates) {
                    const auto dispatched = host->dispatchPositioned(windowId, candidate, updates);
                    if (!dispatched.ok) throw std::runtime_error(dispatched.error);
                },
            };

            const auto result = win.pipeline.prepare(win.rootNodeId, constraints, forceRun, finalize, preparation);
            if (!rearrangeSubmission) rearrangeSubmission = result.rearrange;
            if (result.error) {
                rollback.discard();
                if (rearrangeSubmission) (void)host->completeRearrange(rearrangeSubmission, *result.error);
                (void)host->completeVisualFrame(false);
                applyRuntimeFailure(*result.error, RuntimeFrameErrorPhase::Pipeline);
                if (needsFallbackWindow()) {
                    pumpFallbackFrame(nowMillis);
                    changedWindows.push_back(0);
                }
                return changedWindows;
            }
            for (const auto& snapshot : result.scrollUpdates) {
                if (snapshot.eventSlot.valid()) scrollFeedbacks.emplace_back(snapshot.eventSlot, snapshot);
            }
        }

        // 所有候选已通过。先一起发布场景与输入，再让 HWND、IME 与脚本观察到新状态。
        for (auto& candidate : candidates) {
            auto& win = candidate.window;
            win.pipeline.commit();
            if (candidate.input) {
                win.input.commitState(std::move(*candidate.input));
                win.text.commitState(std::move(*candidate.text));
                host->publishWindowScene(win.windowId, win.pipeline.scene());
            } else {
                (void)win.pipeline.publishRetained([&](const core::NativeScene& scene, core::PublishedFrame& frame) {
                    win.text.populatePublishedFrame(scene.tree(), true, frame.content);
                    prepareDiagnostics(win, frame);
                });
            }
        }
        // 已处理的滚动目标以本次发布的场景为准，只保留仍在排队的位移。
        const auto pendingScrollModifiers = controlRuntime_.pendingScrollModifiers();
        for (auto& candidate : candidates) candidate.window.input.retainPendingWheelValues(pendingScrollModifiers);
        commitWindowChanges(appSpec, mutations, newWindows, destroyOrder);
        for (auto& candidate : candidates) {
            auto& win = candidate.window;
            if (candidate.input) {
                if (candidate.cancelIme && options_.platform.onCancelPlatformIme) options_.platform.onCancelPlatformIme(win.windowId);
                win.input.dispatchCommittedFocus(win.pipeline.scene().tree(), win.text, makeInputCallbacks(win.windowId));
            }
            syncWindowImeState(win);
            const auto& published = win.pipeline.publishedFrame();
            const bool publishedChanged = candidate.transparencyChanged || published.revision != candidate.previousRevision;
            const bool appChanged = publishedChanged && !published.changes.appRepaint.empty();
            if (win.diagnostics.recordWindowFrame(appChanged, published.revision > 0, nowMillis)) {
                win.pipeline.enqueueIntent(core::InputIntent::diagnostics());
            }
            bool overlayChanged = false;
            if (auto* hud = win.diagnostics.hud()) {
                const float scale = static_cast<float>(win.dpiScale > 0.0 ? win.dpiScale : 1.0);
                overlayChanged = hud->updateOverlay(win.pipeline.timeline(), published, win.pipeline.scene().tree(), win.rootNodeId, textLayout_, nowMillis, scale);
                win.overlayDirty = false;
            }
            if (publishedChanged || overlayChanged) changedWindows.push_back(win.windowId);
        }
        candidates.clear();

        if (rearrangeSubmission) {
            const auto completed = host->completeRearrange(rearrangeSubmission);
            if (!completed.ok) {
                (void)host->completeVisualFrame(false);
                applyRuntimeFailure(completed.error, RuntimeFrameErrorPhase::Pipeline);
                return changedWindows;
            }
        }
        const auto frameCompleted = host->completeVisualFrame(true);
        if (!frameCompleted.ok) {
            applyRuntimeFailure(frameCompleted.error, RuntimeFrameErrorPhase::Animation);
            return changedWindows;
        }
        for (const auto& [slot, snapshot] : scrollFeedbacks) {
            const auto feedback = host->invokeEventSlot(slot, snapshot);
            if (!feedback.ok) {
                applyRuntimeFailure(feedback.error, RuntimeFrameErrorPhase::Event);
                return changedWindows;
            }
        }
        dispatchWindowCommands();
        if (host->consumeAppExitRequest() || (!mutations.empty() && appConfig_.exitOnLastWindowClosed && everOpenedWindow_ && windows_.empty())) {
            exitRequested_ = true;
            if (options_.platform.onRequestExit) options_.platform.onRequestExit();
        }

        return changedWindows;
    }

    bool AppSession::wantsFrame() const {
        if (exitRequested_) return false;
        if (reloadRequested_ || globalDiagnostics_.hasActiveToasts() || !pendingWindowEvents_.empty() || !pendingToasts_.empty()) return true;
        {
            const std::scoped_lock lock(const_cast<std::mutex&>(bgEventMutex_));
            if (!queuedSecondInstances_.empty() || !queuedMediaButtons_.empty()) return true;
        }
        if (sessionLoaded_ && !globalDiagnostics_.hasError()) {
            if (controlRuntime_.hasPendingEvents() || controlRuntime_.awaitingFontPreload()) return true;
            if (const auto* host = const_cast<CantonRuntime&>(controlRuntime_).scriptHost()) {
                if (host->hasPendingVisualWork() || host->hasPendingFocusCommands() || host->hasPendingWindowWork() || host->hasPendingSemanticWork() || host->hasPendingDiagnostics()) return true;
            }
            if (fonts_ && fonts_->generation() != fontGeneration_) return true;
        }
        for (const auto& [_, win] : windows_) {
            const bool overlayActive = win->overlayDirty || (win->diagnostics.hud() && win->diagnostics.hud()->overlayAnimating());
            if (win->pipeline.hasPendingIntents() || win->pipeline.hasPendingTransactions() || win->pipeline.scene().tree().activeAnimationCount() > 0 || win->wheelSmoother.active() || win->diagnostics.hasActiveToasts() || win->diagnostics.fpsEnabled() || overlayActive) {
                return true;
            }
        }
        return false;
    }

    bool AppSession::ready() const {
        if (!sessionLoaded_ || globalDiagnostics_.hasError() || windows_.empty()) return false;
        for (const auto& [_, win] : windows_) {
            if (win->config.visible && win->pipeline.publishedFrame().revision > 0) return true;
        }
        return false;
    }

    bool AppSession::needsFallbackWindow() const noexcept {
        if (!globalDiagnostics_.hasError()) return false;
        for (const auto& [_, win] : windows_) {
            if (win->config.visible) return false;
        }
        return true;
    }

    core::InputCallbacks AppSession::makeInputCallbacks(core::WindowId windowId) {
        core::InputCallbacks result;
        result.invokeStringEvent = [this](const core::EventSlotId& slot, const std::string& value) {
            if (slot.valid()) controlRuntime_.enqueueStringEvent(slot, value);
        };
        result.invokeFocusEvent = [this](const core::EventSlotId& slot, bool focused, bool hasFocus) {
            if (slot.valid()) controlRuntime_.enqueueFocusEvent(slot, focused, hasFocus);
        };
        result.invokeEvent = [this](const core::EventSlotId& slot) {
            if (slot.valid()) controlRuntime_.enqueueEvent(slot);
        };
        result.invokeScrollEvent = [this](const core::EventSlotId& slot, const core::ScrollResult& scroll) {
            if (slot.valid()) controlRuntime_.enqueueScrollSnapshotEvent(slot, scroll);
        };
        result.dispatchKeyEvent = [this](const core::EventSlotId& slot, const core::KeyEventPayload& payload) {
            return controlRuntime_.dispatchInputEventNow(slot, payload, core::monotonicMillis());
        };
        result.invalidateNativeState = [this, windowId](core::NodeId nodeId, core::DirtyFlag flag) {
            if (auto* win = window(windowId)) {
                core::MutationTransaction tx;
                tx.operations.emplace_back(core::NativeInvalidationMutation{nodeId, flag});
                win->pipeline.enqueueIntent(core::InputIntent::jsCommit(std::move(tx)));
            }
        };
        result.enqueueKeyIntent = [this, windowId](core::NodeId) {
            if (auto* win = window(windowId)) win->pipeline.enqueueIntent(core::InputIntent::key());
        };
        result.enqueueTextInputIntent = [this, windowId](core::NodeId) {
            if (auto* win = window(windowId)) win->pipeline.enqueueIntent(core::InputIntent::textInput());
        };
        result.enqueueImeCompositionIntent = [this, windowId](core::NodeId) {
            if (auto* win = window(windowId)) win->pipeline.enqueueIntent(core::InputIntent::imeComposition());
        };
        result.readClipboard = options_.platform.readClipboard;
        result.writeClipboard = options_.platform.writeClipboard;
        return result;
    }

    void AppSession::emitGestureOutput(WindowSurfaceState& win, const core::PointerGestureOutput& output) {
        win.input.notePointerEffects(output);
        for (const auto& ev : output.events) {
            if (!ev.slot.valid()) continue;
            if (std::holds_alternative<std::monostate>(ev.payload))
                controlRuntime_.enqueueEvent(ev.slot);
            else
                controlRuntime_.enqueueInputEvent(ev.slot, ev.payload);
        }
    }

    void AppSession::syncWindowImeState(WindowSurfaceState& win) {
        const auto& tree = win.pipeline.scene().tree();
        const bool interactive = sessionLoaded_ && !globalDiagnostics_.hasError();
        const bool editable = win.text.isEditableActive(tree, interactive);
        if (win.imeEnabled != editable) {
            win.imeEnabled = editable;
            if (options_.platform.onImeStateChanged) options_.platform.onImeStateChanged(win.windowId, editable);
        }
        if (editable) {
            const auto caret = win.text.caretBoundsInRoot(tree, interactive);
            if (caret && caret != win.lastCaretBounds) {
                win.lastCaretBounds = caret;
                if (options_.platform.onImeCaretChanged) options_.platform.onImeCaretChanged(win.windowId, *caret);
            }
        } else {
            win.lastCaretBounds.reset();
        }
    }

    void AppSession::notifyWindowResized(core::WindowId windowId, int widthPx, int heightPx) {
        auto* win = window(windowId);
        if (!win || widthPx <= 0 || heightPx <= 0) return;
        if (win->widthPx == widthPx && win->heightPx == heightPx) return;
        win->widthPx = widthPx;
        win->heightPx = heightPx;
        win->pipeline.enqueueIntent(core::InputIntent::resize());
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::notifyWindowGeometry(core::WindowId windowId, const core::WindowGeometryEvent& event) {
        auto* win = window(windowId);
        if (!win) return;
        if (event.placement != core::WindowPlacement::Minimized && event.widthPx > 0 && event.heightPx > 0) {
            if (win->widthPx != event.widthPx || win->heightPx != event.heightPx) {
                win->widthPx = event.widthPx;
                win->heightPx = event.heightPx;
                win->pipeline.enqueueIntent(core::InputIntent::resize());
            }
        }
        win->config.placement = event.placement;
        if (sessionLoaded_ && !globalDiagnostics_.hasError()) {
            if (pumpingFrame_) {
                PendingWindowEvent item;
                item.kind = PendingWindowEvent::Kind::Geometry;
                item.windowId = windowId;
                item.geometry = event;
                pendingWindowEvents_.push_back(item);
            } else if (auto* host = controlRuntime_.scriptHost()) {
                const auto res = host->invokeWindowGeometryChanged(windowId, event);
                if (!res.ok) applyRuntimeFailure(res.error, RuntimeFrameErrorPhase::Event);
            }
        }
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::notifyWindowFocus(core::WindowId windowId, bool focused) {
        auto* win = window(windowId);
        if (!win) return;
        win->focused = focused;
        if (focused) lastFocusedWindowId_ = windowId;
        if (!focused && sessionLoaded_ && !globalDiagnostics_.hasError()) {
            auto callbacks = makeInputCallbacks(windowId);
            auto& tree = win->pipeline.scene().tree();
            (void)win->input.keyStateChanged(tree, true, {}, callbacks);
            emitGestureOutput(*win, win->input.cancelPointer(tree, win->text));
            win->wheelSmoother.cancel();
            (void)win->text.cancelComposition(tree, true, callbacks, true);
        } else if (focused) {
            syncWindowImeState(*win);
        }
        if (sessionLoaded_ && !globalDiagnostics_.hasError()) {
            if (pumpingFrame_) {
                PendingWindowEvent item;
                item.kind = PendingWindowEvent::Kind::Focus;
                item.windowId = windowId;
                item.focused = focused;
                pendingWindowEvents_.push_back(item);
            } else if (auto* host = controlRuntime_.scriptHost()) {
                const auto res = host->invokeWindowFocusChanged(windowId, focused);
                if (!res.ok) applyRuntimeFailure(res.error, RuntimeFrameErrorPhase::Event);
            }
        }
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::notifyWindowDpi(core::WindowId windowId, double dpiScale) {
        auto* win = window(windowId);
        if (!win || !std::isfinite(dpiScale) || dpiScale <= 0) return;
        win->dpiScale = dpiScale;
        win->input.setTouchSlop(static_cast<float>(4.0 * dpiScale));
        win->input.setWheelLineDistance(static_cast<float>(core::scrollLineDistance * dpiScale));
        win->lastCaretBounds.reset();
        if (sessionLoaded_ && !globalDiagnostics_.hasError()) {
            if (pumpingFrame_) {
                PendingWindowEvent item;
                item.kind = PendingWindowEvent::Kind::Dpi;
                item.windowId = windowId;
                item.dpiScale = dpiScale;
                pendingWindowEvents_.push_back(item);
            } else if (auto* host = controlRuntime_.scriptHost()) {
                const auto res = host->invokeWindowDpiChanged(windowId, dpiScale);
                if (!res.ok) applyRuntimeFailure(res.error, RuntimeFrameErrorPhase::Event);
            }
        }
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::notifyWindowCloseRequest(core::WindowId windowId) {
        if (!windows_.contains(windowId) || !sessionLoaded_ || globalDiagnostics_.hasError()) return;
        if (pumpingFrame_) {
            PendingWindowEvent item;
            item.kind = PendingWindowEvent::Kind::CloseRequest;
            item.windowId = windowId;
            pendingWindowEvents_.push_back(item);
        } else if (auto* host = controlRuntime_.scriptHost()) {
            const auto res = host->invokeWindowCloseRequest(windowId);
            if (!res.ok) applyRuntimeFailure(res.error, RuntimeFrameErrorPhase::Event);
        }
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::notifyFallbackResized(int widthPx, int heightPx, double dpiScale) {
        if (widthPx <= 0 || heightPx <= 0) return;
        const double validScale = dpiScale > 0.0 ? dpiScale : fallbackDpiScale_;
        if (fallbackWidthPx_ == widthPx && fallbackHeightPx_ == heightPx && std::abs(fallbackDpiScale_ - validScale) < 1e-4) return;
        fallbackWidthPx_ = widthPx;
        fallbackHeightPx_ = heightPx;
        fallbackDpiScale_ = validScale;
        fallbackPipeline_.enqueueIntent(core::InputIntent::diagnostics());
        controlRuntime_.notifyWorkAvailable();
    }

    core::WindowHitResult AppSession::hitTestWindowChrome(core::WindowId windowId, core::Point pointPx, float resizeBorderPx) const {
        const auto* win = window(windowId);
        if (!win) return core::WindowHitResult::Client;
        if (globalDiagnostics_.hasError()) return core::WindowHitResult::Client;
        const auto& hitSnapshot = win->pipeline.publishedFrame().content.hitTest;
        if (!hitSnapshot) return core::WindowHitResult::Client;
        const core::WindowCapabilities caps{
            win->config.resizable,
            win->config.minimizable,
            win->config.maximizable,
            win->config.closable,
            win->config.placement,
            win->config.chrome,
            resizeBorderPx,
        };
        return core::resolveWindowHitTest(*hitSnapshot, {static_cast<float>(win->widthPx), static_cast<float>(win->heightPx)}, pointPx, caps);
    }

    core::PointerIcon AppSession::pointerIcon(core::WindowId windowId) const noexcept {
        const auto* win = window(windowId);
        return win ? win->input.pointerIcon() : core::PointerIcon::Default;
    }

    core::PointerPlatformEffects AppSession::takePointerEffects(core::WindowId windowId) noexcept {
        auto* win = window(windowId);
        return win ? win->input.takePointerEffects() : core::PointerPlatformEffects{};
    }

    void AppSession::pointerMove(core::WindowId windowId, const core::PointerSample& sample) {
        auto* win = window(windowId);
        if (!win) return;
        if (auto* hud = win->diagnostics.hud()) {
            hud->setOverlayPointer(sample.point);
            if (hud->overlayBoundsEnabled()) {
                win->overlayDirty = true;
                controlRuntime_.notifyWorkAvailable();
            }
        }
        if (!sessionLoaded_ || globalDiagnostics_.hasError()) return;
        auto& tree = win->pipeline.scene().tree();
        const auto& hit = win->pipeline.publishedFrame().content.hitTest;
        if (!hit) return;
        if (win->input.focus().target() || win->input.pointerPressed() || win->text.isActive(tree, true)) {
            core::PointerGestureOutput dragOut;
            auto callbacks = makeInputCallbacks(windowId);
            if (win->input.pointerDrag(tree, true, sample, win->text, callbacks, &dragOut) || !dragOut.events.empty() || dragOut.consumed) {
                emitGestureOutput(*win, dragOut);
                syncWindowImeState(*win);
                controlRuntime_.notifyWorkAvailable();
                return;
            }
        }
        emitGestureOutput(*win, win->input.pointerMove(tree, *hit, sample));
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::pointerExit(core::WindowId windowId) {
        auto* win = window(windowId);
        if (!win) return;
        if (auto* hud = win->diagnostics.hud()) {
            hud->setOverlayPointer(std::nullopt);
            if (hud->overlayBoundsEnabled()) {
                win->overlayDirty = true;
                controlRuntime_.notifyWorkAvailable();
            }
        }
        if (!sessionLoaded_ || globalDiagnostics_.hasError()) return;
        emitGestureOutput(*win, win->input.pointerExit(win->pipeline.scene().tree()));
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::pointerDown(core::WindowId windowId, const core::PointerSample& sample, core::PointerDownOptions options) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return;
        const auto& hit = win->pipeline.publishedFrame().content.hitTest;
        if (!hit) return;
        win->wheelSmoother.cancel();
        auto callbacks = makeInputCallbacks(windowId);
        const auto out = win->input.pointerDown(win->pipeline.scene().tree(), *hit, sample, win->text, callbacks, options);
        emitGestureOutput(*win, out);
        syncWindowImeState(*win);
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::pointerUp(core::WindowId windowId, const core::PointerSample& sample) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return;
        const auto& hit = win->pipeline.publishedFrame().content.hitTest;
        if (!hit) return;
        const auto result = win->input.pointerUp(win->pipeline.scene().tree(), *hit, sample, win->text);
        emitGestureOutput(*win, result.gesture);
        syncWindowImeState(*win);
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::contextClick(core::WindowId windowId, const core::PointerSample& sample) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return;
        const auto& hit = win->pipeline.publishedFrame().content.hitTest;
        if (!hit) return;
        emitGestureOutput(*win, win->input.contextClick(win->pipeline.scene().tree(), *hit, sample));
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::cancelPointer(core::WindowId windowId) {
        auto* win = window(windowId);
        if (!win) return;
        win->wheelSmoother.cancel();
        emitGestureOutput(*win, win->input.cancelPointer(win->pipeline.scene().tree(), win->text));
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::dispatchWheelSample(WindowSurfaceState& win, core::Point pointPx, const core::WheelInput& input) {
        const auto& frame = win.pipeline.publishedFrame();
        const auto result = win.input.wheel(win.pipeline.scene().tree(), win.rootNodeId, pointPx, input, 0, frame.content.hitTest.get());
        if (result.wheelEvent && result.wheelEvent->slot.valid()) {
            controlRuntime_.enqueueInputEvent(result.wheelEvent->slot, result.wheelEvent->payload);
        }
        if (result.scroll.consumed && result.scroll.eventSlot.valid()) {
            controlRuntime_.enqueueScrollSnapshotEvent(result.scroll.eventSlot, result.scroll);
        }
    }

    void AppSession::wheel(core::WindowId windowId, core::Point pointPx, const core::WheelInput& input) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return;
        const double nowMillis = input.timeMillis > 0.0 ? input.timeMillis : core::monotonicMillis();
        if (win->wheelSmoother.push(pointPx, input, nowMillis)) {
            controlRuntime_.notifyWorkAvailable();
            return;
        }
        dispatchWheelSample(*win, pointPx, input);
        controlRuntime_.notifyWorkAvailable();
    }

    bool AppSession::keyDown(core::WindowId windowId, const core::KeyInput& key) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return false;
        auto callbacks = makeInputCallbacks(windowId);
        const bool consumed = win->input.keyDown(win->pipeline.scene().tree(), true, key, win->text, callbacks);
        syncWindowImeState(*win);
        controlRuntime_.notifyWorkAvailable();
        return consumed;
    }

    bool AppSession::keyStateChanged(core::WindowId windowId, const std::function<bool(int)>& isDown) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return false;
        auto callbacks = makeInputCallbacks(windowId);
        const bool consumed = win->input.keyStateChanged(win->pipeline.scene().tree(), true, isDown, callbacks);
        controlRuntime_.notifyWorkAvailable();
        return consumed;
    }

    void AppSession::modifierKeysChanged(core::WindowId windowId, core::KeyModifierState modifiers) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return;
        auto callbacks = makeInputCallbacks(windowId);
        win->input.modifierKeysChanged(win->pipeline.scene().tree(), true, modifiers, callbacks);
        controlRuntime_.notifyWorkAvailable();
    }

    bool AppSession::charInput(core::WindowId windowId, std::string_view utf8Text) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return false;
        auto callbacks = makeInputCallbacks(windowId);
        const bool consumed = win->text.insertText(win->pipeline.scene().tree(), true, utf8Text, callbacks);
        if (consumed) {
            syncWindowImeState(*win);
            controlRuntime_.notifyWorkAvailable();
        }
        return consumed;
    }

    bool AppSession::imeComposition(core::WindowId windowId, std::string preedit, std::size_t cursorOffsetBytes, std::vector<core::TextCompositionClause> clauses) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return false;
        auto callbacks = makeInputCallbacks(windowId);
        const bool consumed = win->text.setComposition(win->pipeline.scene().tree(), true, std::move(preedit), cursorOffsetBytes, std::move(clauses), callbacks);
        if (consumed) {
            syncWindowImeState(*win);
            controlRuntime_.notifyWorkAvailable();
        }
        return consumed;
    }

    bool AppSession::imeCommit(core::WindowId windowId, std::string committed) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return false;
        auto callbacks = makeInputCallbacks(windowId);
        const bool consumed = win->text.commitComposition(win->pipeline.scene().tree(), true, std::move(committed), callbacks);
        if (consumed) {
            syncWindowImeState(*win);
            controlRuntime_.notifyWorkAvailable();
        }
        return consumed;
    }

    bool AppSession::imeEndComposition(core::WindowId windowId) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return false;
        if (!win->text.state().composing()) return false;
        auto callbacks = makeInputCallbacks(windowId);
        const bool consumed = win->text.cancelComposition(win->pipeline.scene().tree(), true, callbacks, false);
        if (consumed) {
            syncWindowImeState(*win);
            controlRuntime_.notifyWorkAvailable();
        }
        return consumed;
    }

    void AppSession::setWindowFpsBadgeEnabled(core::WindowId windowId, bool enabled) {
        auto* win = window(windowId);
        if (!win || !win->diagnostics.hud()) return;
        if (win->diagnostics.hud()->setFpsEnabled(enabled)) {
            win->pipeline.enqueueIntent(core::InputIntent::diagnostics());
            controlRuntime_.notifyWorkAvailable();
        }
    }

    bool AppSession::windowFpsBadgeEnabled(core::WindowId windowId) const noexcept {
        const auto* win = window(windowId);
        return win && win->diagnostics.fpsEnabled();
    }

    void AppSession::setWindowFrameTimeEnabled(core::WindowId windowId, bool enabled) {
        auto* win = window(windowId);
        if (!win || !win->diagnostics.hud()) return;
        auto* hud = win->diagnostics.hud();
        hud->setOverlayFrameTimeEnabled(enabled);
        win->pipeline.timeline().setEnabled(hud->overlayNeedsTimeline());
        win->overlayDirty = true;
        controlRuntime_.notifyWorkAvailable();
    }

    bool AppSession::windowFrameTimeEnabled(core::WindowId windowId) const noexcept {
        const auto* win = window(windowId);
        return win && win->diagnostics.hud() && win->diagnostics.hud()->overlayFrameTimeEnabled();
    }

    void AppSession::setWindowDirtyFlashEnabled(core::WindowId windowId, bool enabled) {
        auto* win = window(windowId);
        if (!win || !win->diagnostics.hud()) return;
        auto* hud = win->diagnostics.hud();
        hud->setOverlayDirtyFlashEnabled(enabled);
        win->pipeline.timeline().setEnabled(hud->overlayNeedsTimeline());
        win->overlayDirty = true;
        controlRuntime_.notifyWorkAvailable();
    }

    bool AppSession::windowDirtyFlashEnabled(core::WindowId windowId) const noexcept {
        const auto* win = window(windowId);
        return win && win->diagnostics.hud() && win->diagnostics.hud()->overlayDirtyFlashEnabled();
    }

    void AppSession::setWindowBoundsOverlayEnabled(core::WindowId windowId, bool enabled) {
        auto* win = window(windowId);
        if (!win || !win->diagnostics.hud()) return;
        auto* hud = win->diagnostics.hud();
        hud->setOverlayBoundsEnabled(enabled);
        win->pipeline.timeline().setEnabled(hud->overlayNeedsTimeline());
        win->overlayDirty = true;
        controlRuntime_.notifyWorkAvailable();
    }

    bool AppSession::windowBoundsOverlayEnabled(core::WindowId windowId) const noexcept {
        const auto* win = window(windowId);
        return win && win->diagnostics.hud() && win->diagnostics.hud()->overlayBoundsEnabled();
    }

    std::span<const core::DrawOp> AppSession::windowOverlayOps(core::WindowId windowId) const noexcept {
        const auto* win = window(windowId);
        if (!win || !win->diagnostics.hud()) return {};
        return win->diagnostics.hud()->overlayOps();
    }

    std::vector<core::WindowId> AppSession::windowIds() const {
        return windowOrder_;
    }

    const WindowSurfaceState* AppSession::window(core::WindowId windowId) const noexcept {
        const auto it = windows_.find(windowId);
        return it == windows_.end() ? nullptr : it->second.get();
    }

    WindowSurfaceState* AppSession::window(core::WindowId windowId) noexcept {
        const auto it = windows_.find(windowId);
        return it == windows_.end() ? nullptr : it->second.get();
    }

    void AppSession::cancelAllNetworkRequests() {
        std::unordered_map<std::uint64_t, std::shared_ptr<HttpCancellation>> active;
        {
            const std::scoped_lock lock(netCancelMutex_);
            active = std::exchange(netCancellations_, {});
        }
        for (auto& [_, cancel] : active) {
            if (cancel) cancel->cancel();
        }
    }

    void AppSession::refreshTrayIcon(const std::string& trayId) {
        const auto it = activeTrays_.find(trayId);
        if (it == activeTrays_.end()) return;
        const auto& spec = it->second;
        const auto effective = !spec.icon.empty() ? spec.icon : appConfig_.icon;
        const IconBitmap* iconPtr = nullptr;
        if (!effective.empty()) {
            if (const auto found = iconCache_.find(effective); found != iconCache_.end()) {
                iconPtr = &found->second.smallIcon;
            } else {
                requestIconAsset(effective);
            }
        }
        if (options_.platform.onTrayUpsert) options_.platform.onTrayUpsert(spec, iconPtr);
    }

    void AppSession::refreshWindowTaskbarIcons(core::WindowId windowId) {
        if (const auto badgeIt = windowTaskbarBadges_.find(windowId); badgeIt != windowTaskbarBadges_.end()) {
            if (badgeIt->second) {
                const IconBitmap* iconPtr = nullptr;
                if (!badgeIt->second->icon.empty()) {
                    if (const auto found = iconCache_.find(badgeIt->second->icon); found != iconCache_.end()) {
                        iconPtr = &found->second.smallIcon;
                    } else {
                        requestIconAsset(badgeIt->second->icon);
                    }
                }
                if (options_.platform.onWindowTaskbarBadge) {
                    options_.platform.onWindowTaskbarBadge(windowId, iconPtr, badgeIt->second->label);
                }
            } else {
                if (options_.platform.onWindowTaskbarBadge) {
                    options_.platform.onWindowTaskbarBadge(windowId, nullptr, {});
                }
            }
        }

        if (const auto actIt = windowTaskbarActions_.find(windowId); actIt != windowTaskbarActions_.end()) {
            std::vector<const IconBitmap*> icons;
            icons.reserve(actIt->second.size());
            for (const auto& act : actIt->second) {
                const IconBitmap* iconPtr = nullptr;
                if (!act.icon.empty()) {
                    if (const auto found = iconCache_.find(act.icon); found != iconCache_.end()) {
                        iconPtr = &found->second.smallIcon;
                    } else {
                        requestIconAsset(act.icon);
                    }
                }
                icons.push_back(iconPtr);
            }
            if (options_.platform.onWindowTaskbarActions) {
                options_.platform.onWindowTaskbarActions(windowId, actIt->second, icons);
            }
        }
    }

    void AppSession::refreshMediaSessionThumbnail() {
        std::string diskPath;
        if (activeMediaSession_.active && !activeMediaSession_.thumbnail.empty()) {
            const auto& ref = activeMediaSession_.thumbnail;
            if (fsExists(ref)) {
                diskPath = ref;
            } else if (const auto cached = mediaThumbTempFiles_.find(ref); cached != mediaThumbTempFiles_.end() && fsExists(cached->second)) {
                diskPath = cached->second;
            }
        }
        if (options_.platform.onMediaSessionUpdated) {
            options_.platform.onMediaSessionUpdated(activeMediaSession_, diskPath);
        }
    }

    void AppSession::initPlatformBridge() {
        auto& b = *platformBridge_;

        b.scheduleTimer = [this](double delayMs, bool repeating) -> std::uint64_t {
            return timerQueue_.schedule(core::monotonicMillis(), delayMs, repeating, {});
        };
        b.rescheduleRepeatingTimer = [this](std::uint64_t id) {
            timerQueue_.rescheduleAfterRun(id, core::monotonicMillis());
        };
        b.cancelTimer = [this](std::uint64_t id) {
            (void)timerQueue_.cancel(id);
        };
        b.clearTimers = [this]() {
            timerQueue_.clear();
        };

        b.getGraphicsEnvironment = [this]() -> core::GraphicsEnvironmentInfo {
            return graphicsEnv_;
        };
        b.getLaunchRequest = [this]() -> core::LaunchRequest {
            return launchRequest_;
        };

        b.getKnownPath = [this](std::string_view kind) -> std::string {
            if (kind == "appData") return appPaths_.appData();
            if (kind == "temp") return appPaths_.temp();
            if (kind == "downloads") return AppPaths::downloads();
            if (kind == "documents") return AppPaths::documents();
            if (kind == "desktop") return AppPaths::desktop();
            if (kind == "pictures") return AppPaths::pictures();
            if (kind == "music") return AppPaths::music();
            if (kind == "videos") return AppPaths::videos();
            throw std::invalid_argument("未知的标准目录类型：" + std::string(kind));
        };

        b.fsAsync = [pool = ioPool_](std::string op, std::string path, std::string path2, std::string bytes, bool flag, std::function<void()> wake) -> std::future<quickjs::FsBridgeResult> {
            auto promise = std::make_shared<std::promise<quickjs::FsBridgeResult>>();
            auto future = promise->get_future();
            const bool posted = pool->post([promise, op = std::move(op), path = std::move(path), path2 = std::move(path2), bytes = std::move(bytes), flag, wake = std::move(wake)]() {
                quickjs::FsBridgeResult out;
                try {
                    if (op == "readText") {
                        out.bytes = fsReadText(path);
                    } else if (op == "readBytes") {
                        out.bytes = fsReadBytes(path);
                    } else if (op == "writeText") {
                        fsWriteText(path, bytes, flag);
                    } else if (op == "writeBytes") {
                        fsWriteBytes(path, bytes, flag);
                    } else if (op == "exists") {
                        out.boolValue = fsExists(path);
                    } else if (op == "stat") {
                        const auto st = fsStat(path);
                        out.isFile = st.isFile;
                        out.isDirectory = st.isDirectory;
                        out.size = st.size;
                        out.modifiedTimeMs = st.modifiedTimeMs;
                        out.createdTimeMs = st.createdTimeMs;
                    } else if (op == "readDir") {
                        for (const auto& entry : fsReadDir(path)) {
                            out.entries.push_back({entry.name, entry.isFile, entry.isDirectory});
                        }
                    } else if (op == "mkdir") {
                        fsMkdir(path, flag);
                    } else if (op == "remove") {
                        fsRemove(path, flag);
                    } else if (op == "rename") {
                        fsRename(path, path2);
                    } else if (op == "copy") {
                        fsCopy(path, path2, flag);
                    } else {
                        throw FsError(FsErrorCode::Io, path, "未知的文件操作：" + op);
                    }
                } catch (const FsError& err) {
                    out.ok = false;
                    out.errorCode = fsErrorCodeName(err.code());
                    out.errorPath = err.path();
                    out.errorMessage = err.what();
                } catch (const std::exception& err) {
                    out.ok = false;
                    out.errorCode = "IO";
                    out.errorPath = path;
                    out.errorMessage = err.what();
                }
                promise->set_value(std::move(out));
                if (wake) wake();
            });
            if (!posted) {
                quickjs::FsBridgeResult err;
                err.ok = false;
                err.errorCode = "IO";
                err.errorPath = {};
                err.errorMessage = "后台 I/O 线程池已关闭";
                promise->set_value(std::move(err));
            }
            return future;
        };

        b.netRequestAsync = [this, pool = ioPool_](std::uint64_t reqId, quickjs::NetBridgeRequest req, std::function<void()> wake) -> std::future<quickjs::NetBridgeResponse> {
            auto cancel = std::make_shared<HttpCancellation>();
            {
                const std::scoped_lock lock(netCancelMutex_);
                netCancellations_[reqId] = cancel;
            }
            auto promise = std::make_shared<std::promise<quickjs::NetBridgeResponse>>();
            auto future = promise->get_future();
            const bool posted = pool->post([this, reqId, cancel, promise, req = std::move(req), wake = std::move(wake)]() {
                quickjs::NetBridgeResponse out;
                try {
                    HttpRequestOptions opts;
                    opts.method = req.method;
                    opts.url = req.url;
                    opts.headers = req.headers;
                    opts.body = req.body;
                    opts.timeoutMs = req.timeoutMs;
                    opts.maxBytes = req.maxBytes;
                    opts.followRedirects = req.followRedirects;
                    opts.maxRedirects = req.maxRedirects;
                    opts.useSystemProxy = req.useSystemProxy;
                    auto resp = httpRequest(opts, cancel.get());
                    out.status = resp.status;
                    out.finalUrl = std::move(resp.url);
                    out.contentType = std::move(resp.contentType);
                    for (auto& [k, v] : resp.headers) out.headers.emplace_back(k, std::move(v));
                    out.body = std::move(resp.body);
                } catch (const NetError& err) {
                    out.ok = false;
                    out.errorCode = netErrorCodeName(err.code());
                    out.errorUrl = err.url();
                    out.errorMessage = err.what();
                } catch (const std::exception& err) {
                    out.ok = false;
                    out.errorCode = "NETWORK";
                    out.errorUrl = req.url;
                    out.errorMessage = err.what();
                }
                {
                    const std::scoped_lock lock(netCancelMutex_);
                    netCancellations_.erase(reqId);
                }
                promise->set_value(std::move(out));
                if (wake) wake();
            });
            if (!posted) {
                const std::scoped_lock lock(netCancelMutex_);
                netCancellations_.erase(reqId);
                quickjs::NetBridgeResponse err;
                err.ok = false;
                err.errorCode = "ABORTED";
                err.errorMessage = "后台 I/O 线程池已关闭";
                promise->set_value(std::move(err));
            }
            return future;
        };

        b.netDownloadAsync = [this, pool = ioPool_](
                                 std::uint64_t reqId,
                                 quickjs::NetBridgeRequest req,
                                 std::string destPath,
                                 std::function<void(quickjs::NetBridgeProgress)> onProgress,
                                 std::function<void()> wake) -> std::future<quickjs::NetBridgeResponse> {
            auto cancel = std::make_shared<HttpCancellation>();
            {
                const std::scoped_lock lock(netCancelMutex_);
                netCancellations_[reqId] = cancel;
            }
            auto promise = std::make_shared<std::promise<quickjs::NetBridgeResponse>>();
            auto future = promise->get_future();
            const bool posted = pool->post([this, reqId, cancel, promise, req = std::move(req), destPath = std::move(destPath), onProgress = std::move(onProgress), wake = std::move(wake)]() {
                quickjs::NetBridgeResponse out;
                try {
                    HttpRequestOptions opts;
                    opts.method = "GET";
                    opts.url = req.url;
                    opts.headers = req.headers;
                    opts.timeoutMs = req.timeoutMs;
                    opts.maxBytes = req.maxBytes;
                    opts.followRedirects = req.followRedirects;
                    opts.maxRedirects = req.maxRedirects;
                    opts.useSystemProxy = req.useSystemProxy;
                    auto res = httpDownloadFile(
                        opts,
                        destPath,
                        [&](const DownloadProgress& p) {
                            if (onProgress) onProgress({p.receivedBytes, p.totalBytes});
                        },
                        cancel.get());
                    out.status = res.status;
                    out.downloadOk = res.ok;
                    out.finalUrl = std::move(res.url);
                    out.destPath = std::move(res.destPath);
                    out.bytesWritten = res.bytesWritten;
                } catch (const NetError& err) {
                    out.ok = false;
                    out.errorCode = netErrorCodeName(err.code());
                    out.errorUrl = err.url();
                    out.errorMessage = err.what();
                } catch (const std::exception& err) {
                    out.ok = false;
                    out.errorCode = "NETWORK";
                    out.errorUrl = req.url;
                    out.errorMessage = err.what();
                }
                {
                    const std::scoped_lock lock(netCancelMutex_);
                    netCancellations_.erase(reqId);
                }
                promise->set_value(std::move(out));
                if (wake) wake();
            });
            if (!posted) {
                const std::scoped_lock lock(netCancelMutex_);
                netCancellations_.erase(reqId);
                quickjs::NetBridgeResponse err;
                err.ok = false;
                err.errorCode = "ABORTED";
                err.errorMessage = "后台 I/O 线程池已关闭";
                promise->set_value(std::move(err));
            }
            return future;
        };

        b.netCancel = [this](std::uint64_t reqId) {
            std::shared_ptr<HttpCancellation> cancel;
            {
                const std::scoped_lock lock(netCancelMutex_);
                if (const auto it = netCancellations_.find(reqId); it != netCancellations_.end()) cancel = it->second;
            }
            if (cancel) cancel->cancel();
        };

        b.upsertTray = [this](const core::TrayIconSpec& spec) {
            activeTrays_[spec.id] = spec;
            refreshTrayIcon(spec.id);
        };
        b.removeTray = [this](const std::string& trayId) {
            activeTrays_.erase(trayId);
            if (options_.platform.onTrayRemove) options_.platform.onTrayRemove(trayId);
        };
        b.showSystemTrayMenu = [this](const std::vector<core::TrayMenuItemSpec>& items, int screenX, int screenY) -> std::uint32_t {
            return options_.platform.onTrayShowSystemMenu ? options_.platform.onTrayShowSystemMenu(items, screenX, screenY) : 0u;
        };
        b.queryTrayAnchor = [this](const std::string& trayId) -> core::TrayAnchorRect {
            return options_.platform.onTrayQueryAnchor ? options_.platform.onTrayQueryAnchor(trayId) : core::TrayAnchorRect{};
        };

        b.setWindowTaskbarProgress = [this](core::WindowId windowId, const core::TaskbarProgressSpec& spec) {
            if (options_.platform.onWindowTaskbarProgress) options_.platform.onWindowTaskbarProgress(windowId, spec);
        };
        b.setWindowTaskbarBadge = [this](core::WindowId windowId, const std::optional<core::TaskbarBadgeSpec>& spec) {
            windowTaskbarBadges_[windowId] = spec;
            refreshWindowTaskbarIcons(windowId);
        };
        b.setWindowTaskbarActions = [this](core::WindowId windowId, const std::vector<core::TaskbarActionSpec>& actions) {
            windowTaskbarActions_[windowId] = actions;
            refreshWindowTaskbarIcons(windowId);
        };

        b.updateMediaSession = [this](const core::MediaSessionSpec& spec) {
            activeMediaSession_ = spec;
            refreshMediaSessionThumbnail();
        };
        b.setJumpList = [this](const std::vector<core::JumpListCategorySpec>& categories) {
            if (options_.platform.onSetJumpList) {
                const auto appId = !appConfig_.appId.empty() ? appConfig_.appId : options_.pathsConfig.appId;
                options_.platform.onSetJumpList(appId, categories);
            }
        };
        b.clearJumpList = [this]() {
            if (options_.platform.onClearJumpList) {
                const auto appId = !appConfig_.appId.empty() ? appConfig_.appId : options_.pathsConfig.appId;
                options_.platform.onClearJumpList(appId);
            }
        };
    }

    std::optional<std::uint32_t> AppSession::nextTimerWaitMillis(double nowMillis) const noexcept {
        if (exitRequested_) return std::nullopt;
        return timerQueue_.nextWaitMillis(nowMillis);
    }

    void AppSession::pumpTimers(double nowMillis) {
        if (exitRequested_ || !sessionLoaded_ || globalDiagnostics_.hasError()) return;
        auto* host = controlRuntime_.scriptHost();
        if (!host) return;
        const auto due = timerQueue_.popDue(nowMillis);
        for (const auto& item : due) {
            const auto res = host->invokeTimer(item.id);
            if (!res.ok) {
                applyRuntimeFailure(res.error, RuntimeFrameErrorPhase::Event);
                return;
            }
        }
    }

    void AppSession::updateGraphicsEnvironment(const core::GraphicsEnvironmentInfo& env) {
        if (graphicsEnv_ == env) return;
        graphicsEnv_ = env;
        if (!sessionLoaded_ || globalDiagnostics_.hasError()) return;
        if (auto* host = controlRuntime_.scriptHost()) {
            const auto res = host->notifyGraphicsEnvironmentChanged();
            if (!res.ok) {
                applyRuntimeFailure(res.error, RuntimeFrameErrorPhase::Event);
            } else {
                controlRuntime_.notifyWorkAvailable();
            }
        }
    }

    void AppSession::notifySecondInstance(core::LaunchRequest request) {
        {
            const std::scoped_lock lock(bgEventMutex_);
            queuedSecondInstances_.push_back(std::move(request));
        }
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::notifyTrayEvent(const core::TrayEvent& event) {
        if (!sessionLoaded_ || globalDiagnostics_.hasError()) return;
        if (auto* host = controlRuntime_.scriptHost()) {
            const auto res = host->notifyTrayEvent(event);
            if (!res.ok) {
                applyRuntimeFailure(res.error, RuntimeFrameErrorPhase::Event);
            } else {
                controlRuntime_.notifyWorkAvailable();
            }
        }
    }

    void AppSession::notifyTaskbarActionClicked(core::WindowId windowId, const std::string& actionId) {
        if (!sessionLoaded_ || globalDiagnostics_.hasError()) return;
        if (auto* host = controlRuntime_.scriptHost()) {
            const auto res = host->notifyTaskbarActionClicked(windowId, actionId);
            if (!res.ok) {
                applyRuntimeFailure(res.error, RuntimeFrameErrorPhase::Event);
            } else {
                controlRuntime_.notifyWorkAvailable();
            }
        }
    }

    void AppSession::notifyMediaButtonPressed(core::MediaButtonKind button) {
        {
            const std::scoped_lock lock(bgEventMutex_);
            queuedMediaButtons_.push_back(button);
        }
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::drainBackgroundEvents() {
        if (!sessionLoaded_ || globalDiagnostics_.hasError()) return;
        auto* host = controlRuntime_.scriptHost();
        if (!host) return;

        std::vector<core::LaunchRequest> secondInstances;
        std::vector<core::MediaButtonKind> mediaButtons;
        {
            const std::scoped_lock lock(bgEventMutex_);
            secondInstances = std::exchange(queuedSecondInstances_, {});
            mediaButtons = std::exchange(queuedMediaButtons_, {});
        }
        for (const auto& req : secondInstances) {
            const auto res = host->notifySecondInstance(req);
            if (!res.ok) {
                applyRuntimeFailure(res.error, RuntimeFrameErrorPhase::Event);
                return;
            }
        }
        for (const auto btn : mediaButtons) {
            const auto res = host->notifyMediaButtonPressed(btn);
            if (!res.ok) {
                applyRuntimeFailure(res.error, RuntimeFrameErrorPhase::Event);
                return;
            }
        }
    }
}
