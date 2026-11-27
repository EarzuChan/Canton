#include <runtime/TimerQueue.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace canton::runtime {

    namespace {
        double sanitizeDelay(double delayMs) noexcept {
            if (!std::isfinite(delayMs) || delayMs <= 0.0) return 0.0;
            return delayMs;
        }
    }

    std::uint64_t TimerQueue::schedule(double nowMillis, double delayMs, bool repeating, std::function<void()> callback) {
        const double sanitized = sanitizeDelay(delayMs);
        const std::uint64_t id = nextId_++;
        const std::uint64_t generation = 1;
        entries_.emplace(id, Entry{id, generation, sanitized, repeating, false, std::move(callback)});
        heap_.push(HeapItem{nowMillis + sanitized, nextSequence_++, id, generation});
        return id;
    }

    void TimerQueue::rescheduleAfterRun(std::uint64_t id, double nowMillis) {
        const auto it = entries_.find(id);
        if (it == entries_.end()) return;
        auto& entry = it->second;
        if (!entry.repeating) {
            entries_.erase(it);
            return;
        }
        entry.inFlight = false;
        ++entry.generation;
        heap_.push(HeapItem{nowMillis + entry.intervalMs, nextSequence_++, id, entry.generation});
    }

    bool TimerQueue::cancel(std::uint64_t id) noexcept {
        return entries_.erase(id) > 0;
    }

    void TimerQueue::clear() noexcept {
        entries_.clear();
        while (!heap_.empty()) heap_.pop();
    }

    void TimerQueue::pruneTop() const noexcept {
        while (!heap_.empty()) {
            const auto& top = heap_.top();
            const auto it = entries_.find(top.id);
            if (it == entries_.end() || it->second.generation != top.generation || it->second.inFlight) {
                heap_.pop();
                continue;
            }
            break;
        }
    }

    std::optional<std::uint32_t> TimerQueue::nextWaitMillis(double nowMillis) const noexcept {
        pruneTop();
        if (heap_.empty()) return std::nullopt;
        const double remaining = heap_.top().dueMillis - nowMillis;
        if (remaining <= 0.0) return 0u;
        const double clamped = std::min(remaining, static_cast<double>(std::numeric_limits<std::uint32_t>::max() - 1));
        return static_cast<std::uint32_t>(std::ceil(clamped));
    }

    std::vector<TimerQueue::DueTimer> TimerQueue::popDue(double nowMillis) {
        std::vector<DueTimer> due;
        while (true) {
            pruneTop();
            if (heap_.empty()) break;
            if (heap_.top().dueMillis > nowMillis) break;
            const auto item = heap_.top();
            heap_.pop();
            const auto it = entries_.find(item.id);
            if (it == entries_.end() || it->second.generation != item.generation || it->second.inFlight) continue;
            auto& entry = it->second;
            if (entry.repeating) {
                entry.inFlight = true;
                due.push_back(DueTimer{entry.id, true, entry.intervalMs, entry.callback});
            } else {
                DueTimer result{entry.id, false, entry.intervalMs, std::move(entry.callback)};
                entries_.erase(it);
                due.push_back(std::move(result));
            }
        }
        return due;
    }

}
