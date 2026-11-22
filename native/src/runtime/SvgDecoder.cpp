#include <runtime/PainterResources.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

#pragma warning(push, 0)
#include <lunasvg.h>
#pragma warning(pop)

namespace canton::runtime {
    namespace {
        bool containsIgnoringCase(std::string_view text, std::string_view needle) {
            return std::search(text.begin(), text.end(), needle.begin(), needle.end(), [](char a, char b) { return (a >= 'A' && a <= 'Z' ? a + 32 : a) == (b >= 'A' && b <= 'Z' ? b + 32 : b); }) != text.end();
        }

        bool isSpace(char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        }

        std::string_view trim(std::string_view text) {
            while (!text.empty() && isSpace(text.front())) text.remove_prefix(1);
            while (!text.empty() && isSpace(text.back())) text.remove_suffix(1);
            return text;
        }

        struct SvgRoot {
            std::string width, height, viewBox;
        };

        // 逐个标签扫属性：根必须是 svg，引用只能指向文档内部或内联数据
        SvgRoot inspect(std::string_view text, const std::string& source) {
            SvgRoot root;
            bool sawRoot = false;
            for (std::size_t at = 0; (at = text.find('<', at)) != std::string_view::npos;) {
                if (text.substr(at, 4) == "<!--") {
                    at = text.find("-->", at + 4);
                    if (at == std::string_view::npos) break;
                    continue;
                }
                if (text.substr(at, 2) == "<?" || text.substr(at, 2) == "<!" || text.substr(at, 2) == "</") {
                    at = text.find('>', at);
                    if (at == std::string_view::npos) break;
                    continue;
                }
                auto cursor = at + 1;
                const auto nameStart = cursor;
                while (cursor < text.size() && !isSpace(text[cursor]) && text[cursor] != '>' && text[cursor] != '/') ++cursor;
                const auto name = text.substr(nameStart, cursor - nameStart);
                if (!sawRoot) {
                    if (name != "svg") throw std::runtime_error("Painter SVG 内容无法解析：" + source);
                    sawRoot = true;
                }
                const auto isRoot = name == "svg" && root.width.empty() && root.height.empty() && root.viewBox.empty();
                while (cursor < text.size() && text[cursor] != '>') {
                    while (cursor < text.size() && (isSpace(text[cursor]) || text[cursor] == '/')) ++cursor;
                    const auto keyStart = cursor;
                    while (cursor < text.size() && text[cursor] != '=' && text[cursor] != '>' && !isSpace(text[cursor])) ++cursor;
                    const auto key = text.substr(keyStart, cursor - keyStart);
                    while (cursor < text.size() && isSpace(text[cursor])) ++cursor;
                    if (cursor >= text.size() || text[cursor] != '=') continue;
                    ++cursor;
                    while (cursor < text.size() && isSpace(text[cursor])) ++cursor;
                    if (cursor >= text.size() || (text[cursor] != '"' && text[cursor] != '\'')) throw std::runtime_error("Painter SVG 内容无法解析：" + source);
                    const auto quote = text[cursor++];
                    const auto valueEnd = text.find(quote, cursor);
                    if (valueEnd == std::string_view::npos) throw std::runtime_error("Painter SVG 内容无法解析：" + source);
                    const auto value = trim(text.substr(cursor, valueEnd - cursor));
                    cursor = valueEnd + 1;
                    if (key == "href" || key == "xlink:href") {
                        if (!value.empty() && !value.starts_with('#')) {
                            if (!value.starts_with("data:")) throw std::runtime_error("Painter SVG 禁止外部资源引用：" + source);
                            (void)dataUrlPayload(value, PainterResourceLimits{});
                        }
                    }
                    if (isRoot && name == "svg") {
                        if (key == "width") root.width = value;
                        if (key == "height") root.height = value;
                        if (key == "viewBox") root.viewBox = value;
                    }
                }
                at = cursor;
            }
            if (!sawRoot) throw std::runtime_error("Painter SVG 内容无法解析：" + source);
            // 样式里的 url(...) 只能指向文档内的片段，也不允许 @import
            if (containsIgnoringCase(text, "@import")) throw std::runtime_error("Painter SVG 禁止外部资源引用：" + source);
            for (std::size_t at = 0; (at = text.find("url(", at)) != std::string_view::npos; at += 4) {
                auto cursor = at + 4;
                while (cursor < text.size() && (isSpace(text[cursor]) || text[cursor] == '"' || text[cursor] == '\'')) ++cursor;
                if (cursor >= text.size() || text[cursor] != '#') throw std::runtime_error("Painter SVG 禁止外部资源引用：" + source);
            }
            return root;
        }

