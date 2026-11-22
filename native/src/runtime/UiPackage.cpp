#include <runtime/UiPackage.h>

#include <charconv>
#include <stdexcept>

namespace canton::runtime {
    namespace {
        bool validUtf8(std::string_view bytes) {
            while (!bytes.empty()) {
                const auto lead = static_cast<unsigned char>(bytes.front());
                bytes.remove_prefix(1);
                if (lead < 0x80) continue;
                if (lead < 0xc2 || lead > 0xf4) return false;
                const auto count = lead < 0xe0 ? 1u : lead < 0xf0 ? 2u : 3u;
                if (bytes.size() < count) return false;
                const auto first = static_cast<unsigned char>(bytes.front());
                if ((lead == 0xe0 && first < 0xa0) || (lead == 0xed && first > 0x9f) ||
                    (lead == 0xf0 && first < 0x90) || (lead == 0xf4 && first > 0x8f)) return false;
                for (const unsigned char c : bytes.substr(0, count))
                    if ((c & 0xc0) != 0x80) return false;
                bytes.remove_prefix(count);
            }
            return true;
        }

        [[noreturn]] void fail(std::string_view reason, std::string_view name = {}) {
            throw std::runtime_error("UI 包：" + std::string(reason) + (name.empty() ? "" : "：" + std::string(name)));
        }

        bool portableName(std::string_view name) {
            if (name.empty() || name.size() > 1024 || !validUtf8(name)) return false;
            for (const unsigned char c : name)
                if (c < 32 || c == 127 || std::string_view("\\:*?\"<>|").find(static_cast<char>(c)) != std::string_view::npos) return false;
            while (true) {
                const auto slash = name.find('/');
                const auto part = name.substr(0, slash);
                if (part.empty() || part.back() == '.' || part.back() == ' ') return false;
                std::string stem(part.substr(0, part.find('.')));
                for (auto& c : stem) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return false;
                if (stem.starts_with("com") || stem.starts_with("lpt")) {
                    const std::string_view suffix(stem.data() + 3, stem.size() - 3);
                    if ((suffix.size() == 1 && suffix.front() >= '1' && suffix.front() <= '9') || suffix == "¹" || suffix == "²" || suffix == "³") return false;
                }
                if (slash == std::string_view::npos) return true;
                name.remove_prefix(slash + 1);
            }
        }

    }

    std::string packageAssetName(std::string_view reference) {
        if (!reference.starts_with('/')) {
            if (!portableName(reference)) fail("资产逻辑名无效", reference);
            return std::string(reference);
        }
        auto encoded = reference.substr(1);
        encoded = encoded.substr(0, encoded.find_first_of("?#"));
        std::string name;
        for (std::size_t i = 0; i < encoded.size(); ++i) {
            if (encoded[i] != '%')
                name += encoded[i];
            else {
                unsigned int byte = 0;
                if (i + 2 >= encoded.size()) fail("资产 URL 编码无效", reference);
                const auto parsed = std::from_chars(encoded.data() + i + 1, encoded.data() + i + 3, byte, 16);
                if (parsed.ec != std::errc{} || parsed.ptr != encoded.data() + i + 3 || byte == '/') fail("资产 URL 编码无效", reference);
                name += static_cast<char>(byte);
                i += 2;
            }
        }
        if (!portableName(name)) fail("资产 URL 路径无效", reference);
        return name;
    }
}
