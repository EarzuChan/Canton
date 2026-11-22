// 保留绘制：排版资源的共享、缓存预算与寿命；文字几何（选区、光标、显式换行、组合字符、回落字体的 em 与行距）；
// 发布帧的片段复用（滚动、改色、屏外更新、失败候选、节点重建），以及保留的片段树与摊平后的指令画出同一幅图。
// 文字用系统字体，只做不比基线的自洽断言
#include "RenderSupport.h"
#include "TextFixtures.h"

#include <core/Paint.h>
#include <core/SceneFramePipeline.h>
#include <d3d11/DWriteText.h>
#include <d3d11/Renderer.h>

#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace canton::core;
using canton::d3d11::DWriteTextMeasurer;
using canton::d3d11::DWriteTextResource;

namespace {
    void check(bool condition, const std::string& message) {
        if (!condition) throw std::runtime_error(message);
    }

    bool same(float left, float right) {
        return std::abs(left - right) < 0.01f;
    }

    LayoutModifierSemantics size(float width, float height) {
        LayoutModifierSemantics value;
        value.kind = LayoutModifierKind::Size;
        value.width = width;
        value.height = height;
        return value;
    }

    PropValue policy(const char* kind) {
        return PropValue::objectValue({{"kind", PropValue::stringValue(kind)}});
    }

    TextStyle textStyle(float fontSize, float lineHeight) {
        TextStyle result;
        result.fontSize = fontSize;
        result.lineHeight = lineHeight;
        return result;
    }

    struct Pixels {
        int width = 0, height = 0;
        std::vector<std::uint32_t> data;
        bool operator==(const Pixels&) const = default;
    };

    Pixels render(canton::d3d11::Renderer& renderer, const PlacedPaintFragment& paint, int width, int height) {
        PublishedFrameContent content;
        content.scenePaint = paint;
        content.viewport = {static_cast<float>(width), static_cast<float>(height)};
        auto target = renderer.createOffscreenTarget(width, height);
        renderer.render(*target, {&content, {}, 0xff000000u});
        return {width, height, renderer.readPixels(*target)};
    }

    Pixels renderOps(canton::d3d11::Renderer& renderer, std::vector<DrawOp> ops, int width, int height) {
        return render(renderer, test_support::fragmentOf(std::move(ops)), width, height);
    }

    // 保留的片段树（含整段剔除）与摊平成指令后画出的图必须逐像素一致
    void compareWithFlattened(canton::d3d11::Renderer& renderer, const PublishedFrame& frame) {
        const auto retained = render(renderer, frame.content.scenePaint, 320, 240);
        const auto flattened = renderOps(renderer, exportDrawOps(frame.content.scenePaint), 320, 240);
        for (std::size_t index = 0; index < retained.data.size(); ++index)
            if (retained.data[index] != flattened.data[index]) throw std::runtime_error("保留片段与摊平指令画出的图不同，坐标：" + std::to_string(index % 320) + "," + std::to_string(index / 320));
    }

