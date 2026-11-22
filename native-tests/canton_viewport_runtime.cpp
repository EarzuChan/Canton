// 视口：tests/fixtures/viewport-runtime 经 UI 包、QuickJS 到 core。视口变化同帧合并、立即限制滚动偏移、
// 滚动范围按 PX 回传脚本、内容尺寸变化与越界滚动经布局反馈修正、脚本侧密度换算后的尺寸与命中一致；
// 再经 SceneHost 与 D3D11 渲染器：放大后新区域有内容、空闲后不再要帧、帧率徽标开关独立绘制。
// 宿主把缩放比交给脚本的通道和 IME 几何属 M4
#include "FixtureRuntime.h"
#include "RenderSupport.h"

#include <core/HitTest.h>
#include <core/ModifierGeometry.h>
#include <core/Scroll.h>
#include <d3d11/Renderer.h>
#include <runtime/RuntimeSessionState.h>
#include <runtime/SceneHost.h>

#include <iostream>

using namespace canton::core;

namespace {
    void check(bool condition, const std::string& message) {
        if (!condition) throw std::runtime_error(message);
    }

    void runtimeViewport(const std::filesystem::path& directory) {
        test_support::FixtureRuntime fixture(directory, {0, 200, 0, 100});
        auto& runtime = fixture.runtime();
        canton::runtime::RuntimeSessionState session;
        session.resize(200, 100, runtime);
        fixture.setConstraints(session.constraints());
        fixture.tick();
        fixture.settle(12);
        check(fixture.findText("450/500/100/600") != 0, "首次布局没有经真实 ScrollState 更新模板");
        NodeId scrollNode = 0;
        for (const auto id : fixture.scene().tree().nodeIds())
            if (ScrollDispatcher::hasVerticalScroll(fixture.scene().node(id))) scrollNode = id;
        check(scrollNode != 0, "夹具缺少滚动实例");
        const auto original = fixture.scene().node(scrollNode).modifier.elements().back().handle;
        const auto measureBefore = runtime.frameCounters().measures;
        const auto resize = [&](int width, int height) {
            session.resize(width, height, runtime);
            fixture.setConstraints(session.constraints());
        };
        resize(200, 100);
        check(!runtime.hasPendingFrameWork(), "相同视口重复制造了帧需求");
        resize(210, 150);
        resize(220, 180);
        resize(240, 200);
        check(runtime.hasPendingFrameWork(), "空闲后的视口变化没有唤醒帧");
        fixture.tick();
        check(runtime.frameCounters().measures == measureBefore + 1, "同帧视口变化没有按最新尺寸合并布局");
        check(ScrollDispatcher::verticalScrollValue(fixture.scene().node(scrollNode)) == 400, "视口放大后的第一帧没有立即限制滚动偏移");
        fixture.settle(12);
        check(fixture.findText("400/400/200/600") != 0, "视口放大没有回传范围和修正值");
        check(fixture.scene().node(scrollNode).modifier.elements().back().handle == original, "视口变化替换了滚动实例");
        resize(300, 700);
        fixture.tick();
        check(ScrollDispatcher::verticalScrollValue(fixture.scene().node(scrollNode)) == 0, "内容能完全显示时没有立即回到合法位置");
        fixture.settle(12);
        check(fixture.findText("0/0/700/700") != 0, "扩展后的视口范围没有回传");
        resize(200, 100);
        fixture.settle(12);
        fixture.command("缩短");
        check(fixture.findText("0/0/100/100") != 0, "内容尺寸变化没有更新滚动范围");
        fixture.command("越界");
        check(fixture.findText("0/0/100/100") != 0, "代码设置的越界滚动没有经统一的布局反馈修正");
        fixture.command("密度");
        check(fixture.findText("0/500/100/600") != 0, "DP 换算后的滚动范围没有以 PX 回传");
        NodeId inputNode = 0;
        for (const auto id : fixture.scene().tree().nodeIds())
            if (test_support::editable(fixture.scene().node(id))) inputNode = id;
        const auto& node = fixture.scene().node(inputNode);
        const auto& descriptor = std::get<TextFieldModifier>(test_support::editable(node)->descriptor.value);
        check(node.bounds.width == 160 && node.bounds.height == 60 && descriptor.presentation.style.fontSize == 30, "DP 与 SP 没有各自换算到原生 PX");
        const auto point = nodeContentToRoot(fixture.scene().tree(), inputNode, {node.contentBounds.x + 1, node.contentBounds.y + 1});
        const auto hit = HitTester{}.hitTest(*runtime.publishedFrame().content.hitTest, point);
        check(hit.hit && hit.node == inputNode, "非恒等密度下的输入命中坐标不一致");
    }

    void hostViewport(const std::filesystem::path& directory) {
        const test_support::ComApartment apartment;
        canton::d3d11::Renderer renderer({canton::d3d11::AdapterPreference::Warp});
        canton::d3d11::DWriteTextMeasurer measurer;
        canton::runtime::SceneHostOptions options;
        options.diagnostics.toasts = canton::runtime::DiagnosticVisibility::Hidden;
        options.diagnostics.badge = canton::runtime::DiagnosticVisibility::Hidden;
        canton::runtime::SceneHost host(measurer, std::make_shared<FontRegistry>(), options);
        host.resize(200, 100);
        canton::runtime::AppSourceConfig source;
        source.policy = canton::runtime::LoadPolicy::OnlyDist;
        source.distOverride = directory;
        host.configure(std::move(source));
        double time = 0;
        const auto settle = [&] {
            for (int count = 0; count < 24 && host.wantsFrame(); ++count) (void)host.pumpFrame(time += 16);
            check(!host.wantsFrame(), "宿主空闲后仍持续要帧");
        };
        settle();
        check(host.ready(), "宿主没有出首帧");
        const auto snapshot = [&](int width, int height) {
            auto target = renderer.createOffscreenTarget(width, height);
            renderer.render(*target, {&host.publishedFrame().content});
            return renderer.readPixels(*target);
        };
        host.resize(300, 200);
        check(host.wantsFrame() && host.pumpFrame(time += 16), "宿主尺寸变化没有发布新画面");
        settle();
        const auto image = snapshot(300, 200);
        check(image[190 * 300 + 290] == 0xff336699u, "放大后的新区域没有画内容");
        const auto appPixel = image[30 * 300 + 12];
        host.setFpsBadgeEnabled(true);
        check(host.wantsFrame() && host.pumpFrame(time += 16), "开启帧率徽标没有发布诊断帧");
        check(!host.publishedFrame().content.diagnosticsFpsDrawOps.empty(), "帧率徽标没有绘制指令");
        host.setFpsBadgeEnabled(false);
        (void)host.pumpFrame(time += 16);
        settle();
        check(host.publishedFrame().content.diagnosticsFpsDrawOps.empty() && snapshot(300, 200)[30 * 300 + 12] == appPixel, "关闭帧率徽标后仍有残留");
    }
}

int main(int argc, char** argv) {
    const char* step = "运行时视口";
    try {
        check(argc == 2, "需要构建好的 viewport-runtime 夹具目录");
        runtimeViewport(argv[1]);
        step = "宿主视口";
        hostViewport(argv[1]);
        std::cout << "视口、内容尺寸、滚动范围回传、密度换算与宿主绘制链路通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << step << "：" << error.what() << '\n';
        return 1;
    }
}
