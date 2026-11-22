// 重排运行时：tests/fixtures/rearrange-runtime 的 .cta 经 UI 包、QuickJS 到 core。纯值更新只走对应阶段、不重跑结构作用域、
// 不泄漏绑定；带 key 的重排复用节点；分支移除与恢复回收资源；异步加载的帧需求随完成、超时与卸载释放；
// 资源失败带着来源进错误屏；稳态改值的帧耗时基准
#include "FixtureRuntime.h"

#include <runtime/DiagnosticsState.h>
#include <runtime/FramePumpDriver.h>
#include <runtime/RuntimeSessionState.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <numeric>
#include <thread>

using namespace canton::core;

namespace {
    void check(bool condition, const std::string& message) {
        if (!condition) throw std::runtime_error(message);
    }

    std::vector<ModifierHandle> handles(const LayoutNode& node) {
        std::vector<ModifierHandle> result;
        for (const auto& instance : node.modifier.elements()) result.push_back(instance.handle);
        return result;
    }

    void run(const std::filesystem::path& directory) {
        test_support::FixtureRuntime fixture(directory, {0, 520, 0, 1200});
        auto& runtime = fixture.runtime();
        fixture.tick();
        const auto counter = fixture.findText("点了 0 次");
        check(counter != 0, "夹具首帧没有计数文字");
        const auto originalHandles = handles(fixture.scene().node(counter));
        // 一条指令一帧；指令产生的后续提交再补一帧
        const auto command = [&](const char* value) {
            fixture.step = value;
            runtime.enqueueStringEvent(fixture.submit(), value);
            fixture.tick();
            if (runtime.hasPendingTransactions() || runtime.hasPendingIntents()) {
                check(runtime.hasPendingFrameWork(), "等待旧提交期间产生的新命令没有保留帧需求");
                fixture.tick();
            }
        };
        command("baseline");
        const auto baseline = runtime.frameCounters();
        const auto hit = runtime.publishedFrame().content.hitTest;
        command("color");
        auto stats = runtime.frameCounters();
        check(stats.measures == baseline.measures && stats.placements == baseline.placements, "改色重新测量或放置了");
        check(stats.paintBuilds == baseline.paintBuilds + 1 && stats.hitBuilds == baseline.hitBuilds, "改色没有只重建绘制");
        check(runtime.publishedFrame().content.hitTest == hit, "改色替换了命中快照");
        check(handles(fixture.scene().node(counter)) == originalHandles, "改色替换了 Modifier 实例");
        command("validate");
        command("offset");
        stats = runtime.frameCounters();
        check(stats.measures == baseline.measures && stats.placements == baseline.placements + 1, "偏移没有复用测量");
        check(runtime.publishedFrame().content.hitTest != hit, "偏移没有重建命中几何");
        command("validate");
        command("text");
        check(runtime.frameCounters().measures == baseline.measures + 1, "文字变化跳过了测量");
        check(fixture.findText("点了 1 次") == counter, "文字变化替换了宿主节点");
        command("validate");
        // 值槽的原生绑定在第一次动态更新时才登记：改色、偏移、文字各登记一次，之后的重复更新不再增加
        const auto bindings = fixture.scene().bindingCount();
        const auto callbacks = fixture.scene().eventSlotCount();
        for (int i = 0; i < 20; ++i) {
            command("color");
            command("offset");
            command("validate");
        }
        check(runtime.frameCounters().measures == baseline.measures + 1, "重复的值更新累积了测量");
        check(fixture.scene().bindingCount() == bindings && fixture.scene().eventSlotCount() == callbacks, "重复的值更新泄漏了原生资源：绑定 " + std::to_string(bindings) + " → " + std::to_string(fixture.scene().bindingCount()) + "，回调 " + std::to_string(callbacks) + " → " + std::to_string(fixture.scene().eventSlotCount()));
        const auto first = fixture.findText("A"), second = fixture.findText("B");
        command("reorder");
        check(fixture.findText("A") == first && fixture.findText("B") == second, "带 key 的重排替换了宿主节点");
        const auto& children = fixture.scene().node(fixture.scene().node(1).children.front()).children;
        check(std::find(children.begin(), children.end(), second) < std::find(children.begin(), children.end(), first), "片段锚点影响了原生次序");
        // 恢复的分支是新实例，值槽还没动态更新过，绑定比移除前少；反复移除与恢复要回到同一个数
        std::size_t restoredBindings = 0;
        for (int i = 0; i < 3; ++i) {
            command("remove");
            check(fixture.scene().bindingCount() < bindings && fixture.scene().eventSlotCount() < callbacks, "分支移除保留了原生资源");
            command("restore");
            if (i == 0) restoredBindings = fixture.scene().bindingCount();
            check(fixture.scene().bindingCount() == restoredBindings && restoredBindings <= bindings && fixture.scene().eventSlotCount() == callbacks, "分支恢复后的绑定或回调数量不对：绑定 " + std::to_string(fixture.scene().bindingCount()) + "，回调 " + std::to_string(fixture.scene().eventSlotCount()));
        }
        command("baseline");
        command("color");
        command("validate");

        // 异步加载：等待期间请求帧，完成、超时或卸载后都释放帧需求
        const auto settledBindings = fixture.scene().bindingCount();
        command("lifetime-baseline");
        command("async-start");
        check(runtime.hasPendingVisualWork(), "异步加载的延迟没有请求宿主帧");
        command("baseline");
        command("baseline");
        command("async-finish");
        fixture.settle();
        check(fixture.findText("延迟加载完成") != 0 && !runtime.hasPendingVisualWork(), "加载完成后保留了延迟帧需求");
        command("async-remove");
        command("async-start");
        for (int index = 0; index < 7; ++index) command("baseline");
        fixture.settle();
        check(fixture.findText("延迟加载超时") != 0 && !runtime.hasPendingVisualWork(), "加载超时后保留了延迟帧需求");
        command("async-remove");
        command("async-start");
        command("async-remove");
        command("async-finish");
        fixture.settle();
        check(fixture.scene().bindingCount() == settledBindings && !runtime.hasPendingVisualWork(), "卸载后的迟到加载重新挂载或保留了帧需求");
        command("lifetime-check");

        // 帧耗时基准：稳态下交替改色与偏移，每帧一条指令
        command("perf-baseline");
        std::vector<double> durations;
        const auto workBefore = runtime.frameCounters();
        const auto layoutsBefore = fixture.text().counters().layoutsCreated;
        for (int frame = 0; frame < 240; ++frame) {
            runtime.enqueueStringEvent(fixture.submit(), frame % 2 ? "offset" : "color");
            const auto started = std::chrono::steady_clock::now();
            fixture.tick();
            durations.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        }
        const auto workAfter = runtime.frameCounters();
        check(workAfter.measures == workBefore.measures && fixture.text().counters().layoutsCreated == layoutsBefore, "稳态改值跑了测量或排版");
        command("perf-report");
        std::sort(durations.begin(), durations.end());
        const auto mean = std::accumulate(durations.begin(), durations.end(), 0.0) / static_cast<double>(durations.size());
        std::cout << "CANTON_FRAME_PERF {\"frames\":" << durations.size() << ",\"mean_ms\":" << mean << ",\"p95_ms\":" << durations[durations.size() * 95 / 100] << ",\"max_ms\":" << durations.back() << ",\"place\":" << workAfter.placements - workBefore.placements << ",\"paint\":" << workAfter.paintBuilds - workBefore.paintBuilds << ",\"hit\":" << workAfter.hitBuilds - workBefore.hitBuilds << ",\"copied_nodes\":" << workAfter.candidateNodesCopied - workBefore.candidateNodesCopied << "}\n";

        // 资源失败：经宿主帧泵进错误屏，诊断里带着资源名
        canton::runtime::RuntimeSessionState session;
        canton::runtime::DiagnosticsState diagnostics;
        canton::runtime::FramePumpDriver driver;
        session.resize(520, 1200, runtime);
        session.markLoaded();
        runtime.enqueueStringEvent(fixture.submit(), "bad-resource");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!diagnostics.hasError() && std::chrono::steady_clock::now() < deadline) {
            (void)driver.pumpFrame(runtime, session, diagnostics, 1, {}, {0, 0, 520, 1200}, true, canton::runtime::PackageSource::None, false, fixture.text(), fixture.advance(16));
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(diagnostics.hasError() && diagnostics.error()->summary.find("missing-image.png") != std::string::npos, "资源加载失败没有进错误屏或丢了资源名：" + (diagnostics.hasError() ? diagnostics.error()->summary : std::string("没有错误")));
        check(runtime.publishedFrame().content.errorFrame.has_value(), "资源错误没有经过正式发布边界");
    }
}

int main(int argc, char** argv) {
    try {
        check(argc == 2, "需要构建好的 rearrange-runtime 夹具目录");
        run(argv[1]);
        std::cout << "重排运行时：值阶段、key 复用、分支回收、异步帧需求、资源失败与帧耗时基准通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
