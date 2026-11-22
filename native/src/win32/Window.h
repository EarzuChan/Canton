#pragma once

#include <core/InputEditing.h>
#include <core/InputSession.h>
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

    struct MessageDispatcherCallbacks {
        std::function<void()> wake;
        std::function<void()> fontsChanged;
        std::function<void()> endSession;
    };

    // UI Owner 线程级隐藏消息分发窗（HWND_MESSAGE）：提供与业务窗口生灭解耦的跨线程唤醒与进程退出通道
    class MessageDispatcher {
       public:
        explicit MessageDispatcher(MessageDispatcherCallbacks callbacks);
        ~MessageDispatcher();
        MessageDispatcher(const MessageDispatcher&) = delete;
        MessageDispatcher& operator=(const MessageDispatcher&) = delete;

        void postWake() noexcept;
        static int runMessageLoop();
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
        void activate();
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
