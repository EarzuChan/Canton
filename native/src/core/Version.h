#pragma once

namespace canton::core {
    inline constexpr unsigned RuntimeVersion = 1u;
    inline constexpr unsigned ProtocolVersion = RuntimeVersion;
    inline constexpr const char* PackageVersion = "0.1.0";
    const char* version() noexcept;
}
