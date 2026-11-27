#pragma once

#include <core/PlatformTypes.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace canton::runtime {

    // 单实例互斥与命名管道热唤醒（ADR B1：5）：
    // 按 AUMID 建立进程级命名互斥体与命名管道。第二实例投递 LaunchRequest、调用 AllowSetForegroundWindow 后退出码 0 离场。
    class SingleInstanceGuard final {
       public:
        SingleInstanceGuard() = default;
        ~SingleInstanceGuard();
        SingleInstanceGuard(const SingleInstanceGuard&) = delete;
        SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

        // 返回 true 表示当前进程成为主实例并已启动 IPC 监听；
        // 返回 false 表示已有主实例存活且本进程已将 request 投递并移交前台权限，调用方应立刻退出（退出码 0）。
        bool acquireOrForward(
            const std::string& appKey,
            const core::LaunchRequest& request,
            std::function<void(core::LaunchRequest)> onSecondInstance);

        void stop();

        [[nodiscard]] bool isPrimary() const noexcept { return primary_; }

       private:
        void listenLoop();

        bool primary_ = false;
        void* mutexHandle_ = nullptr;
        void* stopEvent_ = nullptr;
        std::wstring pipeName_;
        std::function<void(core::LaunchRequest)> callback_;
        std::atomic<bool> stopping_{false};
        std::thread listenerThread_;
    };

}
