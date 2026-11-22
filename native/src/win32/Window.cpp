#include <win32/Window.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <stdexcept>

namespace canton::win32 {
    namespace {
        constexpr wchar_t className[] = L"CantonHostWindow";
        constexpr UINT wakeMessage = WM_APP + 1;
    }

    struct Window::Impl {
        HWND window = nullptr;
        WindowCallbacks callbacks;
        std::atomic<bool> wakePosted{false};

        static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
            if (message == WM_NCCREATE) SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams));
            auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (!self) return DefWindowProcW(window, message, wParam, lParam);
            switch (message) {
                case WM_SIZE:
                    if (wParam != SIZE_MINIMIZED && self->callbacks.resized) self->callbacks.resized(LOWORD(lParam), HIWORD(lParam));
                    return 0;
                case WM_PAINT:
                    ValidateRect(window, nullptr);
                    if (self->callbacks.paint) self->callbacks.paint();
                    return 0;
                case WM_ERASEBKGND:
                    return 1;
                case WM_FONTCHANGE:
                    if (self->callbacks.fontsChanged) self->callbacks.fontsChanged();
                    return 0;
                case WM_GETMINMAXINFO: {
                    auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
                    info->ptMinTrackSize = {160, 120};
                    return 0;
                }
                case WM_DESTROY:
                    self->window = nullptr;
                    PostQuitMessage(0);
                    return 0;
                default:
                    if (message == wakeMessage) {
                        // 先清标志再处理：处理期间新来的唤醒不会丢
                        self->wakePosted.store(false, std::memory_order_release);
                        if (self->callbacks.wake) self->callbacks.wake();
                        return 0;
                    }
            }
            return DefWindowProcW(window, message, wParam, lParam);
        }
    };

    Window::Window(const std::wstring& title, int width, int height, WindowCallbacks callbacks) : impl_(std::make_unique<Impl>()) {
        impl_->callbacks = std::move(callbacks);
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = Impl::procedure;
        description.hInstance = instance;
        description.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        description.lpszClassName = className;
        if (!RegisterClassExW(&description) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("无法登记窗口类");
        constexpr DWORD style = WS_OVERLAPPEDWINDOW;
        // 画面由 DirectComposition 合成，不需要系统的重定向位图
        constexpr DWORD extended = WS_EX_NOREDIRECTIONBITMAP;
        RECT frame{0, 0, width, height};
        AdjustWindowRectEx(&frame, style, FALSE, extended);
        impl_->window = CreateWindowExW(extended, className, title.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr, instance, impl_.get());
        if (!impl_->window) throw std::runtime_error("无法创建窗口");
    }

    Window::~Window() {
        if (impl_->window) {
            SetWindowLongPtrW(impl_->window, GWLP_USERDATA, 0);
            DestroyWindow(impl_->window);
        }
    }

    void* Window::handle() const noexcept {
        return impl_->window;
    }

    void Window::show() {
        ShowWindow(impl_->window, SW_SHOWNORMAL);
        UpdateWindow(impl_->window);
    }

    void Window::clientSize(int& width, int& height) const {
        RECT client{};
        if (impl_->window) GetClientRect(impl_->window, &client);
        width = client.right - client.left;
        height = client.bottom - client.top;
    }

    void Window::postWake() noexcept {
        const auto window = impl_->window;
        if (window && !impl_->wakePosted.exchange(true, std::memory_order_acq_rel)) PostMessageW(window, wakeMessage, 0, 0);
    }

    int Window::runMessageLoop() {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }
}
