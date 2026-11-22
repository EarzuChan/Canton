// 帧提交里不依赖输入的部分：手动 VBlank 的采样节拍；测量失败撤销候选、显式重新提交与上下文重置；
// 收尾失败不发布半成品、错误屏走保留发布；宿主收尾失败进错误屏并停住上下文；
// 脚本与宿主的重载请求都在帧边界执行；无帧时的语义检查点（事件依次交付、资源完成、邮箱上限）
#include "RenderSupport.h"
#include "TextFixtures.h"

#include <core/Version.h>
#include <d3d11/Renderer.h>
#include <quickjs/QuickJsScriptHost.h>
#include <runtime/CantonRuntime.h>
#include <runtime/Digest.h>
#include <runtime/FramePumpDriver.h>
#include <runtime/SceneHost.h>
#include <runtime/ScenePipelineState.h>
#include <runtime/UiPackage.h>
#include <runtime/VBlankSource.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace canton::core;

namespace {
    void check(bool condition, const std::string& message) {
        if (!condition) throw std::runtime_error(message);
    }

    class FailingTextMeasurer final : public ApproximateTextMeasurer {
       public:
        bool failing = false;

       protected:
        float advance(std::string_view, char32_t, const TextStyle&) const override {
            if (failing) throw std::runtime_error("text measurement unavailable");
            return 8;
        }
    };

    LayoutModifierSemantics fixedSize(float width, float height) {
        LayoutModifierSemantics value;
        value.kind = LayoutModifierKind::Size;
        value.width = width;
        value.height = height;
        return value;
    }

    PropValue policy(const char* kind) {
        return PropValue::objectValue({{"kind", PropValue::stringValue(kind)}});
    }

    void manualVBlank() {
        auto host = std::make_unique<canton::quickjs::QuickJsScriptHost>();
        const auto loaded = host->executeModule("vblank-execution.js", R"JS(
            const n = globalThis.__CANTON_NATIVE__
            n.createNode(1, 'LayoutNode')
            let ticks = 0
            const sample = () => {
                n.setModifier(1, {elements: [{type: 'text', value: {text: String(++ticks)}}]})
                n.requestFrame(true)
            }
            n.installFrameDriver(sample, () => {}, () => {})
            n.requestFrame(true)
        )JS");
        check(loaded.ok, "手动 VBlank 脚本失败：" + loaded.error);
        canton::runtime::CantonRuntime runtime;
        runtime.enqueue(std::move(*host->takePendingTransaction()));
        runtime.setScriptHost(std::move(host));
        canton::runtime::ManualVBlankSource source;
        canton::runtime::VBlankFrameDriver driver(source);
        int frames = 0;
        const auto tick = [&](double timestamp) {
            ++frames;
            // 同一拍里再次触发不会重复采样动画
            source.pulse(timestamp + 0.1);
            check(runtime.pumpFrame(1, {0, 400, 0, 300}, timestamp).ok, "手动 VBlank 帧失败");
        };
        driver.start(tick);
        source.pulse(100);
        check(frames == 1 && test_support::textOf(runtime.scene().node(1)) == "1", "第一拍没有恰好采样一次");
        source.pulse(100);
        source.pulse(99);
        source.pulse(std::numeric_limits<double>::quiet_NaN());
        check(frames == 1, "重复、倒退或无效的时间戳产生了帧");
        source.pulse(116);
        check(frames == 2 && test_support::textOf(runtime.scene().node(1)) == "2", "动画在同一拍里重新排期");
        driver.stop();
        source.pulse(132);
        check(frames == 2, "停掉的 VBlank 源仍在执行");
        driver.start(tick);
        source.pulse(148);
        check(frames == 3 && test_support::textOf(runtime.scene().node(1)) == "3", "VBlank 源没有恢复待执行的动画");
    }

