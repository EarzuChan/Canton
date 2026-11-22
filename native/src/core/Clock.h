#pragma once

#include <chrono>

namespace canton::core {
    inline double monotonicMillis() noexcept {
        using namespace std::chrono;
        return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
    }
}
