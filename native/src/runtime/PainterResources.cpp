#include <runtime/PainterResources.h>
#include <stdexcept>

namespace canton::runtime {
    std::shared_ptr<const core::FontContent> decodeFontResource(std::span<const std::uint8_t> payload, std::string_view source) {
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(payload.begin(), payload.end());
        const auto info = canton::core::readFontFaceInfo({bytes->data(), bytes->size()});
        if (!info) throw std::runtime_error("字体资源不是 TrueType 或 OpenType（不支持 WOFF）：" + std::string(source));
        auto content = std::make_shared<canton::core::FontContent>();
        content->bytes = std::move(bytes);
        content->info = *info;
        return content;
    }
}
