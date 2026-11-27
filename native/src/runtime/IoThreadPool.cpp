#include <runtime/IoThreadPool.h>

#include <algorithm>

namespace canton::runtime {

    IoThreadPool::IoThreadPool(std::size_t threadCount) {
        const auto count = std::max<std::size_t>(1, threadCount);
        workers_.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            workers_.emplace_back([this] { workerLoop(); });
        }
    }

    IoThreadPool::~IoThreadPool() {
        shutdown();
    }

    bool IoThreadPool::post(std::function<void()> task) {
        if (!task) return false;
        {
            const std::scoped_lock lock(mutex_);
            if (stopping_.load(std::memory_order_relaxed)) return false;
            queue_.push_back(std::move(task));
        }
        cv_.notify_one();
        return true;
    }

    void IoThreadPool::shutdown() {
        {
            const std::scoped_lock lock(mutex_);
            if (joined_) return;
            stopping_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
        for (auto& worker : workers_) {
            if (worker.joinable()) worker.join();
        }
        const std::scoped_lock lock(mutex_);
        joined_ = true;
    }

    void IoThreadPool::workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] {
                    return stopping_.load(std::memory_order_acquire) || !queue_.empty();
                });
                if (queue_.empty()) {
                    if (stopping_.load(std::memory_order_acquire)) return;
                    continue;
                }
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            if (task) task();
        }
    }

}
