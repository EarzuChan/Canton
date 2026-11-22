// 字体：CSS 字面选择、OS/2 读数、自带字体从内存加载、族表依次回落到系统字体、伪粗体与伪斜体（及关闭开关）、
// 字体环境代际推进后的重排与重建；伪造与斜体的伸出部分经 D3D11 渲染器真的画出来。
#include "RenderSupport.h"
#include "TextFixtures.h"

#include <core/Font.h>
#include <core/Paint.h>
#include <core/SceneFramePipeline.h>
#include <d3d11/DWriteFontResolver.h>
#include <d3d11/DWriteText.h>
#include <d3d11/Renderer.h>

#include <cmath>
#include <filesystem>
#include <iostream>
#include <stdexcept>

using namespace canton::core;
using canton::d3d11::DWriteTextMeasurer;
using canton::d3d11::DWriteTextResource;

namespace {
    void check(bool condition, const std::string& message) {
        if (!condition) throw std::runtime_error(message);
    }

    bool approx(float left, float right, float epsilon = 0.6f) {
        return std::abs(left - right) <= epsilon;
    }

    const std::filesystem::path fontDirectory = std::filesystem::path(CANTON_SOURCE_DIR) / "tests/fixtures/fonts";
    constexpr const char* family = test_support::testFontFamily;
    constexpr const char* uprightFamily = "Canton Test Upright";

    FontBytes face(const char* style) {
        return std::make_shared<const std::vector<std::uint8_t>>(test_support::readFile(fontDirectory / (std::string("CantonTestSans-") + style + ".ttf")));
    }

    // 只有常规体的族，用来验伪造
    FontFamilyDefinition uprightOnly() {
        FontFamilyDefinition result;
        result.name = uprightFamily;
        result.faces.push_back({face("Regular"), std::nullopt, std::nullopt});
        return result;
    }

    TextStyle style(int weight, bool italic, bool synthesis = true, std::vector<std::string> families = {family}) {
        TextStyle result;
        result.fontSize = 100;
        result.fontFamily = std::move(families);
        result.fontWeight = weight;
        result.italic = italic;
        result.fontSynthesis = synthesis;
        return result;
    }

    // 注册表、DirectWrite 测量器与排版服务；测试字体登记在 owner 名下
    struct Fonts {
        FontRegistry registry;
        DWriteTextMeasurer measurer{&registry};
        TextLayoutService service{measurer, 256, 8 * 1024 * 1024, &registry};

        void defineTestFamilies() {
            test_support::defineTestFonts(registry, fontDirectory, this);
            registry.define(this, uprightOnly());
        }

        float width(std::string_view text, const TextStyle& textStyle) const {
            return service.layout(text, textStyle)->width;
        }

        std::shared_ptr<const DWriteTextResource> resource(std::string_view text, const TextStyle& textStyle) const {
            return std::dynamic_pointer_cast<const DWriteTextResource>(service.layout(text, textStyle)->resource);
        }
    };

    void selection() {
        const FontFaceInfo faces[]{{300, false}, {400, false}, {700, false}, {400, true}, {900, false}};
        check(selectFontFace(faces, 400, false, true).index == 1 && selectFontFace(faces, 700, false, true).index == 2, "精确字重没有命中");
        auto choice = selectFontFace(faces, 500, false, true);
        check(choice.found && choice.index == 1 && !choice.syntheticBold, "500 应落到 400");
        check(selectFontFace(faces, 200, false, true).index == 0, "200 应取最近的 300");
        choice = selectFontFace(faces, 800, false, true);
        check(choice.index == 4 && !choice.syntheticBold, "800 应先往上取 900");
        check(selectFontFace(faces, 600, false, true).index == 2, "600 应先往上取 700");
        choice = selectFontFace(faces, 700, true, true);
        check(choice.index == 3 && choice.syntheticBold && !choice.syntheticItalic, "斜体 700 应取斜体 400 并伪造粗体");
        check(!selectFontFace(faces, 700, true, false).syntheticBold, "关闭伪造后不应伪造粗体");
        const FontFaceInfo upright[]{{400, false}, {700, false}};
        choice = selectFontFace(upright, 400, true, true);
        check(choice.index == 0 && choice.syntheticItalic && !choice.syntheticBold, "缺少斜体面应伪造斜体");
        check(!selectFontFace(upright, 400, true, false).syntheticItalic, "关闭伪造后不应伪造斜体");
        const FontFaceInfo single[]{{400, false}};
        check(selectFontFace(single, 600, false, true).syntheticBold && !selectFontFace(single, 590, false, true).syntheticBold, "伪造粗体的阈值是 600");
        check(!selectFontFace({}, 400, false, true).found, "空字面表不应选中");
        check(splitFontFamilies(joinFontFamilies({"A", "B C"})) == std::vector<std::string>{"A", "B C"}, "字体族表编码往返不一致");
    }

