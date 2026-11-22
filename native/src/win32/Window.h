#pragma once

#include <functional>
#include <memory>
#include <string>

namespace canton::win32 {
    struct WindowCallbacks {
        // 客户区尺寸变化（物理像素）；最小化时不调用
        std::function<void(int width, int height)> resized;
        // 系统要求重画
        std::function<void()> paint;
        // postWake 投来的唤醒，在窗口线程上执行
        std::function<void()> wake;
        // 系统字体安装或卸载
        std::function<void()> fontsChanged;
    };

    // 只负责显示的顶层窗口：能缩放、能关闭，不接任何输入。关闭后消息循环结束
    class Window {
       public:
        // width、height 是客户区的初始尺寸
        Window(const std::wstring& title, int width, int height, WindowCallbacks callbacks);
        ~Window();
        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        // HWND
        [[nodiscard]] void* handle() const noexcept;
        void show();
        void clientSize(int& width, int& height) const;
        // 可从任意线程调用；处理前重复投递只算一次
        void postWake() noexcept;

        // 跑到窗口关闭为止，返回退出码
        static int runMessageLoop();

       private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
