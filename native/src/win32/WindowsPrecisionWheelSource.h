#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <core/Geometry.h>
#include <core/WheelInput.h>

namespace canton::win32 {
    class WindowsPrecisionWheelSource final {
       public:
        using InputCallback = std::function<void(canton::core::Point, const canton::core::WheelInput&)>;
        using WakeCallback = std::function<void()>;

        WindowsPrecisionWheelSource(InputCallback onInput, WakeCallback onWake);
        ~WindowsPrecisionWheelSource();
        WindowsPrecisionWheelSource(const WindowsPrecisionWheelSource&) = delete;
        WindowsPrecisionWheelSource& operator=(const WindowsPrecisionWheelSource&) = delete;
        // Owner 线程传入 HWND、客户区像素矩形及每逻辑单位的像素数。
        void sync(HWND window, RECT clientBounds, float scale);
        void tick();
        bool needsTicks() const noexcept;
        void cancel();

       private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}

