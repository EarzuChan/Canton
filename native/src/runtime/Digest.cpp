#include <runtime/Digest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bcrypt.h>

#include <array>
#include <stdexcept>

namespace canton::runtime {
    std::string sha256Hex(std::span<const std::uint8_t> bytes) {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) throw std::runtime_error("无法打开 SHA-256 算法");
        std::array<UCHAR, 32> digest{};
        BCRYPT_HASH_HANDLE hash = nullptr;
        auto ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
        // 单次调用的长度是 32 位，分块喂
        for (std::size_t offset = 0; ok && offset < bytes.size();) {
            const auto chunk = static_cast<ULONG>(std::min<std::size_t>(bytes.size() - offset, 1u << 30));
            ok = BCryptHashData(hash, const_cast<PUCHAR>(bytes.data() + offset), chunk, 0) >= 0;
            offset += chunk;
        }
        ok = ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0;
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        if (!ok) throw std::runtime_error("SHA-256 计算失败");
        constexpr char digits[] = "0123456789abcdef";
        std::string text;
        text.reserve(64);
        for (const auto byte : digest) {
            text += digits[byte >> 4];
            text += digits[byte & 15];
        }
        return text;
    }

    std::uint32_t crc32(std::span<const std::uint8_t> bytes) noexcept {
        static const auto table = [] {
            std::array<std::uint32_t, 256> values{};
            for (std::uint32_t i = 0; i < 256; ++i) {
                auto value = i;
                for (int bit = 0; bit < 8; ++bit) value = (value & 1) ? 0xedb88320u ^ (value >> 1) : value >> 1;
                values[i] = value;
            }
            return values;
        }();
        auto crc = 0xffffffffu;
        for (const auto byte : bytes) crc = table[(crc ^ byte) & 0xff] ^ (crc >> 8);
        return crc ^ 0xffffffffu;
    }
}