    void textResources(canton::d3d11::Renderer& renderer) {
        DWriteTextMeasurer backend;
        TextLayoutService service(backend, 3, 24000);
        const auto mixed = service.layout("中文 English e\xcc\x81 🙂 ffi", textStyle(18, 24), {0, 90});
        check(mixed->resource && mixed->lines.size() > 1 && mixed->baseline > 0, "混排必须产生真实排版资源、换行和基线");
        const auto repeated = service.layout(mixed->text, mixed->style, mixed->options, mixed);
        check(repeated == mixed && service.counters().nodeReuses == 1, "节点没有保留最近有效资源");
        check(service.layout(mixed->text, mixed->style, mixed->options) == mixed, "同请求没有共享服务结果");
        const auto empty = service.layout("", textStyle(18, 24));
        check(empty->width == 0 && empty->height >= 24 && empty->lines.size() == 1, "空文本排版不正确");
        check(service.layout("第一行\n\n第三行\n", textStyle(18, 24), {0, 100})->lines.size() == 4, "显式空行和末尾换行必须保留");
        check(service.layout("alpha beta gamma delta epsilon", textStyle(18, 24), {0, 60})->lines.size() > 2, "maxLines 为零必须不限行");
        const auto ellipsis = service.layout("alpha beta gamma delta epsilon", textStyle(18, 24), {2, 60, false, true});
        check(ellipsis->lines.size() == 2 && ellipsis->resource && ellipsis->truncated, "多行省略未经过正式排版服务");
        const auto single = service.layout("中文 English e\xcc\x81 🙂 ffi", textStyle(18, 24), {1, 0, true});
        check(single->lines.size() == 1 && single->width > 90, "单行文本被错误换行");
        for (const auto& run : single->lines.front().runs) {
            check(run.start <= run.end && run.end <= single->text.size(), "字形簇越过 UTF-8 文本边界");
            const auto rect = service.caretRect(*single, run.start);
            check(std::isfinite(rect.x) && std::isfinite(rect.y), "光标几何不是有限数值");
        }
        check(service.counters().cachedEntries <= 3 && service.counters().cachedBytes <= 24000 && service.counters().evictions > 0, "LRU 没有遵守条目与内存预算");
        // 同一份排版资源画成两种颜色，不串色、不重新排版
        DrawOp red;
        red.type = DrawOpType::DrawText;
        red.rect = {10, 10, 200, 30};
        red.text = "同一字形资源";
        red.textStyle.fontSize = 20;
        red.maxLines = 1;
        red.color = 0xffff0000u;
        DrawOpsBuilder::prepareText(red, service);
        auto blue = red;
        blue.rect.y = 50;
        blue.color = 0xff0000ffu;
        check(blue.textLayout == red.textLayout, "同样的文字没有共享排版资源");
        const auto count = service.counters().layoutsCreated;
        const auto colors = renderOps(renderer, {red, blue}, 240, 100);
        bool sawRed = false, sawBlue = false;
        for (int y = 0; y < 100; ++y)
            for (int x = 0; x < 240; ++x) {
                const auto pixel = colors.data[static_cast<std::size_t>(y * 240 + x)];
                const auto r = (pixel >> 16) & 255, b = pixel & 255;
                sawRed = sawRed || (y < 40 && r > 128 && b < 32);
                sawBlue = sawBlue || (y >= 40 && b > 128 && r < 32);
                check(!(y < 40 && b > 64) && !(y >= 40 && r > 64), "共享文字资源串色");
            }
        check(sawRed && sawBlue && service.counters().layoutsCreated == count, "共享文字资源没画出来或绘制时重新排版");
        service.clearCache();
        check(service.counters().liveResources >= 5 && mixed->resource, "淘汰缓存错误释放了外部持有的资源");
        service.invalidateFontEnvironment();
        check(service.layout(mixed->text, mixed->style, mixed->options, mixed) != mixed, "字体环境变化没有失效旧结果");
        TextLayoutService tiny(backend, 1, 1);
        auto oversized = tiny.layout("超大单项不进入缓存", textStyle(18, 24));
        std::weak_ptr<const TextLayout> weak = oversized;
        check(tiny.counters().cachedEntries == 0 && tiny.counters().liveResources == 1, "超预算资源仍进入了 LRU");
        oversized.reset();
        check(weak.expired() && tiny.counters().liveResources == 0 && tiny.counters().liveBytes == 0, "最后持有者释放后资源没有回收");
    }