    void failedSubmissionRollback() {
        FailingTextMeasurer measurer;
        TextLayoutService text(measurer);
        canton::runtime::ScenePipelineState state{SceneFramePipeline{LayoutEngine{text}}};
        const Constraints constraints{0, 400, 0, 300};
        MutationTransaction initial;
        initial.operations = {CreateNodeMutation{1, NodeType::Layout}, SetPropMutation{1, "measurePolicy", policy("Column")}};
        state.enqueue(std::move(initial));
        check(!state.run(1, constraints, true).error, "初始发布失败");
        const auto previousFrame = state.publishedFrame();
        const NodeHandle child{2, allocateRuntimeIdentity()};
        const BindingHandle content{allocateRuntimeIdentity(), 1};
        MutationTransaction create;
        create.operations = {CreateNodeMutation{2, NodeType::Layout, child.generation}, SetPropMutation{2, "measurePolicy", policy("MinSize")}, RegisterBinding{content, ModifierChainTarget{child}}, SlotUpdate{content, ModifierDescriptors{{test_support::text("first"), {}}}}, InsertChildMutation{1, 2, 0}};
        state.enqueue(create);
        measurer.failing = true;
        check(state.run(1, constraints, true).error.has_value(), "测量故障没有让帧失败");
        check(!state.scene().contains(2) && state.publishedFrame().revision == previousFrame.revision && state.publishedFrame().content.hitTest == previousFrame.content.hitTest, "失败帧改动了已发布的场景或命中几何");
        check(!state.hasPendingTransactions() && !state.hasPendingIntents(), "失败提交在没有新工作时空转");
        measurer.failing = false;
        auto later = create;
        later.operations.emplace_back(SlotUpdate{content, ModifierDescriptors{{test_support::text("latest"), {}}}});
        state.enqueue(std::move(later));
        check(!state.run(1, constraints, true).error, "重新提交没有恢复");
        check(state.scene().contains(2) && test_support::textOf(state.scene().node(2)) == "latest" && state.scene().bindingCount() == 1, "重新提交完整候选未恢复结构与最终值");
        check(state.scene().slotCounters().rejected == 0, "重新提交拒绝了对新建节点的有效写入");
        check(state.publishedFrame().revision == previousFrame.revision + 1, "失败帧被算成了一次发布");
        measurer.failing = true;
        MutationTransaction failure;
        failure.operations = {SlotUpdate{content, ModifierDescriptors{{test_support::text("unpublished"), {}}}}};
        state.enqueue(std::move(failure));
        check(state.run(1, constraints, true).error.has_value(), "第二次故障没有失败");
        state.reset();
        measurer.failing = false;
        MutationTransaction fresh;
        fresh.operations = {CreateNodeMutation{1, NodeType::Layout}, SetPropMutation{1, "measurePolicy", policy("Box")}};
        state.enqueue(std::move(fresh));
        check(!state.run(1, constraints, true).error && !state.scene().contains(2) && state.scene().bindingCount() == 0, "重置后重放了失败的旧上下文");
    }

    void finalPublication() {
        canton::runtime::ScenePipelineState state;
        const Constraints constraints{0, 400, 0, 300};
        MutationTransaction initial;
        initial.operations = {CreateNodeMutation{1, NodeType::Layout}, SetPropMutation{1, "measurePolicy", policy("Box")}, SetModifierMutation{1, {{fixedSize(100, 100), {}}}}};
        state.enqueue(std::move(initial));
        check(!state.run(1, constraints, true).error, "初始帧失败");
        const auto previous = state.publishedFrame();
        MutationTransaction candidate;
        candidate.operations = {CreateNodeMutation{2, NodeType::Layout}, SetPropMutation{2, "measurePolicy", policy("MinSize")}, InsertChildMutation{1, 2, 0}};
        state.enqueue(candidate);
        const auto failed = state.run(1, constraints, true, [&](const auto& scene, auto& frame) {
            check(scene.contains(2) && !state.scene().contains(2), "收尾拿到的不是隔离的候选场景");
            check(frame.content.hitTest != previous.content.hitTest, "收尾在命中几何构建之前执行");
            frame.content.overlayDrawOps.push_back(DrawOp{});
            throw std::runtime_error("resource preparation failed");
        });
        check(failed.error && !state.scene().contains(2) && state.publishedFrame().revision == previous.revision && state.publishedFrame().content.overlayDrawOps.empty(), "收尾失败发布了部分几何或附件");
        check(state.publishRetained([](const auto&, auto& frame) { frame.content.errorFrame = "failed"; }), "保留发布的错误屏没有发布");
        check(!state.scene().contains(2) && state.publishedFrame().content.hitTest == previous.content.hitTest, "错误屏发布重放了失败的结构");
        state.enqueue(std::move(candidate));
        check(!state
                   .run(1, constraints, true,
                        [](const auto&, auto& frame) {
                            frame.content.errorFrame.reset();
                            DrawOp transform;
                            transform.type = DrawOpType::PushTransform;
                            transform.translationX = 12;
                            frame.content.overlayDrawOps = {transform};
                        })
                   .error,
              "收尾后的重试失败");
        check(state.scene().contains(2) && !state.publishedFrame().content.errorFrame && state.publishedFrame().revision == previous.revision + 2, "场景与附件没有在同一次发布里");
        const auto measures = state.counters().measures;
        check(state.publishRetained([](const auto&, auto& frame) { frame.content.overlayDrawOps[0].translationX = 24; }), "只改变换的叠层变化被忽略");
        check(state.counters().measures == measures && state.publishedFrame().plan.passivePaint, "叠层变化跑了布局或漏了重绘");
        check(!state.publishRetained([](const auto&, auto&) {}), "附件不变却产生了发布");
        check(state.counters().publications == state.publishedFrame().revision, "发布计数与版本号对不上");
    }

