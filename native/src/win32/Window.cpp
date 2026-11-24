#include <win32/Window.h>
#include <win32/WindowsPrecisionWheelSource.h>

#include <core/Clock.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>
#include <shobjidl_core.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace canton::win32 {
    namespace {
        using Microsoft::WRL::ComPtr;

        constexpr wchar_t windowClassName[] = L"CantonHostWindow";
        constexpr wchar_t messageClassName[] = L"CantonMessageDispatcher";
        constexpr UINT kWakeMessage = WM_APP + 1;

        constexpr DWORD kDwmUseImmersiveDarkMode = 20;
        constexpr DWORD kDwmWindowCornerPreference = 33;
        constexpr DWORD kDwmSystemBackdropType = 38;

        std::wstring utf8ToWide(std::string_view text) {
            if (text.empty()) return {};
            const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
            if (needed <= 0) return {};
            std::wstring out(static_cast<std::size_t>(needed), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
            return out;
        }

        std::string wideToUtf8(std::wstring_view text) {
            if (text.empty()) return {};
            const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            if (needed <= 0) return {};
            std::string out(static_cast<std::size_t>(needed), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed, nullptr, nullptr);
            return out;
        }

        std::size_t utf16OffsetToUtf8Bytes(std::wstring_view text, std::size_t utf16Offset) {
            const auto clamped = std::min(utf16Offset, text.size());
            if (clamped == 0) return 0;
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(clamped), nullptr, 0, nullptr, nullptr);
            return bytes > 0 ? static_cast<std::size_t>(bytes) : 0;
        }

        std::string encodeCodepointUtf8(char32_t cp) {
            if (cp <= 0x7fu) return std::string(1, static_cast<char>(cp));
            if (cp <= 0x7ffu) {
                return {
                    static_cast<char>(0xc0u | ((cp >> 6u) & 0x1fu)),
                    static_cast<char>(0x80u | (cp & 0x3fu)),
                };
            }
            if (cp <= 0xffffu) {
                return {
                    static_cast<char>(0xe0u | ((cp >> 12u) & 0x0fu)),
                    static_cast<char>(0x80u | ((cp >> 6u) & 0x3fu)),
                    static_cast<char>(0x80u | (cp & 0x3fu)),
                };
            }
            return {
                static_cast<char>(0xf0u | ((cp >> 18u) & 0x07u)),
                static_cast<char>(0x80u | ((cp >> 12u) & 0x3fu)),
                static_cast<char>(0x80u | ((cp >> 6u) & 0x3fu)),
                static_cast<char>(0x80u | (cp & 0x3fu)),
            };
        }

        core::KeyModifierState currentModifiers() noexcept {
            core::KeyModifierState state;
            state.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            state.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            state.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            state.meta = ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) != 0;
            state.command = state.ctrl;
            return state;
        }

        std::string translateVirtualKeyName(WPARAM vk, LPARAM lParam) {
            switch (vk) {
                case VK_BACK: return "Backspace";
                case VK_TAB: return "Tab";
                case VK_RETURN: return "Enter";
                case VK_ESCAPE: return "Escape";
                case VK_SPACE: return " ";
                case VK_PRIOR: return "PageUp";
                case VK_NEXT: return "PageDown";
                case VK_END: return "End";
                case VK_HOME: return "Home";
                case VK_LEFT: return "ArrowLeft";
                case VK_UP: return "ArrowUp";
                case VK_RIGHT: return "ArrowRight";
                case VK_DOWN: return "ArrowDown";
                case VK_INSERT: return "Insert";
                case VK_DELETE: return "Delete";
                case VK_SHIFT:
                case VK_LSHIFT:
                case VK_RSHIFT: return "Shift";
                case VK_CONTROL:
                case VK_LCONTROL:
                case VK_RCONTROL: return "Control";
                case VK_MENU:
                case VK_LMENU:
                case VK_RMENU: return "Alt";
                case VK_LWIN:
                case VK_RWIN: return "Meta";
                case VK_CAPITAL: return "CapsLock";
                default: break;
            }
            if (vk >= VK_F1 && vk <= VK_F24) {
                return "F" + std::to_string(static_cast<int>(vk - VK_F1 + 1));
            }
            if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
                BYTE keyState[256]{};
                if (GetKeyboardState(keyState)) {
                    // Ctrl 按下时不把字母翻成控制字符，保持快捷键可识别的按键名
                    keyState[VK_CONTROL] = keyState[VK_LCONTROL] = keyState[VK_RCONTROL] = 0;
                    keyState[VK_MENU] = keyState[VK_LMENU] = keyState[VK_RMENU] = 0;
                    wchar_t chars[4]{};
                    const UINT scanCode = (static_cast<UINT>(lParam) >> 16u) & 0xffu;
                    const int written = ToUnicode(static_cast<UINT>(vk), scanCode, keyState, chars, 4, 0);
                    if (written == 1 && chars[0] >= 0x20) {
                        return wideToUtf8(std::wstring_view(chars, 1));
                    }
                }
                return std::string(1, static_cast<char>(vk));
            }
            const UINT mapped = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR) & 0xffffu;
            if (mapped >= 0x20) {
                const wchar_t wc = static_cast<wchar_t>(mapped);
                return wideToUtf8(std::wstring_view(&wc, 1));
            }
            return "Unidentified";
        }

        LRESULT toNativeHitResult(core::WindowHitResult result) noexcept {
            switch (result) {
                case core::WindowHitResult::Client:
                case core::WindowHitResult::ClientInteractive:
                    return HTCLIENT;
                case core::WindowHitResult::Draggable:
                    return HTCAPTION;
                case core::WindowHitResult::MinimizeButton:
                    return HTMINBUTTON;
                case core::WindowHitResult::MaximizeButton:
                    return HTMAXBUTTON;
                case core::WindowHitResult::CloseButton:
                    return HTCLOSE;
                case core::WindowHitResult::Left:
                    return HTLEFT;
                case core::WindowHitResult::Right:
                    return HTRIGHT;
                case core::WindowHitResult::Top:
                    return HTTOP;
                case core::WindowHitResult::Bottom:
                    return HTBOTTOM;
                case core::WindowHitResult::TopLeft:
                    return HTTOPLEFT;
                case core::WindowHitResult::TopRight:
                    return HTTOPRIGHT;
                case core::WindowHitResult::BottomLeft:
                    return HTBOTTOMLEFT;
                case core::WindowHitResult::BottomRight:
                    return HTBOTTOMRIGHT;
            }
            return HTCLIENT;
        }

        bool isCaptionButtonHit(LRESULT ht) noexcept {
            return ht == HTMINBUTTON || ht == HTMAXBUTTON || ht == HTCLOSE;
        }

        HCURSOR cursorForIcon(core::PointerIcon icon) noexcept {
            switch (icon) {
                case core::PointerIcon::Default:
                    return LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
                case core::PointerIcon::Hand:
                case core::PointerIcon::Grab:
                case core::PointerIcon::Grabbing:
                    return LoadCursorW(nullptr, MAKEINTRESOURCEW(32649));  // IDC_HAND
                case core::PointerIcon::Text:
                    return LoadCursorW(nullptr, MAKEINTRESOURCEW(32513));  // IDC_IBEAM
                case core::PointerIcon::Crosshair:
                    return LoadCursorW(nullptr, MAKEINTRESOURCEW(32515));  // IDC_CROSS
                case core::PointerIcon::ResizeHorizontal:
                    return LoadCursorW(nullptr, MAKEINTRESOURCEW(32644));  // IDC_SIZEWE
                case core::PointerIcon::ResizeVertical:
                    return LoadCursorW(nullptr, MAKEINTRESOURCEW(32645));  // IDC_SIZENS
                case core::PointerIcon::ResizeNwse:
                    return LoadCursorW(nullptr, MAKEINTRESOURCEW(32642));  // IDC_SIZENWSE
                case core::PointerIcon::ResizeNesw:
                    return LoadCursorW(nullptr, MAKEINTRESOURCEW(32643));  // IDC_SIZENESW
                case core::PointerIcon::None:
                    return nullptr;
            }
            return LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        }

        UINT queryDpiForMonitorOrSystem(HWND owner) noexcept {
            (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            if (owner && IsWindow(owner)) {
                const UINT dpi = GetDpiForWindow(owner);
                if (dpi > 0) return dpi;
            }
            POINT pt{};
            GetCursorPos(&pt);
            if (HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY)) {
                // 通过系统 DPI 兜底前先按主屏/当前屏估算；CreateWindowExW 后仍以 GetDpiForWindow 精确校准
                (void)mon;
            }
            const UINT sysDpi = GetDpiForSystem();
            return sysDpi > 0 ? sysDpi : 96u;
        }

        void computeWindowStyles(const core::WindowConfigSpec& config, bool hasOwner, DWORD& style, DWORD& exStyle) noexcept {
            exStyle = WS_EX_NOREDIRECTIONBITMAP;
            if (config.alwaysOnTop) exStyle |= WS_EX_TOPMOST;

            const bool defaultTaskbar = config.role == core::WindowRole::Standard && !hasOwner;
            const bool wantTaskbar = config.showInTaskbar.value_or(defaultTaskbar);
            if (config.role == core::WindowRole::Utility && !wantTaskbar) {
                exStyle |= WS_EX_TOOLWINDOW;
            } else if (wantTaskbar && (hasOwner || config.role == core::WindowRole::Utility)) {
                exStyle |= WS_EX_APPWINDOW;
            }

            if (config.chrome == core::WindowChrome::Frameless) {
                // 不带 WS_SYSMENU，避免 Win11 DWM 在半透明材质层上叠画幽灵系统三键
                style = WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
                if (config.minimizable) style |= WS_MINIMIZEBOX;
                if (config.maximizable) style |= WS_MAXIMIZEBOX;
                if (config.resizable) style |= WS_THICKFRAME;
                return;
            }

            if (config.chrome == core::WindowChrome::CustomTitleBar) {
                // 自绘标题栏去掉 WS_SYSMENU 以彻底消除 DWM 底层叠画的系统三键残影，保留 WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZEBOX 支撑圆角阴影、Aero Snap 与 Win11 Snap Layout
                style = WS_OVERLAPPED | WS_CAPTION | WS_THICKFRAME | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
                if (config.minimizable) style |= WS_MINIMIZEBOX;
                if (config.maximizable) style |= WS_MAXIMIZEBOX;
                return;
            }

            style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
            if (config.resizable) {
                style |= WS_THICKFRAME;
            }
            if (config.minimizable) style |= WS_MINIMIZEBOX;
            if (config.maximizable) style |= WS_MAXIMIZEBOX;
        }

        RECT outerRectForClientSize(int clientW, int clientH, const core::WindowConfigSpec& config, DWORD style, DWORD exStyle, UINT dpi) noexcept {
            RECT rect{0, 0, std::max(1, clientW), std::max(1, clientH)};
            if (config.chrome == core::WindowChrome::System) {
                AdjustWindowRectExForDpi(&rect, style, FALSE, exStyle, dpi);
            }
            return rect;
        }
    }

    void setProcessAppId(std::string_view appId) {
        if (appId.empty()) return;
        const auto wide = utf8ToWide(appId);
        if (!wide.empty()) (void)SetCurrentProcessExplicitAppUserModelID(wide.c_str());
    }

    std::string readClipboardText() {
        if (!OpenClipboard(nullptr)) return {};
        std::string result;
        if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
            if (const auto* text = static_cast<const wchar_t*>(GlobalLock(handle))) {
                const auto raw = wideToUtf8(text);
                result.reserve(raw.size());
                for (std::size_t i = 0; i < raw.size(); ++i) {
                    if (raw[i] == '\r') {
                        if (i + 1 < raw.size() && raw[i + 1] == '\n') continue;
                        result.push_back('\n');
                    } else {
                        result.push_back(raw[i]);
                    }
                }
                GlobalUnlock(handle);
            }
        }
        CloseClipboard();
        return result;
    }

    void writeClipboardText(std::string_view utf8Text) {
        std::string crlf;
        crlf.reserve(utf8Text.size() + 16);
        for (std::size_t i = 0; i < utf8Text.size(); ++i) {
            if (utf8Text[i] == '\n' && (i == 0 || utf8Text[i - 1] != '\r')) crlf.push_back('\r');
            crlf.push_back(utf8Text[i]);
        }
        const auto wide = utf8ToWide(crlf);
        if (!OpenClipboard(nullptr)) return;
        EmptyClipboard();
        const std::size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (auto* dst = static_cast<wchar_t*>(GlobalLock(mem))) {
                std::memcpy(dst, wide.c_str(), bytes);
                GlobalUnlock(mem);
                if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
            } else {
                GlobalFree(mem);
            }
        }
        CloseClipboard();
    }

    IconHandle::~IconHandle() {
        if (handle_) DestroyIcon(static_cast<HICON>(handle_));
    }

    IconHandle::IconHandle(IconHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    IconHandle& IconHandle::operator=(IconHandle&& other) noexcept {
        if (this != &other) {
            if (handle_) DestroyIcon(static_cast<HICON>(handle_));
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    IconHandle IconHandle::fromPbgra(const std::uint32_t* pixels, int width, int height) {
        if (!pixels || width <= 0 || height <= 0) return {};
        BITMAPV5HEADER header{};
        header.bV5Size = sizeof(header);
        header.bV5Width = width;
        header.bV5Height = -height;
        header.bV5Planes = 1;
        header.bV5BitCount = 32;
        header.bV5Compression = BI_BITFIELDS;
        header.bV5RedMask = 0x00ff0000u;
        header.bV5GreenMask = 0x0000ff00u;
        header.bV5BlueMask = 0x000000ffu;
        header.bV5AlphaMask = 0xff000000u;

        void* bits = nullptr;
        HDC dc = GetDC(nullptr);
        HBITMAP colorBmp = CreateDIBSection(dc, reinterpret_cast<const BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
        ReleaseDC(nullptr, dc);
        if (!colorBmp || !bits) {
            if (colorBmp) DeleteObject(colorBmp);
            return {};
        }
        std::memcpy(bits, pixels, static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * sizeof(std::uint32_t));
        HBITMAP maskBmp = CreateBitmap(width, height, 1, 1, nullptr);
        if (!maskBmp) {
            DeleteObject(colorBmp);
            return {};
        }

        ICONINFO info{};
        info.fIcon = TRUE;
        info.hbmColor = colorBmp;
        info.hbmMask = maskBmp;
        HICON icon = CreateIconIndirect(&info);
        DeleteObject(colorBmp);
        DeleteObject(maskBmp);
        return IconHandle(icon);
    }

    struct MessageDispatcher::Impl {
        HWND hwnd = nullptr;
        MessageDispatcherCallbacks callbacks;
        std::atomic<bool> wakePosted{false};

        static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
            if (message == WM_NCCREATE) {
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
            }
            auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (self && message == kWakeMessage) {
                self->wakePosted.store(false, std::memory_order_release);
                if (self->callbacks.wake) self->callbacks.wake();
                return 0;
            }
            return DefWindowProcW(window, message, wParam, lParam);
        }
    };

    MessageDispatcher::MessageDispatcher(MessageDispatcherCallbacks callbacks) : impl_(std::make_unique<Impl>()) {
        impl_->callbacks = std::move(callbacks);
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = Impl::procedure;
        wc.hInstance = instance;
        wc.lpszClassName = messageClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error("无法登记宿主消息分发窗口类");
        }
        impl_->hwnd = CreateWindowExW(0, messageClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, impl_.get());
        if (!impl_->hwnd) throw std::runtime_error("无法创建宿主消息分发窗");
    }

    MessageDispatcher::~MessageDispatcher() {
        if (impl_ && impl_->hwnd) {
            SetWindowLongPtrW(impl_->hwnd, GWLP_USERDATA, 0);
            DestroyWindow(impl_->hwnd);
            impl_->hwnd = nullptr;
        }
    }

    void MessageDispatcher::postWake() noexcept {
        const auto target = impl_ ? impl_->hwnd : nullptr;
        if (target && !impl_->wakePosted.exchange(true, std::memory_order_acq_rel)) {
            if (!PostMessageW(target, kWakeMessage, 0, 0)) {
                impl_->wakePosted.store(false, std::memory_order_release);
            }
        }
    }

    int MessageDispatcher::runMessageLoop() {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            // PostMessage(kWakeMessage) 在 Win32 队列中优先级高于 WM_MOUSEMOVE 等输入消息；
            // 连续动画帧期间若不先排空输入队列，会导致鼠标拖拽事件被周期性饿死。
            if (message.message == kWakeMessage) {
                MSG input{};
                int drained = 0;
                while (drained++ < 64 && PeekMessageW(&input, nullptr, 0, 0, PM_REMOVE | PM_QS_INPUT)) {
                    if (input.message == WM_QUIT) {
                        PostQuitMessage(static_cast<int>(input.wParam));
                        return static_cast<int>(input.wParam);
                    }
                    TranslateMessage(&input);
                    DispatchMessageW(&input);
                }
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

    void MessageDispatcher::quitMessageLoop(int exitCode) noexcept {
        PostQuitMessage(exitCode);
    }

    struct Window::Impl {
        HWND hwnd = nullptr;
        HWND ownerHwnd = nullptr;
        core::WindowConfigSpec config;
        WindowCallbacks callbacks;
        std::unique_ptr<WindowsPrecisionWheelSource> precisionWheel;
        ComPtr<ITaskbarList> taskbarList;

        double dpiScale = 1.0;
        bool backdropActive = false;
        bool imeEnabled = false;
        HIMC savedImc = nullptr;
        core::Rect imeCaretRect{0, 0, 1, 16};

        bool trackingClientLeave = false;
        bool trackingNcLeave = false;
        LRESULT ncPressedButton = HTNOWHERE;
        bool inFullscreen = false;
        bool preFullscreenMaximized = false;
        DWORD preFullscreenStyle = 0;
        DWORD preFullscreenExStyle = 0;
        RECT preFullscreenRect{};
        bool internalGeometryChange = false;
        bool constructing = false;
        bool pointerLocked = false;
        bool releasingCapture = false;
        wchar_t pendingHighSurrogate = 0;

        int clickCount = 0;
        DWORD lastClickTick = 0;
        POINT lastClickPoint{};

        static std::atomic<bool> endSessionHandled;

        void syncCloseMenuItem() {
            if (!hwnd) return;
            if (HMENU menu = GetSystemMenu(hwnd, FALSE)) {
                EnableMenuItem(menu, SC_CLOSE, MF_BYCOMMAND | (config.closable ? MF_ENABLED : (MF_DISABLED | MF_GRAYED)));
                EnableMenuItem(menu, SC_SIZE, MF_BYCOMMAND | (config.resizable ? MF_ENABLED : (MF_DISABLED | MF_GRAYED)));
                EnableMenuItem(menu, SC_MAXIMIZE, MF_BYCOMMAND | (config.maximizable ? MF_ENABLED : (MF_DISABLED | MF_GRAYED)));
                EnableMenuItem(menu, SC_MINIMIZE, MF_BYCOMMAND | (config.minimizable ? MF_ENABLED : (MF_DISABLED | MF_GRAYED)));
            }
        }

        void applyDwmAttributes() {
            if (!hwnd) return;
            const BOOL dark = TRUE;
            (void)DwmSetWindowAttribute(hwnd, kDwmUseImmersiveDarkMode, &dark, sizeof(dark));

            DWORD corner = 0;
            switch (config.cornerPreference) {
                case core::WindowCornerPreference::Default: corner = 0; break;
                case core::WindowCornerPreference::Square: corner = 1; break;
                case core::WindowCornerPreference::Round: corner = 2; break;
                case core::WindowCornerPreference::RoundSmall: corner = 3; break;
            }
            if (config.transparent) corner = DWMWCP_DONOTROUND;
            (void)DwmSetWindowAttribute(hwnd, kDwmWindowCornerPreference, &corner, sizeof(corner));
            const COLORREF border = config.transparent ? DWMWA_COLOR_NONE : DWMWA_COLOR_DEFAULT;
            (void)DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));

            if (config.backdrop != core::WindowBackdrop::None) {
                const MARGINS full{-1, -1, -1, -1};
                (void)DwmExtendFrameIntoClientArea(hwnd, &full);
                DWORD backdrop = 1;
                switch (config.backdrop) {
                    case core::WindowBackdrop::Mica: backdrop = 2; break;
                    case core::WindowBackdrop::Acrylic: backdrop = 3; break;
                    case core::WindowBackdrop::MicaAlt: backdrop = 4; break;
                    case core::WindowBackdrop::None: backdrop = 1; break;
                }
                const HRESULT hr = DwmSetWindowAttribute(hwnd, kDwmSystemBackdropType, &backdrop, sizeof(backdrop));
                backdropActive = SUCCEEDED(hr);
                if (!backdropActive) {
                    const MARGINS fallbackMargins = config.chrome == core::WindowChrome::CustomTitleBar ? MARGINS{0, 0, 0, 1} : MARGINS{0, 0, 0, 0};
                    (void)DwmExtendFrameIntoClientArea(hwnd, &fallbackMargins);
                }
            } else {
                backdropActive = false;
                DWORD none = 1;
                (void)DwmSetWindowAttribute(hwnd, kDwmSystemBackdropType, &none, sizeof(none));
                const MARGINS margins = config.chrome == core::WindowChrome::CustomTitleBar ? MARGINS{0, 0, 0, 1} : MARGINS{0, 0, 0, 0};
                (void)DwmExtendFrameIntoClientArea(hwnd, &margins);
            }
        }

        void applyTaskbarVisibility() {
            if (!hwnd) return;
            const bool defaultShow = config.role == core::WindowRole::Standard && ownerHwnd == nullptr;
            const bool shouldShow = config.visible && config.showInTaskbar.value_or(defaultShow);
            if (!config.showInTaskbar.has_value()) return;
            if (!taskbarList) {
                if (FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(taskbarList.GetAddressOf()))) || FAILED(taskbarList->HrInit())) {
                    taskbarList.Reset();
                    return;
                }
            }
            if (shouldShow)
                (void)taskbarList->AddTab(hwnd);
            else
                (void)taskbarList->DeleteTab(hwnd);
        }

        void computeInitialPosition(int outerWidth, int outerHeight, int& outX, int& outY) const {
            if (config.position.preset == core::WindowPositionPreset::Coordinates) {
                outX = config.position.x;
                outY = config.position.y;
                return;
            }
            if (config.position.preset == core::WindowPositionPreset::PlatformDefault) {
                outX = CW_USEDEFAULT;
                outY = CW_USEDEFAULT;
                return;
            }
            if (config.position.preset == core::WindowPositionPreset::CenterOwner && ownerHwnd && IsWindow(ownerHwnd)) {
                RECT ownerRect{};
                if (GetWindowRect(ownerHwnd, &ownerRect)) {
                    outX = ownerRect.left + ((ownerRect.right - ownerRect.left) - outerWidth) / 2;
                    outY = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - outerHeight) / 2;
                    return;
                }
            }
            POINT anchor{};
            if (ownerHwnd && IsWindow(ownerHwnd)) {
                RECT r{};
                GetWindowRect(ownerHwnd, &r);
                anchor = {(r.left + r.right) / 2, (r.top + r.bottom) / 2};
            } else {
                GetCursorPos(&anchor);
            }
            HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (monitor && GetMonitorInfoW(monitor, &mi)) {
                const auto& work = mi.rcWork;
                outX = work.left + ((work.right - work.left) - outerWidth) / 2;
                outY = work.top + ((work.bottom - work.top) - outerHeight) / 2;
            } else {
                outX = CW_USEDEFAULT;
                outY = CW_USEDEFAULT;
            }
        }

        void enterFullscreen() {
            if (!hwnd || inFullscreen) return;
            inFullscreen = true;
            preFullscreenMaximized = IsZoomed(hwnd) != FALSE;
            preFullscreenStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
            preFullscreenExStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
            GetWindowRect(hwnd, &preFullscreenRect);

            HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            GetMonitorInfoW(monitor, &mi);

            internalGeometryChange = true;
            if (preFullscreenMaximized) ShowWindow(hwnd, SW_RESTORE);
            SetWindowLongPtrW(hwnd, GWL_STYLE, (preFullscreenStyle & ~(WS_CAPTION | WS_THICKFRAME)) | WS_POPUP);
            SetWindowPos(
                hwnd,
                config.alwaysOnTop ? HWND_TOPMOST : nullptr,
                mi.rcMonitor.left,
                mi.rcMonitor.top,
                mi.rcMonitor.right - mi.rcMonitor.left,
                mi.rcMonitor.bottom - mi.rcMonitor.top,
                SWP_FRAMECHANGED | SWP_NOACTIVATE | (config.alwaysOnTop ? 0 : SWP_NOZORDER));
            internalGeometryChange = false;
            emitGeometrySnapshot();
        }

        void exitFullscreen(core::WindowPlacement targetPlacement) {
            if (!hwnd || !inFullscreen) return;
            inFullscreen = false;
            DWORD style = 0, exStyle = 0;
            computeWindowStyles(config, ownerHwnd != nullptr, style, exStyle);
            internalGeometryChange = true;
            SetWindowLongPtrW(hwnd, GWL_STYLE, style | (IsWindowVisible(hwnd) ? WS_VISIBLE : 0));
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
            SetWindowPos(
                hwnd,
                config.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                preFullscreenRect.left,
                preFullscreenRect.top,
                preFullscreenRect.right - preFullscreenRect.left,
                preFullscreenRect.bottom - preFullscreenRect.top,
                SWP_FRAMECHANGED | SWP_NOACTIVATE);
            if (targetPlacement == core::WindowPlacement::Maximized || (targetPlacement == core::WindowPlacement::Normal && preFullscreenMaximized && config.placement == core::WindowPlacement::Maximized)) {
                ShowWindow(hwnd, SW_MAXIMIZE);
            } else if (targetPlacement == core::WindowPlacement::Minimized) {
                ShowWindow(hwnd, SW_MINIMIZE);
            }
            internalGeometryChange = false;
            applyDwmAttributes();
            syncCloseMenuItem();
            emitGeometrySnapshot();
        }

        void applyPlacementAndVisibility(bool initial) {
            if (!hwnd) return;
            if (!config.visible) {
                if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
                return;
            }

            if (config.placement == core::WindowPlacement::Fullscreen) {
                if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNORMAL);
                enterFullscreen();
                applyTaskbarVisibility();
                return;
            }
            if (inFullscreen) {
                exitFullscreen(config.placement);
            }

            if (config.placement == core::WindowPlacement::Minimized) {
                if (!IsIconic(hwnd)) ShowWindow(hwnd, SW_MINIMIZE);
            } else if (config.placement == core::WindowPlacement::Maximized) {
                if (!IsZoomed(hwnd)) ShowWindow(hwnd, SW_MAXIMIZE);
            } else {
                if (IsIconic(hwnd) || IsZoomed(hwnd)) {
                    ShowWindow(hwnd, SW_RESTORE);
                } else if (!IsWindowVisible(hwnd)) {
                    ShowWindow(hwnd, initial ? SW_SHOWNORMAL : SW_SHOW);
                }
            }
            if (initial) UpdateWindow(hwnd);
            applyTaskbarVisibility();
        }

        core::WindowPlacement currentPlacement() const noexcept {
            if (inFullscreen) return core::WindowPlacement::Fullscreen;
            if (hwnd && IsIconic(hwnd)) return core::WindowPlacement::Minimized;
            if (hwnd && IsZoomed(hwnd)) return core::WindowPlacement::Maximized;
            return core::WindowPlacement::Normal;
        }

        void emitGeometrySnapshot() {
            if (!hwnd) return;
            RECT client{};
            GetClientRect(hwnd, &client);
            const int cw = std::max(0, static_cast<int>(client.right - client.left));
            const int ch = std::max(0, static_cast<int>(client.bottom - client.top));
            if (precisionWheel && cw > 0 && ch > 0) {
                precisionWheel->sync(hwnd, client, 1.0f);
            }
            const auto placement = currentPlacement();
            core::WindowGeometryEvent ev;
            ev.widthPx = cw;
            ev.heightPx = ch;
            ev.placement = placement;
            if (placement == core::WindowPlacement::Normal) {
                RECT winRect{};
                if (GetWindowRect(hwnd, &winRect)) {
                    ev.x = winRect.left;
                    ev.y = winRect.top;
                    ev.hasPosition = true;
                }
            }
            if (callbacks.geometryChanged) callbacks.geometryChanged(ev);
        }

        void updateImeCandidatePosition() {
            if (!hwnd || !imeEnabled) return;
            HIMC imc = ImmGetContext(hwnd);
            if (!imc) return;
            const LONG cx = static_cast<LONG>(std::lround(imeCaretRect.x));
            const LONG top = static_cast<LONG>(std::lround(imeCaretRect.y));
            const LONG bottom = static_cast<LONG>(std::lround(imeCaretRect.y + std::max(1.0f, imeCaretRect.height)));
            const LONG right = static_cast<LONG>(std::lround(imeCaretRect.x + std::max(1.0f, imeCaretRect.width)));

            COMPOSITIONFORM comp{};
            comp.dwStyle = CFS_POINT;
            comp.ptCurrentPos = {cx, top};
            (void)ImmSetCompositionWindow(imc, &comp);

            CANDIDATEFORM exclude{};
            exclude.dwIndex = 0;
            exclude.dwStyle = CFS_EXCLUDE;
            exclude.ptCurrentPos = {cx, bottom};
            exclude.rcArea = {cx, top, right, bottom};
            (void)ImmSetCandidateWindow(imc, &exclude);

            ImmReleaseContext(hwnd, imc);
        }

        int recordClickCount(POINT pt) {
            const DWORD now = GetTickCount();
            const UINT maxDt = GetDoubleClickTime();
            const int maxDx = GetSystemMetrics(SM_CXDOUBLECLK) / 2 + 1;
            const int maxDy = GetSystemMetrics(SM_CYDOUBLECLK) / 2 + 1;
            if (clickCount > 0 && (now - lastClickTick) <= maxDt && std::abs(pt.x - lastClickPoint.x) <= maxDx && std::abs(pt.y - lastClickPoint.y) <= maxDy) {
                clickCount = (clickCount % 4) + 1;
            } else {
                clickCount = 1;
            }
            lastClickTick = now;
            lastClickPoint = pt;
            return clickCount;
        }

        core::PointerSample makeSample(POINT clientPt) const {
            return {
                {static_cast<float>(clientPt.x), static_cast<float>(clientPt.y)},
                currentModifiers(),
                core::monotonicMillis(),
            };
        }

        void handleImeComposition(LPARAM lParam) {
            if (!hwnd || !imeEnabled) return;
            HIMC imc = ImmGetContext(hwnd);
            if (!imc) return;

            if (lParam & GCS_RESULTSTR) {
                const LONG bytes = ImmGetCompositionStringW(imc, GCS_RESULTSTR, nullptr, 0);
                if (bytes > 0) {
                    std::wstring wstr(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
                    ImmGetCompositionStringW(imc, GCS_RESULTSTR, wstr.data(), static_cast<DWORD>(bytes));
                    if (callbacks.imeCommit) (void)callbacks.imeCommit(wideToUtf8(wstr));
                } else if (bytes == 0) {
                    if (callbacks.imeCommit) (void)callbacks.imeCommit({});
                }
            }

            if (lParam & GCS_COMPSTR) {
                const LONG compBytes = ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
                if (compBytes > 0) {
                    std::wstring compWstr(static_cast<std::size_t>(compBytes) / sizeof(wchar_t), L'\0');
                    ImmGetCompositionStringW(imc, GCS_COMPSTR, compWstr.data(), static_cast<DWORD>(compBytes));

                    const LONG cursorW = std::max<LONG>(0, ImmGetCompositionStringW(imc, GCS_CURSORPOS, nullptr, 0));
                    const auto cursorBytes = utf16OffsetToUtf8Bytes(compWstr, static_cast<std::size_t>(cursorW));

                    std::vector<std::uint8_t> attrs;
                    if (lParam & GCS_COMPATTR) {
                        const LONG attrBytes = ImmGetCompositionStringW(imc, GCS_COMPATTR, nullptr, 0);
                        if (attrBytes > 0) {
                            attrs.resize(static_cast<std::size_t>(attrBytes));
                            ImmGetCompositionStringW(imc, GCS_COMPATTR, attrs.data(), static_cast<DWORD>(attrBytes));
                        }
                    }

                    std::vector<core::TextCompositionClause> clauses;
                    if (lParam & GCS_COMPCLAUSE) {
                        const LONG clauseBytes = ImmGetCompositionStringW(imc, GCS_COMPCLAUSE, nullptr, 0);
                        if (clauseBytes >= static_cast<LONG>(sizeof(std::uint32_t) * 2)) {
                            std::vector<std::uint32_t> offsets(static_cast<std::size_t>(clauseBytes) / sizeof(std::uint32_t));
                            ImmGetCompositionStringW(imc, GCS_COMPCLAUSE, offsets.data(), static_cast<DWORD>(clauseBytes));
                            for (std::size_t i = 0; i + 1 < offsets.size(); ++i) {
                                const std::size_t u16Start = std::min<std::size_t>(offsets[i], compWstr.size());
                                const std::size_t u16End = std::min<std::size_t>(offsets[i + 1], compWstr.size());
                                if (u16End <= u16Start) continue;
                                bool isTarget = false;
                                for (std::size_t k = u16Start; k < u16End && k < attrs.size(); ++k) {
                                    if (attrs[k] == ATTR_TARGET_CONVERTED || attrs[k] == ATTR_TARGET_NOTCONVERTED) {
                                        isTarget = true;
                                        break;
                                    }
                                }
                                clauses.push_back({
                                    utf16OffsetToUtf8Bytes(compWstr, u16Start),
                                    utf16OffsetToUtf8Bytes(compWstr, u16End),
                                    isTarget,
                                });
                            }
                        }
                    }

                    if (clauses.empty() && !attrs.empty()) {
                        // 无显式 COMPCLAUSE 时按连续属性分段
                        std::size_t runStart = 0;
                        const auto isTargetAttr = [](std::uint8_t a) { return a == ATTR_TARGET_CONVERTED || a == ATTR_TARGET_NOTCONVERTED; };
                        while (runStart < compWstr.size()) {
                            const bool target = runStart < attrs.size() && isTargetAttr(attrs[runStart]);
                            std::size_t runEnd = runStart + 1;
                            while (runEnd < compWstr.size() && ((runEnd < attrs.size() && isTargetAttr(attrs[runEnd])) == target)) ++runEnd;
                            clauses.push_back({utf16OffsetToUtf8Bytes(compWstr, runStart), utf16OffsetToUtf8Bytes(compWstr, runEnd), target});
                            runStart = runEnd;
                        }
                    }

                    if (callbacks.imeComposition) {
                        (void)callbacks.imeComposition(wideToUtf8(compWstr), cursorBytes, std::move(clauses));
                    }
                } else if (!(lParam & GCS_RESULTSTR)) {
                    if (callbacks.imeComposition) (void)callbacks.imeComposition({}, 0, {});
                }
            }

            ImmReleaseContext(hwnd, imc);
            updateImeCandidatePosition();
        }

        static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
            if (message == WM_NCCREATE) {
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
            }
            auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (!self) return DefWindowProcW(window, message, wParam, lParam);

            switch (message) {
                case WM_CLOSE:
                    if (self->config.closable && self->callbacks.closeRequested) {
                        self->callbacks.closeRequested();
                    }
                    return 0;

                case WM_NCCALCSIZE: {
                    if (self->config.chrome == core::WindowChrome::CustomTitleBar || self->config.chrome == core::WindowChrome::Frameless || self->inFullscreen) {
                        if (wParam == TRUE) {
                            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                            if (!self->inFullscreen && IsZoomed(window)) {
                                const UINT dpi = GetDpiForWindow(window);
                                const int frameX = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                                const int frameY = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                                params->rgrc[0].left += frameX;
                                params->rgrc[0].top += frameY;
                                params->rgrc[0].right -= frameX;
                                params->rgrc[0].bottom -= frameY;
                                if (HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST)) {
                                    MONITORINFO mi{};
                                    mi.cbSize = sizeof(mi);
                                    if (GetMonitorInfoW(monitor, &mi)) {
                                        params->rgrc[0].left = std::max(params->rgrc[0].left, mi.rcWork.left);
                                        params->rgrc[0].top = std::max(params->rgrc[0].top, mi.rcWork.top);
                                        params->rgrc[0].right = std::min(params->rgrc[0].right, mi.rcWork.right);
                                        params->rgrc[0].bottom = std::min(params->rgrc[0].bottom, mi.rcWork.bottom);
                                    }
                                }
                            }
                        }
                        return 0;
                    }
                    break;
                }

                case WM_NCHITTEST: {
                    if (self->config.chrome == core::WindowChrome::System && !self->inFullscreen) {
                        return DefWindowProcW(window, message, wParam, lParam);
                    }
                    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    ScreenToClient(window, &pt);
                    RECT client{};
                    GetClientRect(window, &client);
                    if (pt.x < 0 || pt.y < 0 || pt.x >= (client.right - client.left) || pt.y >= (client.bottom - client.top)) {
                        return DefWindowProcW(window, message, wParam, lParam);
                    }
                    if (!self->callbacks.hitTest) return HTCLIENT;
                    const UINT dpi = GetDpiForWindow(window);
                    const float resizeBorder = (self->inFullscreen || IsZoomed(window))
                                                   ? 0.0f
                                                   : static_cast<float>(GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi));
                    const auto result = self->callbacks.hitTest({static_cast<float>(pt.x), static_cast<float>(pt.y)}, resizeBorder);
                    return toNativeHitResult(result);
                }

                case WM_NCMOUSEMOVE: {
                    if (self->config.chrome != core::WindowChrome::System) {
                        const LRESULT ht = static_cast<LRESULT>(wParam);
                        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                        ScreenToClient(window, &pt);
                        if (!self->trackingNcLeave) {
                            TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_NONCLIENT | TME_LEAVE, window, 0};
                            self->trackingNcLeave = TrackMouseEvent(&tme) != FALSE;
                        }
                        if (self->ncPressedButton != HTNOWHERE && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                            self->ncPressedButton = HTNOWHERE;
                            if (self->callbacks.pointerUp) self->callbacks.pointerUp(self->makeSample(pt));
                        }
                        if (isCaptionButtonHit(ht)) {
                            if (self->callbacks.pointerMove) self->callbacks.pointerMove(self->makeSample(pt));
                            return 0;
                        }
                        if (self->callbacks.pointerExit) self->callbacks.pointerExit();
                    }
                    break;
                }

                case WM_NCMOUSELEAVE: {
                    self->trackingNcLeave = false;
                    if (self->config.chrome != core::WindowChrome::System) {
                        if (self->ncPressedButton != HTNOWHERE && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                            self->ncPressedButton = HTNOWHERE;
                            if (self->callbacks.pointerCancelled) self->callbacks.pointerCancelled();
                        }
                        if (!self->trackingClientLeave && self->callbacks.pointerExit) {
                            self->callbacks.pointerExit();
                        }
                    }
                    return 0;
                }

                case WM_NCLBUTTONDOWN: {
                    const LRESULT ht = static_cast<LRESULT>(wParam);
                    if (self->config.chrome != core::WindowChrome::System && isCaptionButtonHit(ht)) {
                        // 拦截三键按下，不交给 DefWindowProc 画复古按钮；不调 SetCapture 以保持 Win11 Snap Layout 浮层可点击
                        self->ncPressedButton = ht;
                        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                        ScreenToClient(window, &pt);
                        if (self->callbacks.pointerDown) self->callbacks.pointerDown(self->makeSample(pt), {1, false});
                        return 0;
                    }
                    if (ht == HTCAPTION && !self->config.maximizable) {
                        // 保留拖拽但不在稍后双击时最大化
                    }
                    break;
                }

                case WM_NCLBUTTONDBLCLK: {
                    if (wParam == HTCAPTION && !self->config.maximizable) return 0;
                    break;
                }

                case WM_NCLBUTTONUP: {
                    const LRESULT ht = static_cast<LRESULT>(wParam);
                    if (self->config.chrome != core::WindowChrome::System && (isCaptionButtonHit(ht) || self->ncPressedButton != HTNOWHERE)) {
                        const LRESULT pressed = std::exchange(self->ncPressedButton, HTNOWHERE);
                        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                        ScreenToClient(window, &pt);
                        if (self->callbacks.pointerUp) self->callbacks.pointerUp(self->makeSample(pt));
                        if (pressed == ht) {
                            if (ht == HTMINBUTTON && self->config.minimizable) {
                                ShowWindow(window, SW_MINIMIZE);
                            } else if (ht == HTMAXBUTTON && self->config.maximizable) {
                                ShowWindow(window, IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE);
                            } else if (ht == HTCLOSE && self->config.closable && self->callbacks.closeRequested) {
                                self->callbacks.closeRequested();
                            }
                        }
                        return 0;
                    }
                    break;
                }

                case WM_GETMINMAXINFO: {
                    auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
                    const UINT dpi = window ? GetDpiForWindow(window) : 96u;
                    const double scale = dpi > 0 ? static_cast<double>(dpi) / 96.0 : self->dpiScale;
                    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
                    const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE));

                    if (self->config.minSizeDp) {
                        const int minW = static_cast<int>(std::lround(self->config.minSizeDp->width * scale));
                        const int minH = static_cast<int>(std::lround(self->config.minSizeDp->height * scale));
                        const RECT r = outerRectForClientSize(minW, minH, self->config, style, exStyle, dpi > 0 ? dpi : 96u);
                        info->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
                    } else {
                        info->ptMinTrackSize = {120, 80};
                    }
                    if (self->config.maxSizeDp) {
                        const int maxW = static_cast<int>(std::lround(self->config.maxSizeDp->width * scale));
                        const int maxH = static_cast<int>(std::lround(self->config.maxSizeDp->height * scale));
                        const RECT r = outerRectForClientSize(maxW, maxH, self->config, style, exStyle, dpi > 0 ? dpi : 96u);
                        info->ptMaxTrackSize = {r.right - r.left, r.bottom - r.top};
                    }
                    return 0;
                }

                case WM_DPICHANGED: {
                    const UINT newDpi = HIWORD(wParam);
                    if (newDpi > 0) {
                        self->dpiScale = static_cast<double>(newDpi) / 96.0;
                        if (self->callbacks.dpiChanged) self->callbacks.dpiChanged(self->dpiScale);
                    }
                    if (const auto* suggested = reinterpret_cast<const RECT*>(lParam)) {
                        self->internalGeometryChange = true;
                        SetWindowPos(
                            window,
                            nullptr,
                            suggested->left,
                            suggested->top,
                            suggested->right - suggested->left,
                            suggested->bottom - suggested->top,
                            SWP_NOZORDER | SWP_NOACTIVATE);
                        self->internalGeometryChange = false;
                        self->emitGeometrySnapshot();
                        self->updateImeCandidatePosition();
                    }
                    return 0;
                }

                case WM_SIZE: {
                    const int cw = LOWORD(lParam);
                    const int ch = HIWORD(lParam);
                    if (!self->constructing && wParam != SIZE_MINIMIZED && self->callbacks.resized) {
                        self->callbacks.resized(cw, ch);
                    }
                    if (!self->constructing && !self->internalGeometryChange) self->emitGeometrySnapshot();
                    self->updateImeCandidatePosition();
                    return 0;
                }

                case WM_MOVE: {
                    if (!self->constructing && !self->internalGeometryChange) self->emitGeometrySnapshot();
                    self->updateImeCandidatePosition();
                    return 0;
                }

                case WM_PAINT:
                    ValidateRect(window, nullptr);
                    if (!self->constructing && self->callbacks.paint) self->callbacks.paint();
                    return 0;

                case WM_ERASEBKGND:
                    return 1;

                case WM_SETCURSOR: {
                    if (LOWORD(lParam) == HTCLIENT) {
                        const auto icon = self->callbacks.pointerIcon ? self->callbacks.pointerIcon() : core::PointerIcon::Default;
                        SetCursor(cursorForIcon(icon));
                        return TRUE;
                    }
                    break;
                }

                case WM_MOUSEMOVE: {
                    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    if (!self->trackingClientLeave) {
                        TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, window, 0};
                        self->trackingClientLeave = TrackMouseEvent(&tme) != FALSE;
                    }
                    if (self->ncPressedButton != HTNOWHERE && (wParam & MK_LBUTTON) == 0) {
                        self->ncPressedButton = HTNOWHERE;
                        if (self->callbacks.pointerCancelled) self->callbacks.pointerCancelled();
                    }
                    const auto previousIcon = self->callbacks.pointerIcon ? self->callbacks.pointerIcon() : core::PointerIcon::Default;
                    if (self->callbacks.pointerMove) self->callbacks.pointerMove(self->makeSample(pt));
                    // WM_SETCURSOR precedes this hit test, so apply icon changes on the same move.
                    if (self->callbacks.pointerIcon) {
                        const auto icon = self->callbacks.pointerIcon();
                        if (icon != previousIcon) SetCursor(cursorForIcon(icon));
                    }
                    return 0;
                }

                case WM_MOUSELEAVE: {
                    self->trackingClientLeave = false;
                    if (GetCapture() != window && self->callbacks.pointerExit) {
                        self->callbacks.pointerExit();
                    }
                    return 0;
                }

                case WM_LBUTTONDOWN:
                case WM_LBUTTONDBLCLK: {
                    SetFocus(window);
                    SetCapture(window);
                    self->ncPressedButton = HTNOWHERE;
                    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    const int clicks = self->recordClickCount(pt);
                    const bool shift = (wParam & MK_SHIFT) != 0;
                    if (self->callbacks.pointerDown) self->callbacks.pointerDown(self->makeSample(pt), {clicks, shift});
                    return 0;
                }

                case WM_LBUTTONUP: {
                    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    self->ncPressedButton = HTNOWHERE;
                    if (self->callbacks.pointerUp) self->callbacks.pointerUp(self->makeSample(pt));
                    if (GetCapture() == window) {
                        self->releasingCapture = true;
                        ReleaseCapture();
                        self->releasingCapture = false;
                    }
                    return 0;
                }

                case WM_RBUTTONDOWN:
                case WM_MBUTTONDOWN: {
                    SetFocus(window);
                    return 0;
                }

                case WM_RBUTTONUP: {
                    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    if (self->callbacks.contextClick) self->callbacks.contextClick(self->makeSample(pt));
                    return 0;
                }

                case WM_CAPTURECHANGED: {
                    if (!self->releasingCapture && reinterpret_cast<HWND>(lParam) != window) {
                        if (self->pointerLocked) {
                            ClipCursor(nullptr);
                            ShowCursor(TRUE);
                            self->pointerLocked = false;
                        }
                        if (self->callbacks.pointerCancelled) self->callbacks.pointerCancelled();
                    }
                    return 0;
                }

                case WM_MOUSEWHEEL:
                case WM_MOUSEHWHEEL: {
                    POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                    ScreenToClient(window, &pt);
                    const short rawDelta = GET_WHEEL_DELTA_WPARAM(wParam);
                    if (rawDelta == 0) return 0;
                    const float notches = static_cast<float>(rawDelta) / static_cast<float>(WHEEL_DELTA);
                    const bool shift = (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0;

                    core::WheelInput wheel;
                    wheel.timeMillis = core::monotonicMillis();
                    if (message == WM_MOUSEHWHEEL) {
                        UINT charsPerNotch = 3;
                        SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &charsPerNotch, 0);
                        wheel.unitX = core::WheelUnit::Lines;
                        wheel.deltaX = -notches * static_cast<float>(std::max<UINT>(1, charsPerNotch));
                    } else if (shift) {
                        UINT linesPerNotch = 3;
                        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &linesPerNotch, 0);
                        if (linesPerNotch == WHEEL_PAGESCROLL) {
                            wheel.unitX = core::WheelUnit::Pages;
                            wheel.deltaX = notches;
                        } else if (linesPerNotch > 0) {
                            wheel.unitX = core::WheelUnit::Lines;
                            wheel.deltaX = notches * static_cast<float>(linesPerNotch);
                        }
                    } else {
                        UINT linesPerNotch = 3;
                        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &linesPerNotch, 0);
                        if (linesPerNotch == WHEEL_PAGESCROLL) {
                            wheel.unitY = core::WheelUnit::Pages;
                            wheel.deltaY = notches;
                        } else if (linesPerNotch > 0) {
                            wheel.unitY = core::WheelUnit::Lines;
                            wheel.deltaY = notches * static_cast<float>(linesPerNotch);
                        }
                    }
                    if ((wheel.deltaX != 0.0f || wheel.deltaY != 0.0f) && self->callbacks.wheel) {
                        self->callbacks.wheel({static_cast<float>(pt.x), static_cast<float>(pt.y)}, wheel);
                    }
                    return 0;
                }

                case WM_KEYDOWN:
                case WM_SYSKEYDOWN: {
                    if (wParam == VK_PROCESSKEY) break;
                    const auto mods = currentModifiers();
                    if (self->callbacks.modifiersChanged) self->callbacks.modifiersChanged(mods);
                    core::KeyInput key;
                    key.code = static_cast<int>(wParam);
                    key.key = translateVirtualKeyName(wParam, lParam);
                    key.modifiers = mods;
                    if (self->callbacks.keyDown && self->callbacks.keyDown(static_cast<int>(wParam), key)) {
                        return 0;
                    }
                    if (message == WM_SYSKEYDOWN) break;
                    return 0;
                }

                case WM_KEYUP:
                case WM_SYSKEYUP: {
                    const auto mods = currentModifiers();
                    if (self->callbacks.modifiersChanged) self->callbacks.modifiersChanged(mods);
                    const int releasedCode = static_cast<int>(wParam);
                    if (self->callbacks.keyStateChanged) {
                        (void)self->callbacks.keyStateChanged([releasedCode](int code) {
                            if (code == releasedCode) return false;
                            return (GetKeyState(code) & 0x8000) != 0;
                        });
                    }
                    if (message == WM_SYSKEYUP) {
                        if (wParam == VK_F10) return 0;
                        break;
                    }
                    return 0;
                }

                case WM_CHAR: {
                    const auto ch = static_cast<wchar_t>(wParam);
                    if (ch >= 0xd800u && ch <= 0xdbffu) {
                        self->pendingHighSurrogate = ch;
                        return 0;
                    }
                    char32_t scalar = 0;
                    if (ch >= 0xdc00u && ch <= 0xdfffu) {
                        if (self->pendingHighSurrogate != 0) {
                            scalar = 0x10000u + ((static_cast<char32_t>(self->pendingHighSurrogate - 0xd800u) << 10u) | static_cast<char32_t>(ch - 0xdc00u));
                            self->pendingHighSurrogate = 0;
                        } else {
                            return 0;
                        }
                    } else {
                        self->pendingHighSurrogate = 0;
                        scalar = static_cast<char32_t>(ch);
                    }
                    // 过滤控制字符与纯 Ctrl+键组合（保留 AltGr 即 Ctrl+Alt 可打印输入）
                    const bool ctrlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
                    const bool altDown = (GetKeyState(VK_MENU) & 0x8000) != 0;
                    if (scalar < 0x20u || scalar == 0x7fu || (ctrlDown && !altDown)) return 0;
                    if (self->callbacks.charInput) {
                        (void)self->callbacks.charInput(encodeCodepointUtf8(scalar));
                    }
                    return 0;
                }

                case WM_IME_SETCONTEXT: {
                    // 去掉系统默认悬浮组字框，由 Canton 自绘行内预编辑串与分段下划线
                    lParam &= ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW);
                    return DefWindowProcW(window, message, wParam, lParam);
                }

                case WM_IME_STARTCOMPOSITION: {
                    self->updateImeCandidatePosition();
                    return 0;
                }

                case WM_IME_COMPOSITION: {
                    self->handleImeComposition(lParam);
                    return 0;
                }

                case WM_IME_ENDCOMPOSITION: {
                    if (self->callbacks.imeEndComposition) (void)self->callbacks.imeEndComposition();
                    return 0;
                }

                case WM_SETFOCUS: {
                    self->pendingHighSurrogate = 0;
                    if (!self->constructing && self->callbacks.focusChanged) self->callbacks.focusChanged(true);
                    return 0;
                }

                case WM_KILLFOCUS: {
                    self->pendingHighSurrogate = 0;
                    self->ncPressedButton = HTNOWHERE;
                    if (self->pointerLocked) {
                        ClipCursor(nullptr);
                        ShowCursor(TRUE);
                        self->pointerLocked = false;
                    }
                    if (!self->constructing && self->callbacks.focusChanged) self->callbacks.focusChanged(false);
                    return 0;
                }

                case WM_FONTCHANGE:
                    if (self->callbacks.fontsChanged) self->callbacks.fontsChanged();
                    return 0;

                case WM_QUERYENDSESSION:
                    return TRUE;

                case WM_ENDSESSION:
                    if (wParam == TRUE && !endSessionHandled.exchange(true)) {
                        if (self->callbacks.endSession) self->callbacks.endSession();
                    }
                    return 0;

                case WM_DESTROY:
                    if (self->precisionWheel) self->precisionWheel->cancel();
                    self->hwnd = nullptr;
                    return 0;

                default:
                    break;
            }
            return DefWindowProcW(window, message, wParam, lParam);
        }
    };

    std::atomic<bool> Window::Impl::endSessionHandled{false};

    Window::Window(const core::WindowConfigSpec& config, void* ownerHwnd, WindowCallbacks callbacks) : impl_(std::make_unique<Impl>()) {
        impl_->config = config;
        impl_->ownerHwnd = static_cast<HWND>(ownerHwnd);
        impl_->callbacks = std::move(callbacks);

        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = Impl::procedure;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        wc.lpszClassName = windowClassName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            throw std::runtime_error("无法登记 CantonHostWindow 窗口类");
        }

        const UINT initialDpi = queryDpiForMonitorOrSystem(impl_->ownerHwnd);
        impl_->dpiScale = static_cast<double>(initialDpi) / 96.0;

        DWORD style = 0, exStyle = 0;
        computeWindowStyles(config, impl_->ownerHwnd != nullptr, style, exStyle);

        const int clientW = std::max(1, static_cast<int>(std::lround(config.sizeDp.width * impl_->dpiScale)));
        const int clientH = std::max(1, static_cast<int>(std::lround(config.sizeDp.height * impl_->dpiScale)));
        const RECT outer = outerRectForClientSize(clientW, clientH, config, style, exStyle, initialDpi);
        const int outerW = outer.right - outer.left;
        const int outerH = outer.bottom - outer.top;

        int posX = CW_USEDEFAULT, posY = CW_USEDEFAULT;
        impl_->computeInitialPosition(outerW, outerH, posX, posY);

        const auto wideTitle = utf8ToWide(config.title);
        impl_->constructing = true;
        impl_->internalGeometryChange = true;
        impl_->hwnd = CreateWindowExW(
            exStyle,
            windowClassName,
            wideTitle.c_str(),
            style,
            posX,
            posY,
            outerW,
            outerH,
            impl_->ownerHwnd,
            nullptr,
            instance,
            impl_.get());
        impl_->internalGeometryChange = false;
        if (!impl_->hwnd) {
            impl_->constructing = false;
            throw std::runtime_error("无法创建 Win32 窗口");
        }

        const UINT actualDpi = GetDpiForWindow(impl_->hwnd);
        if (actualDpi > 0) impl_->dpiScale = static_cast<double>(actualDpi) / 96.0;

        // 默认关闭输入法上下文，仅在聚焦可编辑 <Input> 时动态开启
        impl_->savedImc = ImmAssociateContext(impl_->hwnd, nullptr);
        impl_->imeEnabled = false;

        impl_->precisionWheel = std::make_unique<WindowsPrecisionWheelSource>(
            [self = impl_.get()](core::Point pt, const core::WheelInput& input) {
                if (self->callbacks.wheel) self->callbacks.wheel(pt, input);
            },
            [self = impl_.get()] {
                if (self->callbacks.wake) self->callbacks.wake();
            });

        impl_->applyDwmAttributes();
        impl_->syncCloseMenuItem();

        // 触发 SWP_FRAMECHANGED 让 WM_NCCALCSIZE(TRUE) 正式生效，并按窗口所在显示器的真实 DPI 精确校准客户区像素尺寸
        if (config.placement == core::WindowPlacement::Normal) {
            const int adjustedClientW = std::max(1, static_cast<int>(std::lround(config.sizeDp.width * impl_->dpiScale)));
            const int adjustedClientH = std::max(1, static_cast<int>(std::lround(config.sizeDp.height * impl_->dpiScale)));
            const RECT adjustedOuter = outerRectForClientSize(adjustedClientW, adjustedClientH, config, style, exStyle, actualDpi > 0 ? actualDpi : initialDpi);
            const int targetW = adjustedOuter.right - adjustedOuter.left;
            const int targetH = adjustedOuter.bottom - adjustedOuter.top;
            int targetX = 0, targetY = 0;
            UINT posFlags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED;
            if (config.position.preset != core::WindowPositionPreset::PlatformDefault) {
                impl_->computeInitialPosition(targetW, targetH, targetX, targetY);
            } else {
                posFlags |= SWP_NOMOVE;
            }
            impl_->internalGeometryChange = true;
            SetWindowPos(impl_->hwnd, nullptr, targetX, targetY, targetW, targetH, posFlags);
            impl_->internalGeometryChange = false;
        } else {
            impl_->internalGeometryChange = true;
            SetWindowPos(impl_->hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            impl_->internalGeometryChange = false;
        }

        impl_->applyPlacementAndVisibility(true);
        impl_->constructing = false;
        RECT initialClient{};
        GetClientRect(impl_->hwnd, &initialClient);
        if (impl_->precisionWheel && initialClient.right > initialClient.left && initialClient.bottom > initialClient.top) {
            impl_->precisionWheel->sync(impl_->hwnd, initialClient, 1.0f);
        }
    }

    Window::~Window() {
        if (impl_ && impl_->hwnd) {
            if (impl_->pointerLocked) {
                ClipCursor(nullptr);
                ShowCursor(TRUE);
                impl_->pointerLocked = false;
            }
            if (impl_->precisionWheel) impl_->precisionWheel->cancel();
            if (!impl_->imeEnabled && impl_->savedImc) {
                ImmAssociateContext(impl_->hwnd, impl_->savedImc);
            }
            SetWindowLongPtrW(impl_->hwnd, GWLP_USERDATA, 0);
            DestroyWindow(impl_->hwnd);
            impl_->hwnd = nullptr;
        }
    }

    void* Window::handle() const noexcept {
        return impl_->hwnd;
    }

    void* Window::ownerHandle() const noexcept {
        return impl_->ownerHwnd;
    }

    const core::WindowConfigSpec& Window::config() const noexcept {
        return impl_->config;
    }

    bool Window::backdropActive() const noexcept {
        return impl_->backdropActive;
    }

    double Window::dpiScale() const noexcept {
        return impl_->dpiScale;
    }

    void Window::clientSize(int& widthPx, int& heightPx) const {
        RECT client{};
        if (impl_->hwnd) GetClientRect(impl_->hwnd, &client);
        widthPx = client.right - client.left;
        heightPx = client.bottom - client.top;
    }

    void Window::updateConfig(const core::WindowConfigSpec& next) {
        if (!impl_->hwnd) return;
        const auto prev = impl_->config;
        impl_->config = next;

        if (prev.title != next.title) {
            const auto wide = utf8ToWide(next.title);
            SetWindowTextW(impl_->hwnd, wide.c_str());
        }

        const bool styleChanged = prev.chrome != next.chrome || prev.role != next.role || prev.resizable != next.resizable || prev.minimizable != next.minimizable || prev.maximizable != next.maximizable || prev.alwaysOnTop != next.alwaysOnTop || prev.showInTaskbar != next.showInTaskbar;
        if (styleChanged && !impl_->inFullscreen) {
            DWORD style = 0, exStyle = 0;
            computeWindowStyles(next, impl_->ownerHwnd != nullptr, style, exStyle);
            if (IsWindowVisible(impl_->hwnd)) style |= WS_VISIBLE;
            if (IsZoomed(impl_->hwnd)) style |= WS_MAXIMIZE;
            if (IsIconic(impl_->hwnd)) style |= WS_MINIMIZE;
            impl_->internalGeometryChange = true;
            SetWindowLongPtrW(impl_->hwnd, GWL_STYLE, style);
            SetWindowLongPtrW(impl_->hwnd, GWL_EXSTYLE, exStyle);
            SetWindowPos(
                impl_->hwnd,
                next.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
            impl_->internalGeometryChange = false;
        } else if (prev.alwaysOnTop != next.alwaysOnTop) {
            SetWindowPos(impl_->hwnd, next.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }

        if (prev.closable != next.closable || prev.resizable != next.resizable || prev.maximizable != next.maximizable || prev.minimizable != next.minimizable) {
            impl_->syncCloseMenuItem();
        }

        if (prev.backdrop != next.backdrop || prev.cornerPreference != next.cornerPreference || prev.chrome != next.chrome || prev.transparent != next.transparent) {
            impl_->applyDwmAttributes();
        }

        // 尺寸与位置在 normal 形态下响应脚本侧变更
        const bool sizeChanged = std::abs(prev.sizeDp.width - next.sizeDp.width) > 0.01f || std::abs(prev.sizeDp.height - next.sizeDp.height) > 0.01f;
        const bool posChanged = prev.position != next.position;
        if ((sizeChanged || posChanged) && next.placement == core::WindowPlacement::Normal && !impl_->inFullscreen) {
            if (IsIconic(impl_->hwnd) || IsZoomed(impl_->hwnd)) ShowWindow(impl_->hwnd, SW_RESTORE);
            const UINT dpi = GetDpiForWindow(impl_->hwnd);
            const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(impl_->hwnd, GWL_STYLE));
            const DWORD exStyle = static_cast<DWORD>(GetWindowLongPtrW(impl_->hwnd, GWL_EXSTYLE));
            const int cw = std::max(1, static_cast<int>(std::lround(next.sizeDp.width * impl_->dpiScale)));
            const int ch = std::max(1, static_cast<int>(std::lround(next.sizeDp.height * impl_->dpiScale)));
            const RECT outer = outerRectForClientSize(cw, ch, next, style, exStyle, dpi > 0 ? dpi : 96u);
            const int ow = outer.right - outer.left;
            const int oh = outer.bottom - outer.top;

            UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
            int x = 0, y = 0;
            if (posChanged && next.position.preset != core::WindowPositionPreset::PlatformDefault) {
                impl_->computeInitialPosition(ow, oh, x, y);
            } else {
                flags |= SWP_NOMOVE;
            }
            if (!sizeChanged) flags |= SWP_NOSIZE;
            impl_->internalGeometryChange = true;
            SetWindowPos(impl_->hwnd, nullptr, x, y, ow, oh, flags);
            impl_->internalGeometryChange = false;
            impl_->emitGeometrySnapshot();
        }

        if (prev.placement != next.placement || prev.visible != next.visible || prev.showInTaskbar != next.showInTaskbar) {
            impl_->applyPlacementAndVisibility(false);
        }
    }

    void Window::setIcons(void* bigHicon, void* smallHicon) {
        if (!impl_->hwnd) return;
        SendMessageW(impl_->hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigHicon));
        SendMessageW(impl_->hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallHicon));
    }

    void Window::activate() {
        if (!impl_->hwnd) return;
        if (IsIconic(impl_->hwnd)) ShowWindow(impl_->hwnd, SW_RESTORE);
        SetForegroundWindow(impl_->hwnd);
        SetActiveWindow(impl_->hwnd);
        SetFocus(impl_->hwnd);
    }

    void Window::setEnabled(bool enabled) {
        if (impl_->hwnd) EnableWindow(impl_->hwnd, enabled ? TRUE : FALSE);
    }

    bool Window::isEnabled() const noexcept {
        return impl_->hwnd && IsWindowEnabled(impl_->hwnd) != FALSE;
    }

    void Window::applyPointerEffects(const core::PointerPlatformEffects& effects) {
        if (!impl_->hwnd) return;
        if (effects.lockPointer && !impl_->pointerLocked) {
            RECT client{};
            GetClientRect(impl_->hwnd, &client);
            POINT tl{client.left, client.top}, br{client.right, client.bottom};
            ClientToScreen(impl_->hwnd, &tl);
            ClientToScreen(impl_->hwnd, &br);
            const RECT clip{tl.x, tl.y, br.x, br.y};
            ClipCursor(&clip);
            ShowCursor(FALSE);
            impl_->pointerLocked = true;
        } else if (effects.unlockPointer && impl_->pointerLocked) {
            ClipCursor(nullptr);
            POINT pt{static_cast<LONG>(std::lround(effects.restorePoint.x)), static_cast<LONG>(std::lround(effects.restorePoint.y))};
            ClientToScreen(impl_->hwnd, &pt);
            SetCursorPos(pt.x, pt.y);
            ShowCursor(TRUE);
            impl_->pointerLocked = false;
        }
    }

    void Window::setImeEnabled(bool enabled) {
        if (!impl_->hwnd || impl_->imeEnabled == enabled) return;
        impl_->imeEnabled = enabled;
        if (enabled) {
            if (impl_->savedImc)
                ImmAssociateContext(impl_->hwnd, impl_->savedImc);
            else
                ImmAssociateContextEx(impl_->hwnd, nullptr, IACE_DEFAULT);
            impl_->updateImeCandidatePosition();
        } else {
            cancelImeComposition();
            const HIMC prev = ImmAssociateContext(impl_->hwnd, nullptr);
            if (prev) impl_->savedImc = prev;
        }
    }

    bool Window::imeEnabled() const noexcept {
        return impl_->imeEnabled;
    }

    void Window::setImeCaretBounds(core::Rect caretPx) {
        impl_->imeCaretRect = caretPx;
        impl_->updateImeCandidatePosition();
    }

    void Window::cancelImeComposition() {
        if (!impl_->hwnd) return;
        if (HIMC imc = ImmGetContext(impl_->hwnd)) {
            (void)ImmNotifyIME(imc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
            ImmReleaseContext(impl_->hwnd, imc);
        }
    }

    void Window::tickPrecisionWheel() {
        if (impl_->precisionWheel) impl_->precisionWheel->tick();
    }

    bool Window::needsPrecisionWheelTicks() const noexcept {
        return impl_->precisionWheel && impl_->precisionWheel->needsTicks();
    }

    long Window::sendHitTestAtClientPoint(int clientX, int clientY) const {
        if (!impl_->hwnd) return HTNOWHERE;
        POINT screen{clientX, clientY};
        ClientToScreen(impl_->hwnd, &screen);
        const LPARAM packed = MAKELPARAM(screen.x, screen.y);
        return static_cast<long>(SendMessageW(impl_->hwnd, WM_NCHITTEST, 0, packed));
    }

}