    void fontTables() {
        const auto regular = readFontFaceInfo(*face("Regular"));
        const auto bold = readFontFaceInfo(*face("Bold"));
        const auto italic = readFontFaceInfo(*face("Italic"));
        check(regular && regular->weight == 400 && !regular->italic, "Regular 的 OS/2 读数错误");
        check(bold && bold->weight == 700 && !bold->italic, "Bold 的 OS/2 读数错误");
        check(italic && italic->weight == 400 && italic->italic, "Italic 的 OS/2 读数错误");
        const std::uint8_t garbage[]{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
        check(!readFontFaceInfo(garbage), "非字体字节不应被接受");
    }

    void bundledFamily() {
        Fonts fonts;
        const auto before = fonts.service.environment();
        fonts.defineTestFamilies();
        check(fonts.service.environment() != before, "登记字体族没有推进字体环境代际");
        const auto resolved = [&](TextStyle textStyle) {
            return fonts.measurer.resolve(textStyle);
        };
        const auto regular = fonts.width("AAAA", style(400, false)), bold = fonts.width("AAAA", style(700, false)), italic = fonts.width("AAAA", style(400, true));
        check(!approx(regular, bold) && !approx(regular, italic), "三个字面的前进量应各不相同");
        check(resolved(style(400, false)).bundled && resolved(style(400, false)).weight == 400, "常规体应取自带的 Regular 面");
        check(resolved(style(700, false)).weight == 700 && !resolved(style(700, false)).syntheticBold, "700 应取 Bold 面");
        check(approx(fonts.width("AAAA", style(500, false)), regular), "500 应按 CSS 落到 Regular 而不是静默变粗");
        check(approx(fonts.width("AAAA", style(600, false)), bold), "600 应先往上取 Bold");
        check(resolved(style(400, true)).italic && !resolved(style(400, true)).syntheticItalic, "斜体应取 Italic 面");
        // 斜体加粗：取 Italic 面并伪造粗体，前进量不变
        const auto italicBold = fonts.resource("AAAA", style(700, true));
        check(italicBold && !italicBold->runs.empty() && italicBold->runs[0].syntheticBold && !italicBold->runs[0].syntheticItalic, "斜体 700 应伪造粗体而不错切");
        check(approx(fonts.width("AAAA", style(700, true)), italic), "斜体 700 的前进量应取自 Italic 面，伪造不改字距");
        check(!fonts.resource("AAAA", style(700, true, false))->runs[0].syntheticBold, "关闭伪造后不应有伪粗体");
        const auto plain = fonts.resource("AAAA", style(400, false));
        check(!plain->runs[0].syntheticBold && !plain->runs[0].syntheticItalic, "真字面不应伪造");
        const auto upright = [&](int weight, bool slanted, bool synthesis = true) {
            return style(weight, slanted, synthesis, {uprightFamily});
        };
        check(fonts.resource("AAAA", upright(400, true))->runs[0].syntheticItalic, "缺少斜体面应伪造错切");
        check(!fonts.resource("AAAA", upright(400, true, false))->runs[0].syntheticItalic, "关闭伪造后不应错切");
        check(fonts.resource("AAAA", upright(800, false))->runs[0].syntheticBold, "只有 Regular 的族请求 800 应伪造粗体");
        check(fonts.service.layout("AAAA", upright(800, false))->inkBounds.width > fonts.service.layout("AAAA", upright(800, false, false))->inkBounds.width, "伪粗体的墨迹边界应外扩");
        // 族名不分大小写；列表按序取首个可用族
        check(approx(fonts.width("AAAA", style(700, false, true, {"No Such Family Anywhere", "canton test sans"})), bold), "族名列表应跳过不存在的族并忽略大小写");
        // 重新登记同名族：旧结果失效
        const auto old = fonts.service.layout("AAAA", style(400, false));
        test_support::defineTestFonts(fonts.registry, fontDirectory, &fonts);
        check(fonts.service.layout("AAAA", style(400, false), {}, old) != old, "字体环境代际变化后旧排版不应复用");
        // 撤销登记后回落到系统字体
        fonts.registry.removeOwned(&fonts);
        check(!resolved(style(400, false)).bundled && !approx(fonts.width("AAAA", style(400, false)), regular), "撤销登记后应回落到系统字体");
    }

    void systemFallback() {
        Fonts fonts;
        fonts.defineTestFamilies();
        // 选中的自带字面里没有的字符由 DirectWrite 的系统回落补上；字面换成系统字体
        const auto mixed = fonts.resource("AB 你好", style(400, false));
        check(mixed && mixed->runs.size() >= 2, "缺字没有回落到系统字体");
        const auto latin = fonts.resource("AB", style(400, false));
        check(!canton::d3d11::sameFontFace(mixed->runs.back().face.Get(), latin->runs.front().face.Get()), "回落的字形仍取自自带字面");
        // 族表逐个查：自带字体优先，其后是系统字体
        check(fonts.measurer.resolve(style(400, false, true, {"No Such Family Anywhere", "Segoe UI"})).family == "Segoe UI", "族表应依次回落到系统字体");
        TextStyle plain;
        plain.fontSize = 100;
        TextStyle missing = plain;
        missing.fontFamily = {"No Such Family Anywhere"};
        check(approx(fonts.width("Hamburgefons", plain), fonts.width("Hamburgefons", missing)), "不存在的族名应回落到默认字体");
        TextStyle bold = plain;
        bold.fontWeight = 700;
        check(fonts.width("Hamburgefons", bold) > fonts.width("Hamburgefons", plain) * 1.01f, "默认字体的粗体应更宽或伪造");
        // 行高是给定值，中英文与表情混排时也不变
        TextStyle sized = plain;
        sized.fontSize = 20;
        sized.lineHeight = 30;
        check(approx(fonts.service.layout("Hello", sized)->height, 30) && approx(fonts.service.layout("Hello 你好 🙂", sized)->height, 30), "行高应恒为 lineHeight");
    }

    void systemFaces() {
        Fonts fonts;
        const auto segoe = [&](int weight, bool italic) {
            return fonts.measurer.resolve(style(weight, italic, true, {"Segoe UI"}));
        };
        check(segoe(600, false).weight == 600 && !segoe(600, false).syntheticBold && !segoe(600, false).bundled, "Segoe UI 的 600 应取真 Semibold");
        check(segoe(400, true).italic && !segoe(400, true).syntheticItalic, "Segoe UI 有斜体字面，不应伪造");
        const auto yahei = fonts.measurer.resolve(style(400, true, true, {"Microsoft YaHei UI"}));
        check(yahei.family == "Microsoft YaHei UI" && yahei.syntheticItalic, "Microsoft YaHei UI 的斜体应由 Canton 伪造");
        check(!fonts.measurer.resolve(style(400, true, false, {"Microsoft YaHei UI"})).syntheticItalic, "关闭伪造后系统字体也不应伪造");
        check(fonts.measurer.resolve(style(700, false, true, {"Microsoft YaHei UI"})).weight == 700, "Microsoft YaHei UI 有真粗体");
    }

    // DirectWrite 排版的几何：换行、最大行数与省略、单行、行与分段、首末行基线、墨迹，
    // 以及经 TextLayoutService 算出的光标位置、点击落点与选区矩形
    void layoutGeometry() {
        Fonts fonts;
        fonts.defineTestFamilies();
        auto textStyle = style(400, false);
        textStyle.fontSize = 20;
        textStyle.lineHeight = 30;
        const std::string paragraph = "alpha beta gamma delta epsilon zeta";
        const auto one = fonts.service.layout(paragraph, textStyle);
        check(one->lines.size() == 1 && !one->truncated && approx(one->height, 30), "不限宽时应只有一行");
        const auto wordWidth = fonts.width("alpha beta", textStyle);
        TextLayoutOptions wrap;
        wrap.maxWidth = wordWidth + 2;
        const auto wrapped = fonts.service.layout(paragraph, textStyle, wrap);
        check(wrapped->lines.size() >= 3 && approx(wrapped->height, 30.0f * static_cast<float>(wrapped->lines.size())), "按宽度换行的行数或行高错误");
        check(wrapped->lines.front().start == 0 && wrapped->lines.back().end == paragraph.size(), "各行没有覆盖整段文字");
        for (std::size_t index = 0; index < wrapped->lines.size(); ++index) {
            const auto& line = wrapped->lines[index];
            check(line.width <= wrap.maxWidth + 0.5f && approx(line.y, 30.0f * static_cast<float>(index)) && !line.runs.empty(), "第 " + std::to_string(index) + " 行的几何错误");
            if (index > 0) check(line.start == wrapped->lines[index - 1].end, "行与行之间有缺口");
        }
        check(paragraph.substr(wrapped->lines[0].start, wrapped->lines[1].start - wrapped->lines[0].start).starts_with("alpha beta"), "换行没有落在词边界");
        check(wrapped->baseline > 0 && wrapped->baseline < 30 && approx(wrapped->lastBaseline - wrapped->baseline, 30.0f * static_cast<float>(wrapped->lines.size() - 1)), "首末行基线错误");
        check(wrapped->inkBounds.width > 0 && wrapped->inkBounds.height > 0 && wrapped->inkBounds.y >= -1 && wrapped->inkBounds.y + wrapped->inkBounds.height <= wrapped->height + 8, "墨迹范围不合理");
        auto limited = wrap;
        limited.maxLines = 2;
        limited.ellipsis = true;
        const auto clipped = fonts.service.layout(paragraph, textStyle, limited);
        check(clipped->lines.size() == 2 && clipped->truncated && approx(clipped->height, 60), "最大行数加省略错误");
        TextLayoutOptions single;
        single.singleLine = true;
        single.maxWidth = wordWidth;
        single.ellipsis = true;
        const auto ellipsized = fonts.service.layout(paragraph, textStyle, single);
        check(ellipsized->lines.size() == 1 && ellipsized->truncated && ellipsized->width <= wordWidth + 0.5f, "单行省略错误");
        // 光标、点击与选区
        const auto& layout = *wrapped;
        const auto second = layout.lines[1].start;
        const auto caret = fonts.service.caretRect(layout, second, {5, 7});
        check(approx(caret.y, 37) && approx(caret.height, 30) && approx(caret.x, 5 + fonts.service.xForByteIndex(layout, second)), "第二行行首的光标位置错误");
        check(approx(fonts.service.xForByteIndex(layout, 5), fonts.width("alpha", textStyle)), "字节下标的横坐标与前缀宽度不一致");
        check(fonts.service.byteIndexAtPoint(layout, {fonts.width("alp", textStyle) + 0.5f, 10}) == 3, "点击落点应在 alp 之后");
        check(fonts.service.byteIndexAtPoint(layout, {1, 45}) == second, "点击第二行行首应落到该行起点");
        check(fonts.service.byteIndexAtPoint(layout, {1, 1000}) >= layout.lines.back().start, "点到末行之下应落到末行");
        const auto selection = fonts.service.boundsForRange(layout, 0, layout.lines[1].start + 2);
        check(selection.size() == 2 && approx(selection[0].y, 0) && approx(selection[1].y, 30) && selection[1].width > 0, "跨行选区应给出每行一个矩形");
    }

    // 在透明底上画一行黑字，返回每个像素的 alpha
    struct Raster {
        int width = 0, height = 0;
        std::vector<std::uint32_t> pixels;

        [[nodiscard]] int alpha(int x, int y) const {
            return static_cast<int>(pixels[static_cast<std::size_t>(y * width + x)] >> 24);
        }
    };

    Raster renderText(canton::d3d11::Renderer& renderer, const TextLayoutService& service, const TextStyle& textStyle, std::string text, Rect box, TextOverflow overflow = TextOverflow::Clip, DrawOp* prepared = nullptr) {
        DrawOp op;
        op.type = DrawOpType::DrawText;
        op.rect = box;
        op.text = std::move(text);
        op.textStyle = textStyle;
        op.maxLines = 1;
        op.overflow = overflow;
        op.color = 0xff000000u;
        DrawOpsBuilder::prepareText(op, service);
        if (op.rect.width <= 0) {
            op.rect.width = op.textLayout->width;
            op.textLayout.reset();
            DrawOpsBuilder::prepareText(op, service);
        }
        if (prepared) *prepared = op;
        PublishedFrameContent content;
        content.scenePaint = test_support::fragmentOf({op});
        content.viewport = {440, 160};
        auto target = renderer.createOffscreenTarget(440, 160);
        renderer.render(*target, {&content, {}, 0});
        return {440, 160, renderer.readPixels(*target)};
    }

    struct Ink {
        double coverage = 0, topCentroid = 0, bottomCentroid = 0;
        // 上下两半墨迹重心的竖直距离，伪斜体的水平偏移应是它乘以错切系数
        double rise = 0;
    };

    Ink measureInk(const Raster& image) {
        Ink ink;
        int minY = image.height, maxY = 0;
        for (int y = 0; y < image.height; ++y)
            for (int x = 0; x < image.width; ++x)
                if (image.alpha(x, y) > 0) minY = std::min(minY, y), maxY = std::max(maxY, y);
        double topSum = 0, topWeight = 0, bottomSum = 0, bottomWeight = 0, topY = 0, bottomY = 0;
        const auto middle = (minY + maxY) / 2;
        for (int y = minY; y <= maxY; ++y)
            for (int x = 0; x < image.width; ++x) {
                const auto alpha = image.alpha(x, y) / 255.0;
                ink.coverage += alpha;
                (y < middle ? topSum : bottomSum) += alpha * x;
                (y < middle ? topWeight : bottomWeight) += alpha;
                (y < middle ? topY : bottomY) += alpha * y;
            }
        ink.topCentroid = topWeight > 0 ? topSum / topWeight : 0;
        ink.bottomCentroid = bottomWeight > 0 ? bottomSum / bottomWeight : 0;
        ink.rise = topWeight > 0 && bottomWeight > 0 ? bottomY / bottomWeight - topY / topWeight : 0;
        return ink;
    }

    int inkBeyond(const Raster& image, int right) {
        int count = 0;
        for (int y = 0; y < image.height; ++y)
            for (int x = right; x < image.width; ++x)
                if (image.alpha(x, y) > 8) ++count;
        return count;
    }

    // 伪粗体覆盖率明显增大，伪斜体上半部向右偏，关闭伪造与真字面不变
    void syntheticRendering(canton::d3d11::Renderer& renderer) {
        Fonts fonts;
        fonts.defineTestFamilies();
        auto base = style(400, false, true, {uprightFamily});
        base.fontSize = 60;
        const Rect box{10, 10, 400, 140};
        const auto regular = measureInk(renderText(renderer, fonts.service, base, "HHHH", box));
        auto bold = base;
        bold.fontWeight = 700;
        check(regular.coverage > 100 && measureInk(renderText(renderer, fonts.service, bold, "HHHH", box)).coverage > regular.coverage * 1.12, "伪粗体的覆盖率应明显大于常规体");
        bold.fontSynthesis = false;
        check(std::abs(measureInk(renderText(renderer, fonts.service, bold, "HHHH", box)).coverage - regular.coverage) < 1.0, "关闭伪造后应与常规体完全一致");
        auto italic = base;
        italic.italic = true;
        const auto fakeItalic = measureInk(renderText(renderer, fonts.service, italic, "HHHH", box));
        const auto expected = syntheticItalicSkew * regular.rise;
        const auto shift = fakeItalic.topCentroid - fakeItalic.bottomCentroid;
        check(expected > 2.0 && std::abs(shift - expected) < expected * 0.15 && std::abs(regular.topCentroid - regular.bottomCentroid) < 1.0, "伪斜体的上半部应按错切系数向右偏移：偏移 " + std::to_string(shift) + "，应为 " + std::to_string(expected));
        check(std::abs(fakeItalic.coverage - regular.coverage) < regular.coverage * 0.05, "错切不应改变覆盖面积");
    }

    // 斜体末字伸出前进量的部分不是视觉溢出，不裁；排版比文字框宽才裁，对齐 Compose 的 hasVisualOverflow
    void italicOverhang(canton::d3d11::Renderer& renderer) {
        Fonts fonts;
        fonts.defineTestFamilies();
        auto real = style(400, true);
        real.fontSize = 60;
        DrawOp op;
        const auto image = renderText(renderer, fonts.service, real, "ffff", {10, 10, 0, 80}, TextOverflow::Clip, &op);
        check(!textHasVisualOverflow(op), "前进量刚好等于文字框不是视觉溢出");
        check(inkBeyond(image, static_cast<int>(std::ceil(op.rect.x + op.rect.width)) + 1) > 0, "真斜体末字伸出前进量的部分不应被裁掉");
        const auto bounds = drawOpBounds(op).rect;
        check(bounds.x + bounds.width >= op.rect.x + op.textLayout->inkBounds.x + op.textLayout->inkBounds.width - 0.5f, "绘制边界应包含伸出的墨迹");
        auto fake = real;
        fake.fontFamily = {uprightFamily};
        const auto fakeImage = renderText(renderer, fonts.service, fake, "HHHH", {10, 10, 0, 80}, TextOverflow::Clip, &op);
        check(inkBeyond(fakeImage, static_cast<int>(std::ceil(op.rect.x + op.rect.width)) + 1) > 0, "伪斜体末字伸出前进量的部分不应被裁掉");
        const auto narrow = renderText(renderer, fonts.service, fake, "HHHH", {10, 10, 100, 80}, TextOverflow::Clip, &op);
        check(textHasVisualOverflow(op) && inkBeyond(narrow, 111) == 0, "真正溢出时 clip 应裁到文字框");
        check(inkBeyond(renderText(renderer, fonts.service, fake, "HHHH", {10, 10, 100, 80}, TextOverflow::Visible), 111) > 0, "visible 不应裁剪");
    }

    // 字形图集满了整体回收：同一帧里回收前后画的字都要和单独画时一模一样
    void atlasReclaim(canton::d3d11::Renderer& renderer) {
        Fonts fonts;
        fonts.defineTestFamilies();
        constexpr int width = 1024, height = 800;
        const auto label = [&](std::string text, Rect rect, float size) {
            DrawOp op;
            op.type = DrawOpType::DrawText;
            op.rect = rect;
            op.text = std::move(text);
            op.textStyle = style(400, false);
            op.textStyle.fontSize = size;
            op.color = 0xffffffffu;
            DrawOpsBuilder::prepareText(op, fonts.service);
            return op;
        };
        const auto render = [&](std::vector<DrawOp> ops) {
            PublishedFrameContent content;
            content.scenePaint = test_support::fragmentOf(std::move(ops));
            content.viewport = {static_cast<float>(width), static_cast<float>(height)};
            auto target = renderer.createOffscreenTarget(width, height);
            renderer.render(*target, {&content, {}, 0xff000000u});
            return renderer.readPixels(*target);
        };
        const auto first = label("Before reclaim 0123", {10, 10, 900, 60}, 32);
        const auto last = label("After reclaim 4567", {10, 90, 900, 60}, 32);
        std::string glyphs;
        for (char c = '!'; c <= '~'; ++c) glyphs += c;
        std::vector<DrawOp> ops{first};
        // 大字号的全套字形，裁在下半区；面积远超 2048² 的图集
        DrawOp clip;
        clip.type = DrawOpType::PushClip;
        clip.rect = {0, 300, static_cast<float>(width), 500};
        ops.push_back(clip);
        for (int size = 120; size <= 240; size += 10) ops.push_back(label(glyphs, {0, 300, static_cast<float>(width), 500}, static_cast<float>(size)));
        DrawOp pop;
        pop.type = DrawOpType::PopClip;
        ops.push_back(pop);
        ops.push_back(last);
        const auto resets = renderer.counters().atlasResets;
        const auto combined = render(ops);
        check(renderer.counters().atlasResets > resets, "大量字形没有触发图集回收");
        const auto alone = render({first, last});
        for (int y = 0; y < 200; ++y)
            for (int x = 0; x < width; ++x) check(combined[static_cast<std::size_t>(y * width + x)] == alone[static_cast<std::size_t>(y * width + x)], "图集回收前后画的字与单独画时不一致，位置 (" + std::to_string(x) + ", " + std::to_string(y) + ")");
    }

    PropValue policy(const char* kind) {
        return PropValue::objectValue({{"kind", PropValue::stringValue(kind)}});
    }

    LayoutModifierSemantics fixedSize(float width, float height) {
        LayoutModifierSemantics value;
        value.kind = LayoutModifierKind::Size;
        value.width = width;
        value.height = height;
        return value;
    }

    TextModifier styledText(TextStyle textStyle) {
        auto text = test_support::text("AAAA", 0xff000000u, 100);
        text.style = std::move(textStyle);
        return text;
    }

    // 代际推进后 Text 重新测量、含文字命令的绘制层重建；代际不变时一律复用
    void rebuildOnGeneration() {
        Fonts fonts;
        LayoutTree tree;
        LayoutEngine layout(fonts.service);
        DrawOpsBuilder builder(fonts.service);
        DrawCommand command;
        command.kind = DrawCommandKind::Text;
        command.text = "AAAA";
        command.color = 0xff000000u;
        command.textStyle = style(400, false);
        const auto commands = std::make_shared<DrawCommands>(DrawCommands{command});
        tree.apply({CreateNodeMutation{1, NodeType::Layout}, SetPropMutation{1, "measurePolicy", policy("Box")}, SetModifierMutation{1, {{fixedSize(300, 100), {}}, {DrawModifier{}, {}}}}, CreateNodeMutation{2, NodeType::Layout}, SetPropMutation{2, "measurePolicy", policy("MinSize")}, SetModifierMutation{2, {{styledText(style(400, false)), {}}}}, InsertChildMutation{1, 2, 0}});
        for (auto& instance : tree.mutableNode(1).modifier.elements())
            if (std::holds_alternative<DrawModifier>(instance.descriptor.value)) instance.preparedDraw = commands;
        layout.layout(tree, 1, {0, 500, 0, 500});
        PaintWorkCounters counters;
        const auto first = builder.build(tree, 1, counters);
        tree.clearDirty();
        const auto drawLayer = [&] {
            for (const auto& instance : tree.node(1).modifier.elements())
                if (std::holds_alternative<DrawModifier>(instance.descriptor.value)) return instance.paintCache;
            return std::shared_ptr<const PaintLayerFragment>{};
        };
        const auto layerBefore = drawLayer();
        const auto textBefore = test_support::textLayoutOf(tree.node(2));
        check(layerBefore && layerBefore->before.size() + layerBefore->after.size() == 1, "绘制层应有一条文字命令");
        check(builder.build(tree, 1, counters).fragment == first.fragment, "代际不变时绘制应整体复用");
        fonts.defineTestFamilies();
        (void)fonts.service.environment();
        tree.invalidateFontEnvironment();
        layout.layout(tree, 1, {0, 500, 0, 500});
        const auto second = builder.build(tree, 1, counters);
        check(second.fragment != first.fragment, "字体代际推进后绘制片段没有重建");
        const auto layerAfter = drawLayer();
        check(layerAfter && layerAfter != layerBefore && layerAfter->drawCommands == commands, "含文字命令的绘制层应重建而命令不变");
        const auto textAfter = test_support::textLayoutOf(tree.node(2));
        check(textAfter && textAfter != textBefore && approx(textAfter->width, fonts.width("AAAA", style(400, false))), "Text 应按新登记的字体重新测量");
    }

    // 流水线在代际推进后无需其它失效也会重新测量；斜体 Text 默认 clip，末字伸出部分照样画出
    void pipeline(canton::d3d11::Renderer& renderer) {
        Fonts fonts;
        NativeScene scene;
        SceneFramePipeline frames{LayoutEngine{fonts.service}};
        PublishedFrame frame;
        MutationTransaction initial;
        initial.operations = {CreateNodeMutation{1, NodeType::Layout}, SetPropMutation{1, "measurePolicy", policy("MinSize")}, SetModifierMutation{1, {{styledText(style(400, false)), {}}}}};
        check(!frames.run(scene, 1, {0, 400, 0, 200}, &initial, true, frame).error, "初始帧失败");
        const auto width = [&] {
            return test_support::textLayoutOf(scene.node(1))->width;
        };
        const auto before = width();
        const auto measures = frames.counters().measures;
        check(!frames.run(scene, 1, {0, 400, 0, 200}, nullptr, true, frame).error && frames.counters().measures == measures, "代际不变时空跑一帧不应重新测量");
        fonts.defineTestFamilies();
        check(!frames.run(scene, 1, {0, 400, 0, 200}, nullptr, true, frame).error, "代际推进后的帧失败");
        check(frames.counters().measures == measures + 1 && approx(width(), fonts.width("AAAA", style(400, false))) && !approx(before, width()), "流水线没有在代际推进后重新测量 Text");

        auto italic = test_support::text("ffff", 0xff000000u, 60);
        italic.style = style(400, true);
        italic.style.fontSize = 60;
        MutationTransaction update;
        update.operations = {SetModifierMutation{1, {{italic, {}}}}};
        check(!frames.run(scene, 1, {0, 400, 0, 200}, &update, true, frame).error, "斜体 Text 的帧失败");
        const auto box = scene.node(1).bounds;
        auto target = renderer.createOffscreenTarget(400, 200);
        renderer.render(*target, {&frame.content, {}, 0});
        const Raster image{400, 200, renderer.readPixels(*target)};
        check(inkBeyond(image, static_cast<int>(std::ceil(box.x + box.width)) + 1) > 0, "完整流水线下真斜体末字伸出的部分被裁掉了");
    }
}

int main() {
    const char* step = "字面选择";
    try {
        const test_support::ComApartment apartment;
        canton::d3d11::Renderer renderer({canton::d3d11::AdapterPreference::Warp});
        selection();
        step = "OS/2 读数";
        fontTables();
        step = "自带字体族";
        bundledFamily();
        step = "系统回落";
        systemFallback();
        step = "系统字面";
        systemFaces();
        step = "排版几何";
        layoutGeometry();
        step = "伪造的渲染";
        syntheticRendering(renderer);
        step = "斜体伸出";
        italicOverhang(renderer);
        step = "图集回收";
        atlasReclaim(renderer);
        step = "代际重建";
        rebuildOnGeneration();
        step = "流水线";
        pipeline(renderer);
        std::cout << "字面选择、自带字体、系统回落、伪造与代际重排通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << step << "：" << error.what() << '\n';
        return 1;
    }
}