    // 宿主的帧泵：收尾失败进错误屏，只发布一次，停住当前上下文；重置后恢复
    void hostFinalizeFailure() {
        FailingTextMeasurer measurer;
        TextLayoutService text(measurer);
        canton::runtime::CantonRuntime runtime{SceneFramePipeline{LayoutEngine{text}}};
        canton::runtime::RuntimeSessionState session;
        canton::runtime::DiagnosticsState diagnostics;
        canton::runtime::FramePumpDriver driver;
        session.resize(400, 300, runtime);
        session.markLoaded();
        MutationTransaction initial;
        initial.operations = {CreateNodeMutation{1, NodeType::Layout}, SetPropMutation{1, "measurePolicy", policy("Box")}};
        runtime.enqueue(std::move(initial));
        bool failResources = false;
        const auto pump = [&](double now) {
            return driver.pumpFrame(runtime, session, diagnostics, 1, {}, {0, 0, 400, 300}, true, canton::runtime::PackageSource::None, false, text, now, [&](const auto&, auto&) {
                if (failResources) throw std::runtime_error("resource preparation failed");
            });
        };
        check(pump(0) && !diagnostics.hasError(), "宿主初始帧失败");
        const auto previous = runtime.publishedFrame();
        failResources = true;
        MutationTransaction added;
        added.operations = {CreateNodeMutation{2, NodeType::Layout}, SetPropMutation{2, "measurePolicy", policy("MinSize")}, InsertChildMutation{1, 2, 0}, SetModifierMutation{2, {{fixedSize(30, 30), {}}}}};
        runtime.enqueue(std::move(added));
        check(pump(16) && diagnostics.hasError(), "收尾失败没有进宿主诊断");
        check(!runtime.scene().contains(2) && runtime.publishedFrame().content.hitTest == previous.content.hitTest && runtime.publishedFrame().content.errorFrame && runtime.publishedFrame().revision == previous.revision + 1, "收尾失败发布了候选几何或发布了多次");
        check(!runtime.hasPendingFrameWork() && !session.loaded(), "失败的上下文仍在产生视觉工作");
        const auto counters = runtime.frameCounters();
        check(!pump(24) && runtime.frameCounters().measures == counters.measures, "停住的上下文又跑了流水线");
        failResources = false;
        session.reset(runtime, diagnostics);
        session.markLoaded();
        MutationTransaction reload;
        reload.operations = {CreateNodeMutation{1, NodeType::Layout}, SetPropMutation{1, "measurePolicy", policy("Box")}};
        runtime.enqueue(std::move(reload));
        check(pump(32) && !diagnostics.hasError() && !runtime.publishedFrame().content.errorFrame, "重置没有恢复停住的宿主");
        // 加载后场景为空也走同一个错误边界，不跑场景流水线
        session.reset(runtime, diagnostics);
        session.markLoaded();
        const auto idleMeasures = runtime.frameCounters().measures;
        check(pump(48) && diagnostics.hasError() && !session.loaded(), "空闲收尾失败越过了宿主的错误边界");
        check(runtime.frameCounters().measures == idleMeasures && runtime.publishedFrame().content.errorFrame, "空闲失败跑了场景流水线或丢了诊断发布");
    }