    void textGeometry(canton::d3d11::Renderer& renderer) {
        DWriteTextMeasurer backend;
        TextLayoutService service(backend);
        const auto wrapped = service.layout("a b c d e f", textStyle(18, 24), {0, 35});
        const auto selection = service.boundsForRange(*wrapped, 0, wrapped->text.size(), {10, 20});
        check(wrapped->lines.size() > 1 && selection.size() == wrapped->lines.size(), "自动换行的全选没有覆盖全部行");
        check(selection == service.boundsForRange(*wrapped, wrapped->text.size(), 0, {10, 20}), "反向选区与正向选区不一致");
        for (std::size_t index = 0; index < wrapped->lines.size(); ++index) {
            const auto& line = wrapped->lines[index];
            check(same(selection[index].x, 10) && same(selection[index].y, 20 + line.y) && same(selection[index].width, line.width), "全选没有使用当前行的完整宽度");
            const auto caret = service.caretRect(*wrapped, line.start);
            check(same(caret.x, 0) && same(caret.y, line.y), "自动换行边界的光标没有归到下一行行首");
            check(service.byteIndexAtPoint(*wrapped, {0, line.y + line.height * 0.5f}) == line.start, "点击自动换行行首没有返回该行起点");
        }
        const auto& secondLine = wrapped->lines[1];
        const auto partial = service.boundsForRange(*wrapped, secondLine.start, secondLine.runs.front().end);
        check(partial.size() == 1 && same(partial[0].x, 0) && same(partial[0].width, secondLine.runs.front().width), "从换行边界开始的部分选区错误");

        const auto a = service.layout("A", textStyle(18, 24));
        const auto b = service.layout("B", textStyle(18, 24));
        const auto explicitLines = service.layout("A\n\nB\n", textStyle(18, 24));
        check(explicitLines->lines.size() == 4 && same(explicitLines->lines[0].width, a->width) && same(explicitLines->lines[2].width, b->width), "换行符改变了可见文字的行宽");
        check(same(explicitLines->lines[1].width, 0) && same(explicitLines->lines[3].width, 0), "空行被计入非零宽度");
        const auto beforeBreak = service.caretRect(*explicitLines, 1);
        const auto afterBreak = service.caretRect(*explicitLines, 2);
        check(same(beforeBreak.x, a->width) && same(beforeBreak.y, 0) && same(afterBreak.x, 0) && same(afterBreak.y, explicitLines->lines[1].y), "显式换行两侧的光标位置错误");
        const auto withSpace = service.layout("A ", textStyle(18, 24));
        check(withSpace->width > a->width && same(service.layout("A \nB", textStyle(18, 24))->lines[0].width, withSpace->width), "排除换行时错误丢弃了真实尾随空格");

        // 显式换行后居中或末端对齐，与把两行分开画在同一位置一致
        for (const auto alignment : {TextAlign::Center, TextAlign::End}) {
            DrawOp multiline;
            multiline.type = DrawOpType::DrawText;
            multiline.text = "A\nA";
            multiline.textStyle = textStyle(18, 24);
            multiline.rect = {10, 10, 180, 80};
            multiline.textAlign = alignment;
            multiline.color = 0xffffffffu;
            DrawOpsBuilder::prepareText(multiline, service);
            auto first = multiline;
            first.text = "A";
            first.maxLines = 1;
            first.textLayout.reset();
            DrawOpsBuilder::prepareText(first, service);
            auto second = first;
            second.rect.y += multiline.textLayout->lines[1].y;
            check(renderOps(renderer, {multiline}, 200, 100) == renderOps(renderer, {first, second}, 200, 100), "显式换行改变了居中或末端对齐的绘制位置");
        }

        // 组合字符与 ZWJ 序列：末尾字符几何不丢；点击只落在字形簇边界，且随横坐标单调
        for (const std::string text : {"e\xcc\x81X", "👨‍👩‍👧‍👦X"}) {
            const auto layout = service.layout(text, textStyle(18, 24), {1, 0, true});
            check(layout->lines[0].end == text.size() && !layout->truncated, "组合文字的末尾字符几何丢失");
            const auto cluster = text.size() - 1;
            std::size_t previous = 0;
            for (float x = 0; x <= layout->width + 1; x += 0.25f) {
                const auto index = service.byteIndexAtPoint(*layout, {x, 12});
                check(index == 0 || index == cluster || index == text.size(), "点击落到了字形簇内部：" + std::to_string(index));
                check(index >= previous, "点击落点没有随横坐标单调");
                previous = index;
            }
            check(previous == text.size() && service.xForByteIndex(*layout, cluster) > 0, "组合字形簇没有宽度");
            const auto range = service.boundsForRange(*layout, 0, cluster);
            check(range.size() == 1 && same(range[0].width, service.xForByteIndex(*layout, cluster)), "组合字形簇的选区几何错误");
        }
    }