        float svgLength(std::string_view value, float fallback) {
            const auto text = asciiLower(trim(value));
            if (text.empty() || text.ends_with('%')) return fallback;
            auto result = std::strtof(text.c_str(), nullptr);
            if (text.ends_with("in"))
                result *= 96.0f;
            else if (text.ends_with("cm"))
                result *= 96.0f / 2.54f;
            else if (text.ends_with("mm"))
                result *= 96.0f / 25.4f;
            else if (text.ends_with("pt"))
                result *= 96.0f / 72.0f;
            else if (text.ends_with("pc"))
                result *= 16.0f;
            if (!std::isfinite(result) || result <= 0.0f) throw std::runtime_error("Painter SVG 视口尺寸必须是正有限数");
            return result;
        }
    }

    namespace {
        class SvgContent final : public core::VectorContent {
           public:
            SvgContent(std::unique_ptr<lunasvg::Document> document, core::Size size) : document_(std::move(document)), size_(size) {
                intrinsicSize = size;
            }

            std::vector<std::uint32_t> rasterize(core::Rect source, int width, int height) const override {
                std::vector<std::uint32_t> result(static_cast<std::size_t>(std::max(width, 0)) * static_cast<std::size_t>(std::max(height, 0)));
                if (result.empty() || !(source.width > 0) || !(source.height > 0)) return result;
                // plutovg 的位图就是预乘的 0xAARRGGBB，直接画进结果缓冲；源区域按两个方向各自缩放
                lunasvg::Bitmap bitmap(reinterpret_cast<std::uint8_t*>(result.data()), width, height, width * 4);
                const auto scaleX = static_cast<float>(width) / source.width, scaleY = static_cast<float>(height) / source.height;
                const auto unitX = document_->width() / size_.width, unitY = document_->height() / size_.height;
                document_->render(bitmap, lunasvg::Matrix(scaleX / unitX, 0, 0, scaleY / unitY, -source.x * scaleX, -source.y * scaleY));
                return result;
            }

           private:
            std::unique_ptr<lunasvg::Document> document_;
            core::Size size_;
        };
    }

    std::shared_ptr<const core::VectorContent> decodeSvgResource(std::string_view text, std::string_view sourceName) {
        const std::string source(sourceName);
        if (containsIgnoringCase(text, "<!DOCTYPE") || containsIgnoringCase(text, "<!ENTITY")) throw std::runtime_error("Painter SVG 不支持 DTD 或外部实体：" + source);
        const auto root = inspect(text, source);
        // viewBox 的四个数用空白或逗号分隔
        std::vector<float> viewBox;
        for (std::size_t at = 0; at < root.viewBox.size();) {
            while (at < root.viewBox.size() && (isSpace(root.viewBox[at]) || root.viewBox[at] == ',')) ++at;
            if (at >= root.viewBox.size()) break;
            char* end = nullptr;
            const auto value = std::strtof(root.viewBox.c_str() + at, &end);
            if (end == root.viewBox.c_str() + at) break;
            viewBox.push_back(value);
            at = static_cast<std::size_t>(end - root.viewBox.c_str());
        }
        const auto width = svgLength(root.width, viewBox.size() == 4 ? viewBox[2] : 300.0f);
        const auto height = svgLength(root.height, viewBox.size() == 4 ? viewBox[3] : 150.0f);
        if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0f || height <= 0.0f) throw std::runtime_error("Painter SVG 视口无效：" + source);
        auto document = lunasvg::Document::loadFromData(text.data(), text.size());
        if (!document) throw std::runtime_error("Painter SVG 内容无法解析：" + source);
        if (!(document->width() > 0) || !(document->height() > 0) || !std::isfinite(document->width()) || !std::isfinite(document->height())) throw std::runtime_error("Painter SVG 视口无效：" + source);
        if (const auto box = document->boundingBox(); !(box.w > 0) && !(box.h > 0)) throw std::runtime_error("Painter SVG 未产生可绘制内容：" + source);
        return std::make_shared<SvgContent>(std::move(document), core::Size{width, height});
    }
}
