#include <core/SlotUpdate.h>

#include <atomic>

namespace canton::core {
    std::uint64_t allocateRuntimeIdentity() {
        static std::atomic<std::uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }
}
