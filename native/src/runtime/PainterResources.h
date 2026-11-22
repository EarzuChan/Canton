#pragma once

#include <core/Font.h>
#include <cstddef>
#include <span>

namespace canton::runtime {
    struct PainterResourceLimits {
        static constexpr std::size_t MaxResourceBytes = 16 * 1024 * 1024;
        static constexpr int MaxTimeoutMs = 5000;
        std::size_t maxBytes = MaxResourceBytes;
        int timeoutMs = MaxTimeoutMs;
    };

    std::shared_ptr<const core::FontContent> decodeFontResource(std::span<const std::uint8_t> payload, std::string_view source);
}
