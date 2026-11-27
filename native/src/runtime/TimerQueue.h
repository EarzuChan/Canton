#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <queue>
#include <unordered_map>
#include <vector>

namespace canton::runtime {

    // 单调时钟最小堆定时队列：仅在 UI Owner 线程操作，支撑宿主事件循环精准定时唤醒与 HMR 整体清空。
    class TimerQueue final {
       public:
        struct DueTimer {
            std::uint64_t id = 0;
            bool repeating = false;
            double intervalMs = 0.0;
            std::function<void()> callback;
        };

        // 安排单次或周期定时器，返回正整数 ID
        std::uint64_t schedule(double nowMillis, double delayMs, bool repeating, std::function<void()> callback);

        // 周期定时器在单次回调返回后安排下一拍，避免慢回调积压连环重入
        void rescheduleAfterRun(std::uint64_t id, double nowMillis);

        // 幂等取消；无效或已取消 ID 静默忽略
        bool cancel(std::uint64_t id) noexcept;

        // 一次性清零全部活跃定时器（HMR / VM 重载 / 退出）
        void clear() noexcept;

        // 距离最近到期定时项的等待毫秒数；无活跃定时项返回 std::nullopt
        [[nodiscard]] std::optional<std::uint32_t> nextWaitMillis(double nowMillis) const noexcept;

        // 弹出所有已到期项（按到期时刻与提交序升序）
        [[nodiscard]] std::vector<DueTimer> popDue(double nowMillis);

        [[nodiscard]] bool empty() const noexcept {
            return entries_.empty();
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return entries_.size();
        }

        [[nodiscard]] bool active(std::uint64_t id) const noexcept {
            return entries_.contains(id);
        }

       private:
        struct Entry {
            std::uint64_t id = 0;
            std::uint64_t generation = 0;
            double intervalMs = 0.0;
            bool repeating = false;
            bool inFlight = false;
            std::function<void()> callback;
        };

        struct HeapItem {
            double dueMillis = 0.0;
            std::uint64_t sequence = 0;
            std::uint64_t id = 0;
            std::uint64_t generation = 0;
        };

        struct HeapCompare {
            bool operator()(const HeapItem& a, const HeapItem& b) const noexcept {
                if (a.dueMillis != b.dueMillis) return a.dueMillis > b.dueMillis;
                return a.sequence > b.sequence;
            }
        };

        void pruneTop() const noexcept;

        std::uint64_t nextId_ = 1;
        std::uint64_t nextSequence_ = 1;
        std::unordered_map<std::uint64_t, Entry> entries_;
        mutable std::priority_queue<HeapItem, std::vector<HeapItem>, HeapCompare> heap_;
    };

}
