#pragma once

#include <core/InputSession.h>
#include <core/PlatformTypes.h>
#include <core/TextLayoutService.h>
#include <core/WheelSmoother.h>
#include <core/WindowTypes.h>
#include <runtime/AppPaths.h>
#include <runtime/CantonRuntime.h>
#include <runtime/DiagnosticsState.h>
#include <runtime/HttpClient.h>
#include <runtime/InputTextSession.h>
#include <runtime/IoThreadPool.h>
#include <runtime/PackageRuntimeSource.h>
#include <runtime/ScenePipelineState.h>
#include <runtime/TimerQueue.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace canton::runtime {

    class ResourceWorkers;

    struct IconBitmap {
        int width = 0;
        int height = 0;
        // 预乘 0xAARRGGBB（小端内存即 BGRA）
        std::vector<std::uint32_t> pixels;
    };

    struct AppPlatformCallbacks {
        std::function<void(const core::AppConfigSpec&)> onAppConfigChanged;
        std::function<void(core::WindowId, const core::WindowConfigSpec&)> onWindowCreated;
        std::function<void(core::WindowId, const core::WindowConfigSpec&)> onWindowUpdated;
        // 同一重排销毁父子窗时，AppSession 保证按拓扑逆序（先 Owned 子孙、后 Owner）逐个调用
        std::function<void(core::WindowId)> onWindowDestroyed;
        std::function<void(core::WindowId, core::WindowCommandKind)> onWindowCommand;
        std::function<void(core::WindowId, const IconBitmap* bigIcon, const IconBitmap* smallIcon)> onWindowIconChanged;
        std::function<void(core::WindowId, bool editable)> onImeStateChanged;
        std::function<void(core::WindowId, core::Rect caretPx)> onImeCaretChanged;
        std::function<void(core::WindowId)> onCancelPlatformIme;
        std::function<std::string()> readClipboard;
        std::function<void(const std::string&)> writeClipboard;
        std::function<void()> onRequestExit;

        // M6 桌面融合与系统能力回调（由 HostMain 绑定至 win32）
        std::function<void(const core::TrayIconSpec&, const IconBitmap* icon)> onTrayUpsert;
        std::function<void(const std::string& trayId)> onTrayRemove;
        std::function<void()> onTrayClearAll;
        std::function<std::uint32_t(const std::vector<core::TrayMenuItemSpec>& items, int screenX, int screenY)> onTrayShowSystemMenu;
        std::function<core::TrayAnchorRect(const std::string& trayId)> onTrayQueryAnchor;
        std::function<void(core::WindowId, const core::TaskbarProgressSpec&)> onWindowTaskbarProgress;
        std::function<void(core::WindowId, const IconBitmap* icon, const std::string& label)> onWindowTaskbarBadge;
        std::function<void(core::WindowId, const std::vector<core::TaskbarActionSpec>&, const std::vector<const IconBitmap*>& icons)> onWindowTaskbarActions;
        std::function<void(const core::MediaSessionSpec&, const std::string& thumbnailDiskPath)> onMediaSessionUpdated;
        std::function<void(const std::string& appId, const std::vector<core::JumpListCategorySpec>&)> onSetJumpList;
        std::function<void(const std::string& appId)> onClearJumpList;
    };

    struct AppSessionOptions {
        std::function<std::unique_ptr<DiagnosticsHud>()> hudFactory;
        bool detailedErrorScreen = false;
        std::vector<std::string> diagnosticsFontFamily;
        PackageRuntimeSource::LiveSessionFactory live;
        RuntimePackageLoader::DistResources distResources;
        AppResolver::MountPackage mountPackage;
        std::shared_ptr<const core::NativePainterRegistry> painters;
        std::shared_ptr<const core::ShaderCompiler> shaderCompiler;
        core::GraphicsEnvironmentInfo initialGraphicsEnv;
        core::LaunchRequest launchRequest;
        AppPathsConfig pathsConfig;
        AppPlatformCallbacks platform;
    };

    struct WindowSurfaceState {
        core::WindowId windowId = 0;
        core::NodeId rootNodeId = 0;
        core::WindowConfigSpec config;
        std::string appliedIconPath;
        ScenePipelineState pipeline;
        DiagnosticsState diagnostics;
        core::InputSession input;
        core::WheelSmoother wheelSmoother;
        InputTextSession text;
        int widthPx = 960;
        int heightPx = 640;
        double dpiScale = 1.0;
        bool focused = false;
        bool imeEnabled = false;
        std::optional<core::Rect> lastCaretBounds;
        bool overlayDirty = false;

        explicit WindowSurfaceState(const core::TextLayoutService& textLayout)
            : pipeline(core::SceneFramePipeline(core::LayoutEngine(textLayout))), text(textLayout) {}
    };

    // 1 VM : N 窗口会话的运行时总编排：单 QuickJsScriptHost，每窗独立 NativeScene / InputSession / InputTextSession / DiagnosticsHud，
    // 支持同帧切窗不误退、拓扑逆序毁窗、0 窗错误屏兜底、Live 热更保持已有窗口会话与整体重载清理。
    class AppSession final {
       public:
        AppSession(const core::TextMeasurer& measurer, std::shared_ptr<core::FontRegistry> fonts, AppSessionOptions options);
        ~AppSession();
        AppSession(const AppSession&) = delete;
        AppSession& operator=(const AppSession&) = delete;

        void setWorkAvailable(std::function<void()> callback);
        void configure(AppSourceConfig config);
        void setStartupError(const std::string& message);
        void requestReload() noexcept;
        void notifyToast(core::LogLevel level, std::string_view tag, std::string title, std::string message = {}, bool coalesce = true);
        void triggerSimulatedError(std::string summary, std::string detail = {});
        // 无条件有序卸载（用于 exitApplication 与 WM_ENDSESSION），不触发各窗口 onCloseRequest
        void shutdownSession();

        // 推进一帧；返回有发布帧变化的窗口列表（0 表示 0 窗兜底错误屏变化）
        std::vector<core::WindowId> pumpFrame(double nowMillis);
        [[nodiscard]] bool wantsFrame() const;
        [[nodiscard]] bool ready() const;
        [[nodiscard]] bool exitRequested() const noexcept { return exitRequested_; }
        // 当前存在错误且没有任何可见应用窗口时，需要宿主展示 0 窗兜底错误屏
        [[nodiscard]] bool needsFallbackWindow() const noexcept;

        // 窗口平台事件输入（均在 UI Owner 线程调用）
        void notifyWindowResized(core::WindowId windowId, int widthPx, int heightPx);
        void notifyWindowGeometry(core::WindowId windowId, const core::WindowGeometryEvent& event);
        void notifyWindowFocus(core::WindowId windowId, bool focused);
        void notifyWindowDpi(core::WindowId windowId, double dpiScale);
        void notifyWindowCloseRequest(core::WindowId windowId);
        void notifyFallbackResized(int widthPx, int heightPx, double dpiScale = 1.0);

        // 定时器、图形环境、单实例热唤醒、托盘、任务栏按钮与媒体键入口
        [[nodiscard]] std::optional<std::uint32_t> nextTimerWaitMillis(double nowMillis) const noexcept;
        void pumpTimers(double nowMillis);
        void updateGraphicsEnvironment(const core::GraphicsEnvironmentInfo& env);
        // 线程安全：可由单实例 IPC 后台线程调用
        void notifySecondInstance(core::LaunchRequest request);
        void notifyTrayEvent(const core::TrayEvent& event);
        void notifyTaskbarActionClicked(core::WindowId windowId, const std::string& actionId);
        // 线程安全：可由 WinRT SMTC 线程调用
        void notifyMediaButtonPressed(core::MediaButtonKind button);
        [[nodiscard]] const AppPaths& appPaths() const noexcept { return appPaths_; }

        [[nodiscard]] core::WindowHitResult hitTestWindowChrome(core::WindowId windowId, core::Point pointPx, float resizeBorderPx) const;
        [[nodiscard]] core::PointerIcon pointerIcon(core::WindowId windowId) const noexcept;
        [[nodiscard]] core::PointerPlatformEffects takePointerEffects(core::WindowId windowId) noexcept;

        void pointerMove(core::WindowId windowId, const core::PointerSample& sample);
        void pointerExit(core::WindowId windowId);
        void pointerDown(core::WindowId windowId, const core::PointerSample& sample, core::PointerDownOptions options = {});
        void pointerUp(core::WindowId windowId, const core::PointerSample& sample);
        void contextClick(core::WindowId windowId, const core::PointerSample& sample);
        void cancelPointer(core::WindowId windowId);
        void wheel(core::WindowId windowId, core::Point pointPx, const core::WheelInput& input);

        bool keyDown(core::WindowId windowId, const core::KeyInput& key);
        bool keyStateChanged(core::WindowId windowId, const std::function<bool(int)>& isDown);
        void modifierKeysChanged(core::WindowId windowId, core::KeyModifierState modifiers);
        bool charInput(core::WindowId windowId, std::string_view utf8Text);

        bool imeComposition(core::WindowId windowId, std::string preedit, std::size_t cursorOffsetBytes, std::vector<core::TextCompositionClause> clauses);
        bool imeCommit(core::WindowId windowId, std::string committed);
        bool imeEndComposition(core::WindowId windowId);

        void setWindowFpsBadgeEnabled(core::WindowId windowId, bool enabled);
        [[nodiscard]] bool windowFpsBadgeEnabled(core::WindowId windowId) const noexcept;
        void setWindowFrameTimeEnabled(core::WindowId windowId, bool enabled);
        [[nodiscard]] bool windowFrameTimeEnabled(core::WindowId windowId) const noexcept;
        void setWindowDirtyFlashEnabled(core::WindowId windowId, bool enabled);
        [[nodiscard]] bool windowDirtyFlashEnabled(core::WindowId windowId) const noexcept;
        void setWindowBoundsOverlayEnabled(core::WindowId windowId, bool enabled);
        [[nodiscard]] bool windowBoundsOverlayEnabled(core::WindowId windowId) const noexcept;
        [[nodiscard]] std::span<const core::DrawOp> windowOverlayOps(core::WindowId windowId) const noexcept;

        [[nodiscard]] std::vector<core::WindowId> windowIds() const;
        [[nodiscard]] const WindowSurfaceState* window(core::WindowId windowId) const noexcept;
        [[nodiscard]] WindowSurfaceState* window(core::WindowId windowId) noexcept;
        [[nodiscard]] const core::PublishedFrame& fallbackPublishedFrame() const noexcept {
            return fallbackPipeline_.publishedFrame();
        }
        [[nodiscard]] const DiagnosticsState& globalDiagnostics() const noexcept {
            return globalDiagnostics_;
        }
        [[nodiscard]] const PackageRuntimeSource& source() const noexcept {
            return source_;
        }
        [[nodiscard]] const core::TextLayoutService& textLayout() const noexcept {
            return textLayout_;
        }
        [[nodiscard]] const core::AppConfigSpec& appConfig() const noexcept {
            return appConfig_;
        }

        // 按 Win32 owner 依赖对一批待销毁窗口排拓扑逆序（Owned 子孙严格排在 Owner 之前）
        static std::vector<core::WindowId> sortDestroyOrder(const std::vector<core::WindowId>& toDestroy, const std::unordered_map<core::WindowId, std::optional<core::WindowId>>& owners);

       private:
        struct DecodedIcons {
            IconBitmap bigIcon;
            IconBitmap smallIcon;
        };

        void applyLoadOutcome(PackageLoadOutcome outcome);
        void teardownAllWindows();
        [[nodiscard]] std::unique_ptr<WindowSurfaceState> makeWindowSurface(const quickjs::WindowMutation& mutation);
        void commitWindowChanges(const std::optional<core::AppConfigSpec>& appSpec,
            const std::vector<quickjs::WindowMutation>& mutations,
            std::unordered_map<core::WindowId, std::unique_ptr<WindowSurfaceState>>& newWindows,
            const std::vector<core::WindowId>& destroyOrder);
        void dispatchWindowCommands();
        void applyRuntimeFailure(const std::string& message, RuntimeFrameErrorPhase phase);
        core::InputCallbacks makeInputCallbacks(core::WindowId windowId);
        void emitGestureOutput(WindowSurfaceState& win, const core::PointerGestureOutput& output);
        void dispatchWheelSample(WindowSurfaceState& win, core::Point pointPx, const core::WheelInput& input);
        void syncWindowImeState(WindowSurfaceState& win);
        void prepareDiagnosticsTextOps(core::PublishedFrame& frame) const;
        void pumpFallbackFrame(double nowMillis);
        void requestIconIfNeeded(WindowSurfaceState& win);
        void requestIconAsset(const std::string& path);
        void pumpPendingIcons();
        void initPlatformBridge();
        void drainBackgroundEvents();
        void refreshTrayIcon(const std::string& trayId);
        void refreshWindowTaskbarIcons(core::WindowId windowId);
        void refreshMediaSessionThumbnail();
        void cancelAllNetworkRequests();

        std::shared_ptr<core::FontRegistry> fonts_;
        core::TextLayoutService textLayout_;
        AppSessionOptions options_;
        std::shared_ptr<ResourceWorkers> resourceWorkers_;
        std::shared_ptr<IoThreadPool> ioPool_;
        AppPaths appPaths_;
        TimerQueue timerQueue_;
        core::GraphicsEnvironmentInfo graphicsEnv_;
        core::LaunchRequest launchRequest_;
        std::shared_ptr<quickjs::PlatformBridge> platformBridge_;
        CantonRuntime controlRuntime_;
        DiagnosticsState globalDiagnostics_;
        PackageRuntimeSource source_;
        core::PainterLoader activePainterLoader_;

        core::AppConfigSpec appConfig_;
        bool sessionLoaded_ = false;
        bool everOpenedWindow_ = false;
        bool exitRequested_ = false;
        bool reloadRequested_ = false;
        std::uint64_t fontGeneration_ = 0;

        struct PendingWindowEvent {
            enum class Kind { Geometry, Focus, Dpi, CloseRequest };
            Kind kind = Kind::Geometry;
            core::WindowId windowId = 0;
            core::WindowGeometryEvent geometry;
            bool focused = false;
            double dpiScale = 1.0;
        };

        void drainPendingWindowEvents();

        struct QueuedToast {
            core::LogLevel level = core::LogLevel::Info;
            std::string tag;
            std::string title;
            std::string message;
            bool coalesce = true;
        };

        void enqueueMirroredToast(core::LogLevel level, std::string_view tag, std::string title, std::string message, bool coalesce);
        [[nodiscard]] core::WindowId selectToastTargetWindow(const std::vector<core::WindowId>& order,
            const std::function<const WindowSurfaceState&(core::WindowId)>& resolveWindow,
            const std::function<const core::WindowConfigSpec&(const WindowSurfaceState&)>& resolveConfig) const;
        [[nodiscard]] core::WindowId selectPrimaryErrorWindowId() const;

        bool pumpingFrame_ = false;
        std::vector<PendingWindowEvent> pendingWindowEvents_;
        std::vector<QueuedToast> pendingToasts_;
        core::WindowId lastFocusedWindowId_ = 0;

        std::vector<core::WindowId> windowOrder_;
        std::unordered_map<core::WindowId, std::unique_ptr<WindowSurfaceState>> windows_;

        ScenePipelineState fallbackPipeline_;
        int fallbackWidthPx_ = 960;
        int fallbackHeightPx_ = 640;
        double fallbackDpiScale_ = 1.0;

        std::unordered_map<std::string, DecodedIcons> iconCache_;
        struct PendingIcon {
            std::string path;
            std::future<core::PainterLoadResult> future;
        };
        std::vector<PendingIcon> pendingIcons_;

        std::mutex netCancelMutex_;
        std::unordered_map<std::uint64_t, std::shared_ptr<HttpCancellation>> netCancellations_;

        std::mutex bgEventMutex_;
        std::vector<core::LaunchRequest> queuedSecondInstances_;
        std::vector<core::MediaButtonKind> queuedMediaButtons_;

        std::unordered_map<std::string, core::TrayIconSpec> activeTrays_;
        std::unordered_map<core::WindowId, std::optional<core::TaskbarBadgeSpec>> windowTaskbarBadges_;
        std::unordered_map<core::WindowId, std::vector<core::TaskbarActionSpec>> windowTaskbarActions_;
        core::MediaSessionSpec activeMediaSession_;
        std::unordered_map<std::string, std::string> mediaThumbTempFiles_;
    };

}
