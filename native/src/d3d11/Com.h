#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wrl/client.h>
// windows.h 把 DrawText 定义成宏，与 core 的 DrawOpType::DrawText 撞名
#undef DrawText

#include <stdexcept>
#include <string>
#include <string_view>

namespace canton::d3d11 {
    template <class T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    // 平台调用失败：带上 HRESULT，设备丢失由调用方按码识别
    class PlatformError : public std::runtime_error {
       public:
        PlatformError(const std::string& what, HRESULT code) : std::runtime_error(what + "（HRESULT 0x" + hex(code) + "）"), code_(code) {}

        [[nodiscard]] HRESULT code() const noexcept {
            return code_;
        }

       private:
        static std::string hex(HRESULT code) {
            constexpr char digits[] = "0123456789abcdef";
            std::string text(8, '0');
            auto value = static_cast<unsigned long>(code);
            for (int i = 7; i >= 0; --i, value >>= 4) text[static_cast<std::size_t>(i)] = digits[value & 15];
            return text;
        }

        HRESULT code_;
    };

    inline void check(HRESULT result, const char* what) {
        if (FAILED(result)) throw PlatformError(what, result);
    }

    [[nodiscard]] std::wstring widen(std::string_view utf8);
    [[nodiscard]] std::string narrow(std::wstring_view utf16);
}
