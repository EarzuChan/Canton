// 布局贯通：布局值经 QuickJS 的类型化读取到原生（错误值在边界拒绝），以及 tests/fixtures/layout 的 .cta
// 经 Vite 构建、UI 包、QuickJS 进 core 场景树后的布局结果；Handle 用在错误父级时报错
#include "FixtureRuntime.h"

#include <core/LazyLayout.h>

#include <cmath>
#include <iostream>

using namespace canton::core;

namespace {
    void check(bool value, const std::string& message) {
        if (!value) throw std::runtime_error(message);
    }

    bool same(float a, float b) {
        return std::abs(a - b) < 0.01f;
    }

    void boundary() {
        canton::quickjs::QuickJsScriptHost host;
        const auto loaded = host.executeModule("布局值边界.js", R"JS(
            const n = globalThis.__CANTON_NATIVE__
            const reject = fn => { try { fn(); } catch { return; } throw new Error('无效布局值未拒绝'); }
            n.createNode(1, 'LayoutNode')
            n.updateBinding(n.registerBinding(1, 'measurePolicy'), {kind: 'Box'})
            n.createNode(2, 'LayoutNode')
            n.insertChild(1, 2, 0)
            const chain = elements => ({elements})
            const intrinsic = maximum => ({type: 'intrinsicWidth', value: {maximum}})
            reject(() => n.setModifier(2, chain([intrinsic('false')])))
            reject(() => n.setModifier(2, chain([{type: 'intrinsicWidth', value: {maximum: false, value: 10}}])))
            const spec = iterations => ({kind: 'tween', durationMillis: 100, delayMillis: 20, x1: 0, y1: 0, x2: 1, y2: 1, iterations, repeatMode: 'reverse'})
            const animation = value => ({type: 'animateContentSize', value: {animationSpec: value}})
            for (const count of [0, 1.5, Infinity, 2147483648]) reject(() => n.setModifier(2, chain([animation(spec(count))])))
            reject(() => n.setModifier(2, chain([animation({...spec(2), repeatMode: '未知'})])))
            reject(() => n.setModifier(2, chain([{type: 'matchParentSize', value: {width: 10}}])))
            reject(() => n.setModifier(2, chain([{type: 'animateItem', value: {fadeInSpec: 1, placementSpec: null, fadeOutSpec: null}}])))
            n.setModifier(2, chain([intrinsic(false), animation(spec(2147483647)), {type: 'text', value: {text: 'wide small words'}}]))
        )JS");
        check(loaded.ok, loaded.error);
        auto transaction = host.takePendingTransaction();
        check(transaction.has_value(), "类型化布局值未提交");
        NativeScene scene;
        SceneFramePipeline pipeline;
        PublishedFrame published;
        const auto result = pipeline.run(scene, 1, {0, 300, 0, 200}, &*transaction, true, published);
        if (result.error) throw std::runtime_error(*result.error);
        host.publishScene(scene);
        check(host.completeRearrange(transaction->rearrange).ok, "类型化布局事务未完成");
        const auto& node = scene.node(2);
        check(std::get<LayoutModifierSemantics>(node.modifier.elements()[0].descriptor.value).kind == LayoutModifierKind::IntrinsicWidth && node.bounds.width > 0, "固有 Modifier 未经过类型化读取和正式测量");
        const auto& spec = std::get<AnimateContentSizeModifier>(node.modifier.elements()[1].descriptor.value).animationSpec;
        check(spec.iterations == 2147483647 && spec.reverse && spec.kind == AnimationKind::Tween, "有限重复描述跨原生边界丢失次数或方向");
    }