    // 字号是 em：拉丁与中文回落 run 的 em 一致；行高是绝对行距，与回落字体撑高的行盒无关
    void emAndLineHeight() {
        DWriteTextMeasurer backend;
        TextLayoutService service(backend);
        const auto text = std::string("Hello \xe4\xbd\xa0\xe5\xa5\xbd");
        for (const float fontSize : {10.0f, 14.0f, 30.0f}) {
            const auto resource = std::dynamic_pointer_cast<const DWriteTextResource>(service.layout(text, textStyle(fontSize, 0))->resource);
            check(resource && resource->runs.size() >= 2, "拉丁与中文没有分成回落字体的多个 run");
            for (const auto& run : resource->runs) check(same(run.emSize, fontSize), "回落字体的 em 与字号不一致");
        }
        for (const float lineHeight : {14.0f, 20.0f, 30.0f}) {
            const auto layout = service.layout("Hello\n\xe4\xbd\xa0\xe5\xa5\xbd\nHello \xe4\xbd\xa0\xe5\xa5\xbd", textStyle(14, lineHeight));
            check(layout->lines.size() == 3 && same(layout->height, lineHeight * 3), "多行总高不是行数乘行高");
            const auto offset = layout->lines[0].baseline - layout->lines[0].y;
            for (std::size_t index = 0; index < layout->lines.size(); ++index) {
                const auto& line = layout->lines[index];
                check(same(line.y, lineHeight * static_cast<float>(index)) && same(line.height, lineHeight), "行盒位置或行高不是绝对行距");
                check(same(line.baseline - line.y, offset), "含回落字体的行基线相对行盒位置发生漂移");
            }
        }
        check(same(service.layout("A", textStyle(14, 0))->lineHeight, 14 * defaultLineHeightRatio), "行高默认值不是字号的 1.2 倍");
    }

    void publishedReuse(canton::d3d11::Renderer& renderer) {
        DWriteTextMeasurer backend;
        TextLayoutService service(backend);
        NativeScene scene;
        SceneFramePipeline pipeline{LayoutEngine{service}};
        PublishedFrame frame;
        LayoutModifierSemantics scroll;
        scroll.kind = LayoutModifierKind::VerticalScroll;
        MutationTransaction initial;
        initial.operations = {CreateNodeMutation{1, NodeType::Layout}, SetPropMutation{1, "measurePolicy", policy("Column")}, SetModifierMutation{1, {{size(320, 240), {}}, {scroll, "scroll"}}}};
        for (NodeId id = 2; id < 82; ++id) {
            initial.operations.push_back(CreateNodeMutation{id, NodeType::Layout});
            initial.operations.push_back(SetPropMutation{id, "measurePolicy", policy("MinSize")});
            initial.operations.push_back(SetModifierMutation{id, {{size(300, 30), {}}, {test_support::text("音轨 " + std::to_string(id) + " English 🙂", 0xffabcdefu, 18), {}}}});
            initial.operations.push_back(InsertChildMutation{1, id, static_cast<std::uint32_t>(id - 2)});
        }
        const auto publish = [&](const MutationTransaction* transaction, const FrameFinalizer& extra = {}) {
            return pipeline.run(scene, 1, {0, 320, 0, 240}, transaction, true, frame, extra);
        };
        check(!publish(&initial).error, "初始列表发布失败");
        const auto warmedLayouts = service.counters().layoutsCreated;
        const auto firstFragment = scene.node(2).paintCache;
        const auto firstText = test_support::textLayoutOf(scene.node(2));
        const auto previousFrame = frame;
        compareWithFlattened(renderer, frame);
        // 重复绘制不排版；视口外的项整段剔除，实例数只够画屏内的八九项
        const auto instances = renderer.counters().instances;
        for (int repeat = 0; repeat < 4; ++repeat) (void)render(renderer, frame.content.scenePaint, 320, 240);
        check(service.counters().layoutsCreated == warmedLayouts, "重复绘制发生了排版");
        check((renderer.counters().instances - instances) / 4 < 400, "屏外稳定文本没有被剔除：每帧实例 " + std::to_string((renderer.counters().instances - instances) / 4));

        auto before = pipeline.counters();
        scroll.scrollValue = 600;
        MutationTransaction movement;
        movement.operations = {SetModifierMutation{1, {{size(320, 240), {}}, {scroll, "scroll"}}}};
        check(!publish(&movement).error, "滚动发布失败");
        check(scene.node(2).paintCache == firstFragment && test_support::textLayoutOf(scene.node(2)) == firstText, "整体滚动重建了稳定子片段或文字");
        check(pipeline.counters().paintWork.contentBuilds == before.paintWork.contentBuilds && pipeline.counters().measures == before.measures, "滚动重建了内容命令或执行测量");
        check(service.counters().layoutsCreated == warmedLayouts, "滚动产生新排版");
        compareWithFlattened(renderer, frame);
        compareWithFlattened(renderer, previousFrame);

        const auto sibling = scene.node(3).paintCache;
        MutationTransaction color;
        color.operations = {SetModifierMutation{2, {{size(300, 30), {}}, {test_support::text("音轨 2 English 🙂", 0xffff3311u, 18), {}}}}};
        check(!publish(&color).error, "颜色更新发布失败");
        check(test_support::textLayoutOf(scene.node(2)) == firstText && scene.node(3).paintCache == sibling && service.counters().layoutsCreated == warmedLayouts, "纯颜色更新破坏了文字或兄弟片段复用");
        MutationTransaction offscreen;
        offscreen.operations = {SetModifierMutation{2, {{size(300, 30), {}}, {test_support::text("屏外更新后的文字", 0xffff3311u, 18), {}}}}};
        check(!publish(&offscreen).error && test_support::textLayoutOf(scene.node(2))->text == "屏外更新后的文字", "屏外文字更新被冻结");
        scroll.scrollValue = 0;
        movement.operations = {SetModifierMutation{1, {{size(320, 240), {}}, {scroll, "scroll"}}}};
        check(!publish(&movement).error, "滚回发布失败");
        compareWithFlattened(renderer, frame);

        const auto retained = frame;
        MutationTransaction failure;
        failure.operations = {SetModifierMutation{2, {{size(300, 30), {}}, {test_support::text("失败候选不能污染成功帧", 0xffff3311u, 18), {}}}}};
        check(publish(&failure, [](const auto&, auto&) { throw std::runtime_error("注入准备失败"); }).error.has_value(), "准备失败没有上报");
        check(frame.revision == retained.revision && frame.content.scenePaint == retained.content.scenePaint && test_support::textOf(scene.node(2)) == "屏外更新后的文字", "失败候选改写了已发布内容");
        compareWithFlattened(renderer, frame);
        MutationTransaction recreate;
        recreate.operations = {RemoveChildMutation{1, 2}, DeleteNodeMutation{2}, CreateNodeMutation{2, NodeType::Layout}, SetPropMutation{2, "measurePolicy", policy("MinSize")}, SetModifierMutation{2, {{size(300, 30), {}}, {test_support::text("新代际", 0xffff3311u, 18), {}}}}, InsertChildMutation{1, 2, 0}};
        check(!publish(&recreate).error && test_support::textLayoutOf(scene.node(2)) != firstText, "节点代际重用命中了过期文字");
        compareWithFlattened(renderer, frame);
    }

