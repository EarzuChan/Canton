#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace canton::runtime {

    // 共享后台 I/O 线程池：供文件存储与 WinHTTP 异步请求共用，析构时有序排空停止。
    class IoThreadPool final {
       public:
        explicit IoThreadPool(std::size_t threadCount = 4);
        ~IoThreadPool();
        IoThreadPool(const IoThreadPool&) = delete;
        IoThreadPool& operator=(const IoThreadPool&) = delete;

        // 提交异步任务；若线程池已开始关闭则返回 false
        bool post(std::function<void()> task);

        // 触发关闭并等待工作线程有序退出
        void shutdown();

        [[nodiscard]] bool stopping() const noexcept {
            return stopping_.load(std::memory_order_acquire);
        }

       private:
        void workerLoop();

        std::mutex mutex_;
        std::condition_variable cv_;
        std::deque<std::function<void()>> queue_;
        std::vector<std::thread> workers_;
        std::atomic<bool> stopping_{false};
        bool joined_ = false;
    };

}
