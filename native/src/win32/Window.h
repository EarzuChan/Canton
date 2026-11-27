#pragma once

#include <core/InputEditing.h>
#include <core/InputSession.h>
#include <core/PlatformTypes.h>
#include <core/PointerGesture.h>
#include <core/WheelInput.h>
#include <core/WindowTypes.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace canton::win32 {

    // 绑定进程级显式 AppUserModelID（AUMID），须在首个窗口显示前调用以归并任务栏身份
    void setProcessAppId(std::string_view appId);

    // 系统剪贴板 UTF-8 文本读写（内部经 CF_UNICODETEXT 与 CRLF 归一化）
    [[nodiscard]] std::string readClipboardText();
    void writeClipboardText(std::string_view utf8Text);

    // 32 位预乘 BGRA 图标句柄 RAII 包装
    class IconHandle {
       public:
        IconHandle() = default;
        explicit IconHandle(void* handle) noexcept : handle_(handle) {}
        ~IconHandle();
        IconHandle(IconHandle&& other) noexcept;
        IconHandle& operator=(IconHandle&& other) noexcept;
        IconHandle(const IconHandle&) = delete;
        IconHandle& operator=(const IconHandle&) = delete;

        [[nodiscard]] void* get() const noexcept { return handle_; }
        [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

        static IconHandle fromPbgra(const std::uint32_t* pixels, int width, int height);

       private:
        void* handle_ = nullptr;
    };

    // 系统级图形偏好与会话环境（SM_REMOTESESSION / EnableTransparency / SPI_GETCLIENTAREAANIMATION）
    struct SystemGraphicsPreferences {
        bool isRemoteSession = false;
        bool systemTransparency = true;
        bool systemAnimations = true;
    };
    [[nodiscard]] SystemGraphicsPreferences querySystemGraphicsPreferences() noexcept;

    struct MessageDispatcherCallbacks {
        std::function<void()> wake;
        std::function<void()> fontsChanged;
        std::function<void()> settingsChanged;
        std::function<void()> taskbarCreated;
        std::function<void(std::uint32_t iconId, std::uint32_t mouseMsg, int cursorX, int cursorY)> trayMessage;
        std::function<void()> endSession;
    };

    // App 级隐藏顶层 HWND（WS_EX_TOOLWINDOW，非 HWND_MESSAGE）：承接托盘、SMTC、关机通知、字体/系统设置变更与定时唤醒
    class MessageDispatcher {
       public:
        static constexpr std::uint32_t kTrayCallbackMessage = 0x8002u;  // WM_APP + 2

        explicit MessageDispatcher(MessageDispatcherCallbacks callbacks);
        ~MessageDispatcher();
        MessageDispatcher(const MessageDispatcher&) = delete;
        MessageDispatcher& operator=(const MessageDispatcher&) = delete;

        [[nodiscard]] void* hwnd() const noexcept;
        void postWake() noexcept;
        static int runMessageLoop(
            std::function<std::optional<std::uint32_t>()> nextTimeoutMs = {},
            std::function<void()> onTimerWake = {});
        static void quitMessageLoop(int exitCode = 0) noexcept;

       private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    struct WindowCallbacks {
        std::function<void(int widthPx, int heightPx)> resized;
        std::function<void(const core::WindowGeometryEvent&)> geometryChanged;
        std::function<void(bool focused)> focusChanged;
        std::function<void(double dpiScale)> dpiChanged;
        std::function<void()> closeRequested;
        std::function<void()> paint;
        std::function<void()> wake;
        std::function<void()> fontsChanged;
        std::function<void()> endSession;

        // 自绘标题栏与窗口非客户区 5 级命中查询（输入为窗口客户区物理像素坐标与当前 DPI 缩放边框厚度）
        std::function<core::WindowHitResult(core::Point pointPx, float resizeBorderPx)> hitTest;
        std::function<core::PointerIcon()> pointerIcon;

        // 指针、滚轮、键盘、字符与纯 IMM32 输入回调
        std::function<void(const core::PointerSample&)> pointerMove;
        std::function<void()> pointerExit;
        std::function<void(const core::PointerSample&, core::PointerDownOptions)> pointerDown;
        std::function<void(const core::PointerSample&)> pointerUp;
        std::function<void(const core::PointerSample&)> contextClick;
        std::function<void()> pointerCancelled;
        std::function<void(core::Point pointPx, const core::WheelInput&)> wheel;

        // virtualKey 为原始 Win32 VK_* 码（供宿主调试热键使用），key 为标准化 KeyInput
        std::function<bool(int virtualKey, const core::KeyInput& key)> keyDown;
        std::function<bool(const std::function<bool(int)>& isDown)> keyStateChanged;
        std::function<void(core::KeyModifierState)> modifiersChanged;
        std::function<bool(std::string_view utf8Text)> charInput;

        std::function<bool(std::string preedit, std::size_t cursorOffsetBytes, std::vector<core::TextCompositionClause> clauses)> imeComposition;
        std::function<bool(std::string committed)> imeCommit;
        std::function<bool()> imeEndComposition;

        // 任务栏缩略图工具栏按钮点击回调（ITaskbarList3 THBN_CLICKED）
        std::function<void(const std::string& actionId)> taskbarActionClicked;
    };

    class Window {
       public:
        Window(const core::WindowConfigSpec& config, void* ownerHwnd, WindowCallbacks callbacks);
        ~Window();
        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        [[nodiscard]] void* handle() const noexcept;
        [[nodiscard]] void* ownerHandle() const noexcept;
        [[nodiscard]] const core::WindowConfigSpec& config() const noexcept;
        // 当前窗口是否成功启用了 Win11 DWM 系统材质（Mica / Mica Alt / Acrylic）；为真时渲染器清为透明底色
        [[nodiscard]] bool backdropActive() const noexcept;
        [[nodiscard]] double dpiScale() const noexcept;
        void clientSize(int& widthPx, int& heightPx) const;

        void updateConfig(const core::WindowConfigSpec& config);
        void setIcons(void* bigHicon, void* smallHicon);
        void setTaskbarProgress(const core::TaskbarProgressSpec& spec);
        void setTaskbarBadge(void* overlayHicon, std::string_view label);
        void setTaskbarActions(const std::vector<core::TaskbarActionSpec>& actions, const std::vector<void*>& actionHicons);
        void activate();
        void requestAttention(core::WindowAttentionMode mode);
        void setEnabled(bool enabled);
        [[nodiscard]] bool isEnabled() const noexcept;

        // 输入与平台效果控制
        void applyPointerEffects(const core::PointerPlatformEffects& effects);
        void setImeEnabled(bool enabled);
        [[nodiscard]] bool imeEnabled() const noexcept;
        void setImeCaretBounds(core::Rect caretPx);
        void cancelImeComposition();
        void tickPrecisionWheel();
        [[nodiscard]] bool needsPrecisionWheelTicks() const noexcept;

        // 供自动化单测直接查询当前 HWND 的 WM_NCHITTEST 结果（传入窗口客户区物理像素坐标）
        [[nodiscard]] long sendHitTestAtClientPoint(int clientX, int clientY) const;

       private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}
