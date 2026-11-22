#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace canton::runtime {
    // SHA-256 的小写十六进制，系统 BCrypt 计算
    [[nodiscard]] std::string sha256Hex(std::span<const std::uint8_t> bytes);
    [[nodiscard]] std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept;
}
