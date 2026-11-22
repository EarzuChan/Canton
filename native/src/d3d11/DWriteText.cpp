#include <d3d11/DWriteFontResolver.h>
#include <d3d11/DWriteText.h>

#include <core/TextMetrics.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace canton::d3d11 {
    namespace {
        // 不换行时给排版的宽高上限
        constexpr float unbounded = 1.0e6f;

        struct Utf16Text {
            std::wstring text;
            // 每个 UTF-16 下标对应的 UTF-8 字节偏移，末尾多一项指向文本结尾
            std::vector<std::size_t> bytes;

            [[nodiscard]] std::size_t byteAt(std::size_t index) const {
                return bytes[std::min(index, bytes.size() - 1)];
            }
        };

        Utf16Text toUtf16(std::string_view source) {
            Utf16Text result;
            result.text.reserve(source.size());
            result.bytes.reserve(source.size() + 1);
            for (std::size_t index = 0; index < source.size();) {
                const auto start = index;
                const auto codepoint = core::decodeUtf8Codepoint(source, index);
                if (codepoint >= 0x10000) {
                    const auto value = static_cast<std::uint32_t>(codepoint) - 0x10000;
                    result.text.push_back(static_cast<wchar_t>(0xd800 + (value >> 10)));
                    result.text.push_back(static_cast<wchar_t>(0xdc00 + (value & 0x3ff)));
                    result.bytes.push_back(start);
                    result.bytes.push_back(start);
                } else {
                    // 孤立代理项不是合法文本，按替换字符处理
                    result.text.push_back(codepoint >= 0xd800 && codepoint <= 0xdfff ? static_cast<wchar_t>(0xfffd) : static_cast<wchar_t>(codepoint));
                    result.bytes.push_back(start);
                }
            }
            result.bytes.push_back(source.size());
            return result;
        }

        struct Cluster {
            std::size_t start = 0;
            std::size_t end = 0;
            float x = 0;
            float width = 0;
            float baseline = 0;
        };

        // 收集排版产出的字形与字符簇几何，不做任何绘制
        class GlyphCollector final : public IDWriteTextRenderer {
           public:
            std::vector<GlyphRun> runs;
            std::vector<Cluster> clusters;
            // 省略号等行内对象的字形只参与绘制与行宽，不对应源文本
            std::vector<Cluster> decorations;

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
                if (id == __uuidof(IUnknown) || id == __uuidof(IDWritePixelSnapping) || id == __uuidof(IDWriteTextRenderer)) {
                    *object = static_cast<IDWriteTextRenderer*>(this);
                    return S_OK;
                }
                *object = nullptr;
                return E_NOINTERFACE;
            }

            // 只在栈上使用，不参与引用计数
            ULONG STDMETHODCALLTYPE AddRef() override {
                return 1;
            }

            ULONG STDMETHODCALLTYPE Release() override {
                return 1;
            }

            HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override {
                *disabled = TRUE;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* transform) override {
                *transform = {1, 0, 0, 1, 0, 0};
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels) override {
                *pixels = 1;
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override {
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override {
                return S_OK;
            }

            HRESULT STDMETHODCALLTYPE DrawInlineObject(void* context, FLOAT x, FLOAT y, IDWriteInlineObject* object, BOOL sideways, BOOL rightToLeft, IUnknown* effect) override {
                if (!object) return S_OK;
                ++inlineDepth_;
                const auto result = object->Draw(context, this, x, y, sideways, rightToLeft, effect);
                --inlineDepth_;
                return result;
            }

            HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT originX, FLOAT originY, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN* run, const DWRITE_GLYPH_RUN_DESCRIPTION* description, IUnknown*) override {
                if (!run || run->glyphCount == 0 || !run->fontFace) return S_OK;
                const auto rightToLeft = (run->bidiLevel & 1) != 0;
                GlyphRun output;
                output.face = run->fontFace;
                output.emSize = run->fontEmSize;
                output.baseline = originY;
                // 每个字形左缘的笔位置；从右到左的字形从原点向左排
                std::vector<float> left(run->glyphCount), advance(run->glyphCount);
                auto pen = originX;
                for (UINT32 i = 0; i < run->glyphCount; ++i) {
                    advance[i] = run->glyphAdvances ? run->glyphAdvances[i] : 0.0f;
                    if (rightToLeft) pen -= advance[i];
                    left[i] = pen;
                    if (!rightToLeft) pen += advance[i];
                    const auto offset = run->glyphOffsets ? run->glyphOffsets[i] : DWRITE_GLYPH_OFFSET{};
                    output.glyphs.push_back(run->glyphIndices[i]);
                    output.positions.push_back({left[i] + (rightToLeft ? -offset.advanceOffset : offset.advanceOffset), originY - offset.ascenderOffset});
                }
                runs.push_back(std::move(output));
                const auto extent = [&](UINT32 first, UINT32 last) {
                    Cluster cluster;
                    cluster.baseline = originY;
                    auto minimum = left[first], maximum = left[first] + advance[first];
                    for (auto i = first; i < last; ++i) {
                        minimum = std::min(minimum, left[i]);
                        maximum = std::max(maximum, left[i] + advance[i]);
                    }
                    cluster.x = minimum;
                    cluster.width = maximum - minimum;
                    return cluster;
                };
                if (inlineDepth_ > 0 || !description || !description->clusterMap || description->stringLength == 0) {
                    decorations.push_back(extent(0, run->glyphCount));
                    return S_OK;
                }
                // 字符簇：clusterMap 给出每个字符所属簇的首字形
                for (UINT32 at = 0; at < description->stringLength;) {
                    auto next = at + 1;
                    while (next < description->stringLength && description->clusterMap[next] == description->clusterMap[at]) ++next;
                    const UINT32 firstGlyph = description->clusterMap[at];
                    const UINT32 lastGlyph = next < description->stringLength ? description->clusterMap[next] : run->glyphCount;
                    if (firstGlyph < lastGlyph && lastGlyph <= run->glyphCount) {
                        auto cluster = extent(firstGlyph, lastGlyph);
                        cluster.start = description->textPosition + at;
                        cluster.end = description->textPosition + next;
                        clusters.push_back(cluster);
                    }
                    at = next;
                }
                return S_OK;
            }

           private:
            int inlineDepth_ = 0;
        };
    }

    std::size_t DWriteTextResource::estimatedBytes() const noexcept {
        auto bytes = sizeof(DWriteTextResource) + runs.capacity() * sizeof(GlyphRun);
        for (const auto& run : runs) bytes += run.glyphs.capacity() * sizeof(std::uint16_t) + run.positions.capacity() * sizeof(core::Point);
        return bytes;
    }

    struct DWriteTextMeasurer::Impl {
        explicit Impl(const core::FontRegistry* fonts) : resolver(fonts) {}

        std::mutex mutex;
        FontResolver resolver;
    };

    DWriteTextMeasurer::DWriteTextMeasurer(const core::FontRegistry* fonts) : impl_(std::make_unique<Impl>(fonts)) {}

    DWriteTextMeasurer::~DWriteTextMeasurer() = default;

    DWriteTextMeasurer::ResolvedFace DWriteTextMeasurer::resolve(const core::TextStyle& style) const {
        const std::scoped_lock lock(impl_->mutex);
        const auto font = impl_->resolver.resolve(style);
        return {narrow(font.family), font.info.weight, font.info.italic, font.bundled, font.syntheticBold, font.syntheticItalic};
    }

    core::TextLayout DWriteTextMeasurer::createLayout(std::string_view source, core::TextStyle style, core::TextLayoutOptions options) const {
        core::TextLayout result;
        const auto fontSize = style.fontSize > 0.0f ? style.fontSize : core::defaultFontSize;
        const auto lineHeight = style.lineHeight > 0.0f ? style.lineHeight : fontSize * core::defaultLineHeightRatio;
        result.lineHeight = lineHeight;
        auto resource = std::make_shared<DWriteTextResource>();
        result.resource = resource;

        ResolvedFont font;
        {
            const std::scoped_lock lock(impl_->mutex);
            font = impl_->resolver.resolve(style);
        }
        // 行盒由 Canton 定：每行高 lineHeight，基线取主字体的半行距（可为负）加上行；回落字体不撑高行盒
        const auto unit = fontSize / static_cast<float>(std::max<UINT16>(font.metrics.designUnitsPerEm, 1));
        const auto ascent = static_cast<float>(font.metrics.ascent) * unit, descent = static_cast<float>(font.metrics.descent) * unit;
        const auto lineBaseline = (lineHeight - (ascent + descent)) * 0.5f + ascent;
        result.baseline = result.lastBaseline = lineBaseline;

        auto visible = source;
        if (options.singleLine) visible = visible.substr(0, visible.find('\n'));
        const auto text = toUtf16(visible);

        auto* factory = dwriteFactory();
        ComPtr<IDWriteTextFormat> format;
        check(factory->CreateTextFormat(font.family.c_str(), font.collection.Get(), font.weight, font.style, font.stretch, fontSize, L"", format.GetAddressOf()), "无法创建文字格式");
        // 限宽只在多行或需要省略时生效；单行不省略时按内容自然宽度排
        const auto constrained = options.maxWidth > 0 && (!options.singleLine || options.ellipsis);
        const auto wraps = constrained && !options.singleLine;
        check(format->SetWordWrapping(wraps ? DWRITE_WORD_WRAPPING_EMERGENCY_BREAK : DWRITE_WORD_WRAPPING_NO_WRAP), "无法设置换行方式");
        const auto limited = options.maxLines > 0 && options.ellipsis;
        ComPtr<IDWriteTextLayout> layout;
        check(factory->CreateTextLayout(text.text.c_str(), static_cast<UINT32>(text.text.size()), format.Get(), constrained ? options.maxWidth : unbounded, limited ? static_cast<float>(options.maxLines) * lineHeight : unbounded, layout.GetAddressOf()), "无法创建文字排版");
        check(layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lineHeight, lineBaseline), "无法设置行高");
        if (options.ellipsis) {
            ComPtr<IDWriteInlineObject> sign;
            check(factory->CreateEllipsisTrimmingSign(format.Get(), sign.GetAddressOf()), "无法创建省略号");
            const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            check(layout->SetTrimming(&trimming, sign.Get()), "无法设置省略");
        }

        UINT32 lineCount = 0;
        layout->GetLineMetrics(nullptr, 0, &lineCount);
        std::vector<DWRITE_LINE_METRICS> metrics(std::max<UINT32>(lineCount, 1));
        check(layout->GetLineMetrics(metrics.data(), static_cast<UINT32>(metrics.size()), &lineCount), "无法读取行信息");
        metrics.resize(lineCount);
        GlyphCollector collector;
        check(layout->Draw(nullptr, &collector, 0, 0), "无法读取字形");

        // 行距统一，行号由基线反推
        const auto lineOf = [&](float baseline) {
            return static_cast<std::size_t>(std::max(0.0f, std::round((baseline - lineBaseline) / lineHeight)));
        };
        auto kept = std::max<std::size_t>(metrics.size(), 1);
        if (options.maxLines > 0) kept = std::min(kept, static_cast<std::size_t>(options.maxLines));
        result.lines.resize(kept);
        std::vector<std::size_t> lineStart(metrics.size() + 1, 0);
        for (std::size_t i = 0; i < metrics.size(); ++i) lineStart[i + 1] = lineStart[i] + metrics[i].length;
        for (std::size_t i = 0; i < kept; ++i) {
            auto& line = result.lines[i];
            line.y = static_cast<float>(i) * lineHeight;
            line.height = lineHeight;
            line.baseline = line.y + lineBaseline;
            if (i >= metrics.size()) continue;
            line.start = text.byteAt(lineStart[i]);
            line.end = text.byteAt(lineStart[i + 1] - metrics[i].newlineLength);
            // 被省略的行只算到最后一个还显示的字符
            if (metrics[i].isTrimmed) line.end = line.start;
        }
        for (const auto& cluster : collector.clusters) {
            const auto index = lineOf(cluster.baseline);
            if (index >= kept || index >= metrics.size()) continue;
            auto& line = result.lines[index];
            const auto& metric = metrics[index];
            // 换行符不占位，也不进输入几何
            if (cluster.start >= lineStart[index + 1] - metric.newlineLength) continue;
            const auto start = text.byteAt(cluster.start), end = text.byteAt(cluster.end);
            line.runs.push_back({start, end, cluster.x, cluster.width});
            if (metric.isTrimmed) line.end = std::max(line.end, end);
            // 自动折行处的行尾空白悬在行外，不计入行宽
            const auto softWrapped = metric.newlineLength == 0 && index + 1 < metrics.size();
            if (softWrapped && cluster.start >= lineStart[index + 1] - metric.trailingWhitespaceLength) continue;
            line.width = std::max(line.width, cluster.x + cluster.width);
        }
        for (const auto& decoration : collector.decorations) {
            const auto index = lineOf(decoration.baseline);
            if (index < kept) result.lines[index].width = std::max(result.lines[index].width, decoration.x + decoration.width);
        }
        for (auto& line : result.lines) {
            // 一个字符区间可能对应多段字形，合并其前进范围后再建立视觉顺序
            std::stable_sort(line.runs.begin(), line.runs.end(), [](const auto& left, const auto& right) { return left.start != right.start ? left.start < right.start : left.end < right.end; });
            std::size_t count = 0;
            for (const auto current : line.runs) {
                if (count > 0 && line.runs[count - 1].start == current.start && line.runs[count - 1].end == current.end) {
                    auto& previous = line.runs[count - 1];
                    const auto right = std::max(previous.x + previous.width, current.x + current.width);
                    previous.x = std::min(previous.x, current.x);
                    previous.width = right - previous.x;
                } else
                    line.runs[count++] = current;
            }
            line.runs.resize(count);
            std::stable_sort(line.runs.begin(), line.runs.end(), [](const auto& left, const auto& right) { return left.x < right.x; });
            result.width = std::max(result.width, line.width);
        }
        result.height = std::max(lineHeight, static_cast<float>(kept) * lineHeight);
        result.baseline = result.lines.front().baseline;
        result.lastBaseline = result.lines.back().baseline;
        result.truncated = result.lines.back().end < source.size();

        float inkLeft = 0, inkTop = 0, inkRight = 0, inkBottom = 0;
        bool hasInk = false;
        std::vector<DWRITE_GLYPH_METRICS> glyphMetrics;
        for (auto& run : collector.runs) {
            run.line = lineOf(run.baseline);
            if (run.line >= kept) continue;
            // 伪斜体对所有字形生效；伪粗体只作用于主字体，回落字体已按请求的字重匹配
            run.syntheticItalic = font.syntheticItalic;
            run.syntheticBold = font.syntheticBold && sameFontFace(run.face.Get(), font.face.Get());
            DWRITE_FONT_METRICS faceMetrics{};
            run.face->GetMetrics(&faceMetrics);
            const auto scale = run.emSize / static_cast<float>(std::max<UINT16>(faceMetrics.designUnitsPerEm, 1));
            glyphMetrics.resize(run.glyphs.size());
            if (SUCCEEDED(run.face->GetDesignGlyphMetrics(run.glyphs.data(), static_cast<UINT32>(run.glyphs.size()), glyphMetrics.data(), FALSE))) {
                for (std::size_t i = 0; i < run.glyphs.size(); ++i) {
                    const auto& glyph = glyphMetrics[i];
                    const auto left = run.positions[i].x + static_cast<float>(glyph.leftSideBearing) * scale;
                    const auto right = run.positions[i].x + static_cast<float>(static_cast<INT32>(glyph.advanceWidth) - glyph.rightSideBearing) * scale;
                    const auto top = run.positions[i].y - static_cast<float>(glyph.verticalOriginY - glyph.topSideBearing) * scale;
                    const auto bottom = run.positions[i].y - static_cast<float>(glyph.verticalOriginY - static_cast<INT32>(glyph.advanceHeight) + glyph.bottomSideBearing) * scale;
                    if (right <= left || bottom <= top) continue;
                    inkLeft = hasInk ? std::min(inkLeft, left) : left;
                    inkTop = hasInk ? std::min(inkTop, top) : top;
                    inkRight = hasInk ? std::max(inkRight, right) : right;
                    inkBottom = hasInk ? std::max(inkBottom, bottom) : bottom;
                    hasInk = true;
                }
            } else
                result.boundsKnown = false;
            resource->runs.push_back(std::move(run));
        }
        if (hasInk) {
            // 伪造会超出字形本身：描边向外半个笔宽，错切向右推移
            if (font.syntheticBold) {
                const auto grow = core::syntheticBoldStroke * fontSize * 0.5f;
                inkLeft -= grow, inkTop -= grow, inkRight += grow, inkBottom += grow;
            }
            if (font.syntheticItalic) {
                inkRight += core::syntheticItalicSkew * (inkBottom - inkTop);
                inkLeft -= core::syntheticItalicSkew * fontSize * 0.25f;
            }
            result.inkBounds = {inkLeft, inkTop, inkRight - inkLeft, inkBottom - inkTop};
        }
        return result;
    }
}
