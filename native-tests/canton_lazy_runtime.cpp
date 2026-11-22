// Lazy 端到端：tests/fixtures/lazy 的四种容器经 UI 包、QuickJS 到 core。可见项元数据、只材料化视口附近的项、
// 项内局部状态、PX 滚动与大跳转、退挂重建按 key 恢复、前插/重排按 key 保持锚点、滚动动画、缓存上限、失败候选不污染发布帧。
// Input 的焦点、选区与 IME 部分属 M4
#include "FixtureRuntime.h"

#include <core/InputSession.h>
#include <core/LazyLayout.h>
#include <runtime/InputTextSession.h>

#include <cmath>
#include <iostream>

using namespace canton::core;

namespace {
    void check(bool value, const std::string& message) {
        if (!value) throw std::runtime_error(message);
    }

    NodeId lazyNode(const NativeScene& scene) {
        for (auto id : scene.tree().nodeIds())
            if (std::holds_alternative<LazyMeasurePolicy>(scene.node(id).measurePolicy)) return id;
        throw std::runtime_error("Lazy 未经正式 Layout 创建");
    }

    void run(const std::filesystem::path& directory) {
        test_support::FixtureRuntime fixture(directory, {0, 320, 0, 350});
        fixture.step = "首次材料化";
        fixture.tick();
        fixture.settle();
        const auto state = [&] {
            return *fixture.scene().node(lazyNode(fixture.scene())).lazy;
        };
        const auto first = [&] {
            return state().snapshot.firstVisibleItemIndex;
        };
        const auto offset = [&] {
            return state().snapshot.firstVisibleItemScrollOffset;
        };
        check(state().snapshot.totalItemsCount == 10000, "Lazy 未发布可见项元数据");
        check(fixture.scene().node(lazyNode(fixture.scene())).children.size() < 64 && fixture.scene().tree().size() < 260, "Lazy 首帧全量创建了离屏业务项");
        const auto item = fixture.findText("项:0:0");
        check(item != 0, "Lazy 项内容没有创建");
        const auto click = test_support::event(fixture.scene().node(item), EventSlotKind::Click);
        check(click.valid(), "Lazy 项的点击槽没有注册");
        fixture.runtime().enqueueEvent(click);
        fixture.settle();
        check(fixture.findText("项:0:1") != 0, "Lazy 项内局部状态未更新");
        fixture.command("pixel");
        check(first() > 1000, "PX 滚动未触发 Lazy 可见范围重新材料化");
        fixture.command("jump");
        check(first() == 9000 && std::abs(offset() - 5) < 0.1f, "大跳转没有在同帧材料化请求项，或丢了可变尺寸补测后的偏移");
        check(fixture.scene().node(lazyNode(fixture.scene())).children.size() < 64, "大跳转创建了中间所有项");
        fixture.command("hide");
        fixture.command("show");
        check(first() == 9000 && std::abs(offset() - 5) < 0.1f, "Lazy 退挂重建按旧绝对 PX 恢复导致可变尺寸首项漂移");
        fixture.command("hide");
        fixture.command("prepend");
        fixture.command("show");
        check(first() == 9001 && std::abs(offset() - 5) < 0.1f && fixture.findText("项:9000:0") != 0, "隐藏期间数据前插丢失已发布首项 key 与偏移");
        fixture.command("unprepend");
        check(first() == 9000, "移除临时前插项后未恢复首项 key");
        fixture.command("hide");
        fixture.command("pixel");
        fixture.command("show");
        check(first() > 1000 && first() != 9000, "隐藏期间的 PX 定位被旧逻辑位置恢复覆盖");
        fixture.command("hide");
        fixture.command("start");
        fixture.command("show");
        check(first() == 0, "隐藏期间的按项定位被旧逻辑位置恢复覆盖");
        fixture.command("jump");
        // 验证 Lazy 项内 Input 在数据前插（insert）与反转重排（reverse）前后完整保留焦点、选区与活跃 IME compositionRange
        NodeId input9000 = 0;
        for (const auto id : fixture.scene().tree().nodeIds()) {
            if (const auto* ed = test_support::editable(fixture.scene().node(id))) {
                if (std::get<TextFieldModifier>(ed->descriptor.value).value == "焦点9000") input9000 = id;
            }
        }
        check(input9000 != 0, "Lazy 第 9000 项未挂载测试 Input");
        {
            InputSession lazyInput;
            canton::runtime::InputTextSession lazyText(fixture.text());
            InputCallbacks lazyCb;
            struct PrepGuard {
                canton::runtime::CantonRuntime& rt;
                ~PrepGuard() { rt.setInteractionPreparation({}); }
            } prepGuard{fixture.runtime()};
            fixture.runtime().setInteractionPreparation([&](LayoutTree& tree) { lazyInput.prepareLazyInteraction(tree); });
            check(lazyInput.requestFocus(fixture.runtime().scene().tree(), input9000, lazyText, lazyCb), "Lazy Input 聚焦请求失败");
            (void)lazyText.state().selectRange(0, 6);  // 选中 "焦点"
            check(lazyText.setComposition(fixture.scene().tree(), true, "zhongwen", 8, {{0, 8, true}}, lazyCb), "Lazy Input 进入 IME 组合失败");
            const auto compRangeBefore = lazyText.state().compositionRange();

            fixture.command("insert");
            lazyInput.synchronizePublishedInput(fixture.scene().tree(), true, lazyText);
            check(first() == 9001 && fixture.findText("项:9000:0") != 0, "前插项没有保持稳定 key 锚点");
            check(lazyText.isActive(fixture.scene().tree(), true) && lazyText.state().composing() && lazyText.state().compositionRange() == compRangeBefore, "Lazy 前插数据重排丢失了 Input 焦点或活跃 IME compositionRange");

            fixture.command("reverse");
            lazyInput.synchronizePublishedInput(fixture.scene().tree(), true, lazyText);
            check(fixture.findText("项:9000:0") != 0 && first() == 999, "重排没有按 key 保持首项");
            check(lazyText.isActive(fixture.scene().tree(), true) && lazyText.state().composing() && lazyText.state().text() == "zhongwen9000", "Lazy 列表反转重排丢失了 Input 活跃 IME 预编辑状态");
            check(lazyText.commitComposition(fixture.scene().tree(), true, std::string("中文"), lazyCb) && lazyText.state().text() == "中文9000", "重排后提交 IME 结果串失败");
            (void)lazyText.state().selectRange(0, 6);
            fixture.command("prepend");
            lazyInput.synchronizePublishedInput(fixture.scene().tree(), true, lazyText);
            check(lazyText.state().selectedText() == "中文", "Lazy 重排未保留非组合态 Input 选区");
            fixture.command("unprepend");
            lazyInput.clearFocus(fixture.runtime().scene().tree(), lazyText, lazyCb);
            lazyInput.prepareLazyInteraction(fixture.runtime().scene().tree());
        }
        fixture.command("start");
        check(first() == 0, "返回首项失败");
        fixture.command("animate");
        check(first() == 100, "Lazy 滚动动画没有复用帧时钟并最终校正到请求项");
        fixture.command("start");
        fixture.command("row");
        const auto policy = [&] {
            return std::get<LazyMeasurePolicy>(fixture.scene().node(lazyNode(fixture.scene())).measurePolicy);
        };
        check(policy().horizontal && !policy().grid, "LazyRow 没有复用同一套布局与滚动链路");
        fixture.command("verticalGrid");
        check(state().cells == 3 && policy().grid, "Fixed 网格列数错误");
        fixture.command("horizontalGrid");
        check(state().cells >= 1 && policy().horizontal && policy().grid, "Adaptive 网格没有经单位换算");
        check(fixture.scene().tree().size() < 600 && fixture.host().bindingCount() < 2000, "Lazy 缓存没有限制活动原生实例或绑定");
        const auto revision = fixture.runtime().publishedFrame().revision;
        const auto size = fixture.scene().tree().size();
        fixture.runtime().enqueueStringEvent(fixture.submit(), "bad");
        const auto failed = fixture.runtime().pumpFrame(1, {0, 320, 0, 350}, fixture.advance(16));
        check(!failed.ok && fixture.runtime().publishedFrame().revision == revision && fixture.scene().tree().size() == size, "失败的 Lazy 候选污染了已发布帧");
    }
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "需要构建好的 lazy 夹具目录");
        run(argv[1]);
        std::cout << "Lazy 四容器、可变尺寸、key 锚点、缓存与原子提交通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
