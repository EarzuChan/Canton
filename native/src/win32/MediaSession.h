#pragma once

#include <core/PlatformTypes.h>

#include <functional>
#include <memory>
#include <string>

namespace canton::win32 {

    // 系统全局媒体控制（Windows SMTC）：纯 Windows SDK WinRT COM ABI，绑定 App 级隐藏顶层 HWND。
    // 全部业务窗口关闭驻留托盘期间，仍可响应系统媒体中心/锁屏卡片与物理媒体键。
    class MediaSessionController final {
       public:
        using ButtonCallback = std::function<void(core::MediaButtonKind)>;

        explicit MediaSessionController(void* appHwnd, ButtonCallback onButton = {});
        ~MediaSessionController();
        MediaSessionController(const MediaSessionController&) = delete;
        MediaSessionController& operator=(const MediaSessionController&) = delete;

        // 同步媒体会话状态；thumbnailDiskPath 为可直接由系统读取的磁盘文件路径（可空）
        void update(const core::MediaSessionSpec& spec, const std::string& thumbnailDiskPath = {});
        void deactivate();

        [[nodiscard]] bool available() const noexcept;

       private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}