    // 已发布的帧持有它用到的排版资源；节点退休、帧也释放后资源回收
    void publicationLifetime() {
        DWriteTextMeasurer backend;
        TextLayoutService service(backend);
        SceneFramePipeline pipeline{LayoutEngine{service}};
        std::weak_ptr<const TextLayout> retired;
        {
            NativeScene temporary;
            PublishedFrame retainedFrame;
            MutationTransaction create;
            create.operations = {CreateNodeMutation{7, NodeType::Layout}, SetPropMutation{7, "measurePolicy", policy("MinSize")}, SetModifierMutation{7, {{test_support::text("寿命验证", 0xffffffffu, 18), {}}}}};
            check(!pipeline.run(temporary, 7, {0, 320, 0, 240}, &create, true, retainedFrame).error, "寿命场景发布失败");
            retired = test_support::textLayoutOf(temporary.node(7));
            MutationTransaction remove;
            remove.operations = {DeleteNodeMutation{7}};
            const auto oldFrame = retainedFrame;
            (void)pipeline.run(temporary, 7, {0, 320, 0, 240}, &remove, true, retainedFrame);
            service.clearCache();
            check(!retired.expired(), "旧帧不能继续持有已退休节点的资源");
        }
        service.clearCache();
        check(retired.expired(), "旧帧释放后文本资源没有回收");
    }
}

int main() {
    const char* step = "排版资源";
    try {
        const test_support::ComApartment apartment;
        canton::d3d11::Renderer renderer({canton::d3d11::AdapterPreference::Warp});
        textResources(renderer);
        step = "文字几何";
        textGeometry(renderer);
        step = "em 与行距";
        emAndLineHeight();
        step = "发布帧复用";
        publishedReuse(renderer);
        step = "发布寿命";
        publicationLifetime();
        std::cout << "排版资源、文字几何、片段复用与发布寿命通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << step << "：" << error.what() << '\n';
        return 1;
    }
}