    // Handle 产出的父布局数据用在不认它的父级下：原生按数据种类校验，报错不静默
    void wrongParent() {
        const auto run = [](const std::string& source) {
            canton::quickjs::QuickJsScriptHost host;
            const auto loaded = host.executeModule("错误父级.js", "const n = globalThis.__CANTON_NATIVE__\n" + source);
            if (!loaded.ok) return loaded.error;
            auto transaction = host.takePendingTransaction();
            NativeScene scene;
            SceneFramePipeline pipeline;
            PublishedFrame published;
            const auto result = pipeline.run(scene, 1, {0, 300, 0, 200}, &*transaction, true, published);
            return result.error.value_or(std::string());
        };
        const auto tree = [](const char* parent, const char* element) {
            return std::string("n.createNode(1, 'LayoutNode'); n.updateBinding(n.registerBinding(1, 'measurePolicy'), {kind: '") + parent + "'})\n" + "n.createNode(2, 'LayoutNode'); n.insertChild(1, 2, 0)\n" + "n.setModifier(2, {elements: [" + element + "]})\n";
        };
        check(run(tree("Box", "{type: 'weight', value: {weight: 1, fill: true}}")).find("weight") != std::string::npos, "Box 下的 weight 没有报错");
        check(run(tree("Row", "{type: 'matchParentSize', value: {}}")).find("matchParentSize") != std::string::npos, "Row 下的 matchParentSize 没有报错");
        check(!run(tree("Column", "{type: 'fillParentMaxSize', value: {fraction: 1}}")).empty(), "Lazy 之外的 fillParentMaxSize 没有报错");
        check(run(tree("Column", "{type: 'animateItem', value: {fadeInSpec: null, placementSpec: null, fadeOutSpec: null}}")).find("animateItem") != std::string::npos, "Lazy 之外的 animateItem 没有报错");
        check(run(tree("Row", "{type: 'weight', value: {weight: 1, fill: true}}")).empty(), "合法的父级数据被误拒");
    }

    void fixture(const std::filesystem::path& directory) {
        test_support::FixtureRuntime fixture(directory, {0, 400, 0, 600});
        fixture.tick();
        NodeId intrinsic = 0, flowing = 0, columns = 0, weighted = 0, matching = 0, animated = 0;
        const auto& tree = fixture.scene().tree();
        for (auto id : tree.nodeIds()) {
            const auto& node = tree.node(id);
            if (const auto* flow = std::get_if<FlowMeasurePolicy>(&node.measurePolicy)) {
                if (!flow->horizontal)
                    columns = id;
                else if (flow->maxItems == 2)
                    weighted = id;
                else
                    flowing = id;
            }
            for (const auto& instance : node.modifier.elements()) {
                if (const auto* layout = std::get_if<LayoutModifierSemantics>(&instance.descriptor.value); layout && layout->kind == LayoutModifierKind::IntrinsicHeight) intrinsic = id;
                if (const auto* data = std::get_if<ParentDataModifierSemantics>(&instance.descriptor.value); data && data->kind == ParentDataKind::MatchParentSize) matching = id;
                if (std::holds_alternative<AnimateContentSizeModifier>(instance.descriptor.value)) animated = id;
            }
        }
        check(intrinsic && flowing && columns && weighted && matching && animated, "夹具没有交付完整的布局受体");
        check(tree.node(intrinsic).bounds.width == 50 && tree.node(intrinsic).bounds.height == 40, "IntrinsicSize 布局错误");
        check(tree.node(flowing).bounds.width == 70 && tree.node(flowing).bounds.height == 27 && tree.node(flowing).flowLines.size() == 2, "FlowRow 换行错误");
        check(tree.node(columns).bounds.width == 67 && tree.node(columns).bounds.height == 30, "FlowColumn 布局错误");
        const auto& weights = tree.node(weighted).children;
        check(weights.size() == 3 && tree.node(weights[0]).bounds.width == 30 && tree.node(weights[1]).bounds.width == 60 && tree.node(weights[2]).bounds.width == 95, "Flow 的 weight 没有逐行分配");
        check(tree.node(matching).bounds.width == 20 && tree.node(matching).bounds.height == 10, "matchParentSize 贡献或丢失了父尺寸");
        fixture.runtime().enqueueStringEvent(fixture.submit(), "展开");
        const auto at = [&](double time) {
            fixture.advance(time - fixture.now() - 16);
            fixture.tick();
        };
        at(20);
        const auto sample = [&](double time, float width, const char* text) {
            at(time);
            check(same(fixture.scene().node(animated).bounds.width, width), "原生尺寸 repeatable 与绝对时间不符：" + std::to_string(fixture.scene().node(animated).bounds.width));
            check(fixture.findText(text) != 0, std::string("动画 Ref 没有按同一有限重复规格交付文本：") + text);
        };
        sample(90, 45, "动画:5.0");
        sample(150, 60, "动画:10.0");
        sample(210, 45, "动画:5.0");
        sample(260, 60, "动画:10.0");
        check(fixture.scene().tree().activeAnimationCount() == 0, "有限重复完成后原生尺寸仍申请动画帧");
    }
}

int main(int argc, char** argv) {
    const char* step = "布局值边界";
    try {
        check(argc == 2, "需要构建好的 layout 夹具目录");
        boundary();
        step = "错误父级";
        wrongParent();
        step = "layout 夹具";
        fixture(argv[1]);
        std::cout << "布局值边界、错误父级与 layout 夹具贯通通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << step << "：" << error.what() << '\n';
        return 1;
    }
}