    // 脚本与宿主请求的重载在帧边界经包加载执行
    void reloadAtFrameBoundary(canton::d3d11::Renderer& renderer) {
        const auto directory = std::filesystem::temp_directory_path() / ("canton-reload-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);

        struct Cleanup {
            std::filesystem::path path;

            ~Cleanup() {
                std::error_code error;
                std::filesystem::remove_all(path, error);
            }
        } cleanup{directory};

        const auto writePackage = [&](const char* color, const char* request) {
            const auto source = std::string("const n = globalThis.__CANTON_NATIVE__;\n") + "void (n.createNode(1, 'LayoutNode'), n.updateBinding(n.registerBinding(1, 'measurePolicy'), {kind: 'Box'}));\n" + "n.setModifier(1, {elements: [{type: 'size', value: {width: 100, height: 100}}, {type: 'background', value: {color: " + color + "}}]});\n" + request;
            std::ofstream(directory / "app.js", std::ios::binary) << source;
            const auto digest = canton::runtime::sha256Hex({reinterpret_cast<const std::uint8_t*>(source.data()), source.size()});
            std::ofstream(directory / std::string(canton::runtime::uiManifestName), std::ios::binary) << "{\"format\":3,\"framework\":\"" << canton::core::PackageVersion << "\",\"protocol\":" << canton::core::RuntimeVersion << ",\"scriptFormat\":\"js\",\"entry\":\"app.js\",\"entries\":[{\"path\":\"app.js\",\"kind\":\"module\",\"size\":" << source.size() << ",\"sha256\":\"" << digest << "\"}]}";
        };
        canton::core::ApproximateTextMeasurer measurer;
        canton::runtime::SceneHost host(measurer, std::make_shared<FontRegistry>(), {});
        host.resize(100, 100);
        const auto pixel = [&] {
            auto target = renderer.createOffscreenTarget(100, 100);
            renderer.render(*target, {&host.publishedFrame().content});
            return renderer.readPixels(*target)[50 * 100 + 50];
        };
        double timestamp = 0;
        const auto pulse = [&] {
            (void)host.pumpFrame(timestamp += 16);
        };
        writePackage("0xffff0000", "Promise.resolve().then(() => n.reload({path: 'app.js'}));");
        canton::runtime::AppSourceConfig source;
        source.policy = canton::runtime::LoadPolicy::OnlyDist;
        source.distOverride = directory;
        host.configure(std::move(source));
        pulse();
        check(pixel() == 0xffff0000u, "初次加载没有发布");
        check(host.wantsFrame(), "脚本在模块求值后请求的重载没有留住帧时钟");
        writePackage("0xff0000ff", "");
        pulse();
        check(pixel() == 0xff0000ffu && !host.wantsFrame(), "脚本请求的重载没有加载或没有停稳");
        writePackage("0xff00ff00", "");
        host.requestReload();
        check(pixel() == 0xff0000ffu && host.wantsFrame(), "宿主请求的重载在帧边界之前替换了已发布内容");
        pulse();
        check(pixel() == 0xff00ff00u, "宿主请求的重载没有加载新包");
        writePackage("0xffff0000", "Promise.resolve().then(() => n.diagnosticsRequestReload({}));");
        host.requestReload();
        pulse();
        check(pixel() == 0xffff0000u, "重载没有走包加载边界");
        writePackage("0xff0000ff", "");
        pulse();
        pulse();
        check(pixel() == 0xff0000ffu && !host.wantsFrame(), "诊断请求的重载没有共用同一个重载入口");
    }

    // 没有帧时也要推进语义：同帧前的事件依次交付，资源完成经 Owner 唤醒，发布版本变化不丢有效事件，邮箱有上限
    void semanticOwnerWithoutFrames() {
        auto request = std::make_shared<std::promise<PainterLoadResult>>();
        std::function<void()> wake;
        auto script = std::make_unique<canton::quickjs::QuickJsScriptHost>();
        script->setPainterLoader([&](const std::string&, std::function<void()> notify) {
            wake = std::move(notify);
            return request->get_future();
        });
        const auto loaded = script->executeModule("语义检查点.js", R"JS(
const n = globalThis.__CANTON_NATIVE__
let count = 0
n.createNode(1, 'LayoutNode')
n.setModifier(1, { elements: [{ type: 'clickable', value: { onClick() {
    n.diagnosticsToast('i', '语义验收', '输入', [String(++count)], false)
    if (count === 3) {
        const handle = n.acquirePainter('资源', () => {
            n.diagnosticsToast('i', '语义验收', '资源完成', [], false)
            n.releasePainter(handle)
        })
    }
} } }] })
)JS");
        check(loaded.ok, "语义测试脚本加载失败：" + loaded.error);
        auto transaction = script->takePendingTransaction();
        canton::runtime::CantonRuntime runtime;
        runtime.setScriptHost(std::move(script));
        runtime.enqueue(std::move(*transaction));
        check(runtime.pumpFrame(1, {0, 200, 0, 100}, 0).ok, "语义测试初始帧失败");
        const auto revision = runtime.publishedFrame().revision;
        const auto slot = *runtime.scene().activeEventSlots().begin();
        for (int i = 0; i < 3; ++i) runtime.enqueueEvent(slot);
        const auto semantic = runtime.semanticCheckpoint(16);
        check(semantic.ok, "输入语义检查点失败：" + semantic.error);
        const auto inputs = runtime.takeDiagnosticToasts();
        check(inputs.size() == 3 && inputs[0].content == "1" && inputs[1].content == "2" && inputs[2].content == "3", "同帧前输入没有依次完成语义操作");
        check(runtime.publishedFrame().revision == revision, "语义任务自行发布了视觉帧");
        request->set_value({std::make_shared<PainterContent>(), {}});
        check(static_cast<bool>(wake), "资源请求未接入 Owner 唤醒");
        wake();
        check(runtime.semanticCheckpoint(20).ok, "资源完成的语义检查点失败");
        const auto completions = runtime.takeDiagnosticToasts();
        check(completions.size() == 1 && completions[0].title == "资源完成", "无帧时资源完成没有经过 Owner 语义唤醒");
        check(runtime.publishedFrame().revision == revision, "资源语义完成绕过了视觉授权");
        // 发布版本改变不等于输入身份退休，已排队的有效回调仍须交付
        runtime.enqueueEvent(slot);
        check(runtime.hasPendingEvents(), "有效事件没有进入 Owner 邮箱");
        check(runtime.publishRetained([](const auto&, auto& frame) { frame.content.overlayDrawOps.push_back(DrawOp{}); }), "保留场景的发布版本未推进");
        check(runtime.publishedFrame().revision == revision + 1 && runtime.scene().hasEventSlot(slot), "版本变化验收误换了事件身份");
        const auto afterPublication = runtime.semanticCheckpoint(24);
        check(afterPublication.ok, "跨发布版本事件失败：" + afterPublication.error);
        const auto retainedEvents = runtime.takeDiagnosticToasts();
        check(retainedEvents.size() == 1 && retainedEvents[0].content == "4", "单纯发布版本变化误删了有效离散事件");
        check(!runtime.hasPendingEvents() && runtime.publishedFrame().revision == revision + 1, "跨版本事件没有完成语义交付或自行发布视觉帧");
        for (int i = 0; i < 4097; ++i) runtime.enqueueEvent(slot);
        const auto overload = runtime.semanticCheckpoint(32);
        check(!overload.ok && overload.error.find("4096") != std::string::npos, "邮箱过载未明确报错");
        runtime.reset();
        runtime.enqueueEvent(slot);
        check(runtime.semanticCheckpoint(48).ok, "旧代际事件命中了新上下文");
    }
}

int main() {
    const char* step = "手动 VBlank";
    try {
        const test_support::ComApartment apartment;
        canton::d3d11::Renderer renderer({canton::d3d11::AdapterPreference::Warp});
        manualVBlank();
        step = "失败提交回滚";
        failedSubmissionRollback();
        step = "收尾与发布";
        finalPublication();
        step = "宿主收尾失败";
        hostFinalizeFailure();
        step = "帧边界重载";
        reloadAtFrameBoundary(renderer);
        step = "无帧语义";
        semanticOwnerWithoutFrames();
        std::cout << "VBlank 节拍、失败提交回滚、收尾发布、宿主错误边界、帧边界重载与无帧语义通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << step << "：" << error.what() << '\n';
        return 1;
    }
}
