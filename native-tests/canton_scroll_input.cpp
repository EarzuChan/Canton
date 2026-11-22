// 滚轮与平滑滚动全链路原生测试：
// 1. 滚轮主轴筛选、单轴/双轴嵌套路由与同轴到达边界自动接力（boundary handoff）；
// 2. 精确像素（触控板 Pixels）与归一化滚轮（鼠标 Lines / Pages）；
// 3. 300ms 滚动会话目标锁定（防滑到边界误串外层）与超时/场景失效重选；
// 4. WheelSmoother 140ms 平滑曲线推进、反向重置与精确像素直通；
// 5. QuickJS 真实脚本 onScroll 回传与重排帧发布贯通。
#include "TextFixtures.h"

#include <core/HitTest.h>
#include <core/InputSession.h>
#include <core/Layout.h>
#include <core/SceneFramePipeline.h>
#include <core/Scroll.h>
#include <core/WheelSmoother.h>
#include <quickjs/QuickJsScriptHost.h>
#include <runtime/CantonRuntime.h>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace canton::core;

namespace {
    void check(bool cond, const std::string& msg) {
        if (!cond) throw std::runtime_error(msg);
    }

    bool near(float a, float b, float eps = 0.05f) {
        return std::fabs(a - b) <= eps;
    }

    PropValue policy(const char* kind) {
        return PropValue::objectValue({{"kind", PropValue::stringValue(kind)}});
    }

    ModifierDescriptors boxWithSize(float w, float h) {
        LayoutModifierSemantics s;
        s.kind = LayoutModifierKind::Size;
        s.width = w;
        s.height = h;
        return {{s}};
    }

    ModifierDescriptors scrollContainer(float w, float h, LayoutModifierKind kind, float value, EventSlotId slot) {
        auto chain = boxWithSize(w, h);
        LayoutModifierSemantics sc;
        sc.kind = kind;
        sc.scrollValue = value;
        sc.eventSlot = std::move(slot);
        chain.push_back({sc});
        return chain;
    }

    // 1 & 2 & 3. 主轴选择、双轴路由、同轴边界接力、Pixels/Lines/Pages 单位与 300ms 会话目标锁定
    void verifyRoutingHandoffAndSessionLock() {
        LayoutTree tree;
        const auto outerVSlot = makeEventSlotId(1, EventSlotKind::VerticalScroll);
        const auto innerHSlot = makeEventSlotId(2, EventSlotKind::HorizontalScroll);
        const auto innerVSlot = makeEventSlotId(3, EventSlotKind::VerticalScroll);

        // 1: 外层纵向可滚 (200x100，内容高 300 -> max 200)
        // 2: 中层横向可滚 (200x100，内容宽 400 -> max 200)
        // 3: 内层纵向可滚 (200x100，内容高 150 -> max 50)
        tree.apply({
            CreateNodeMutation{1, NodeType::Layout},
            SetPropMutation{1, "measurePolicy", policy("Column")},
            SetModifierMutation{1, scrollContainer(200, 100, LayoutModifierKind::VerticalScroll, 0, outerVSlot)},

            CreateNodeMutation{2, NodeType::Layout},
            SetPropMutation{2, "measurePolicy", policy("Row")},
            SetModifierMutation{2, scrollContainer(200, 100, LayoutModifierKind::HorizontalScroll, 0, innerHSlot)},
            InsertChildMutation{1, 2, 0},

            CreateNodeMutation{3, NodeType::Layout},
            SetPropMutation{3, "measurePolicy", policy("Column")},
            SetModifierMutation{3, scrollContainer(200, 100, LayoutModifierKind::VerticalScroll, 0, innerVSlot)},
            InsertChildMutation{2, 3, 0},

            CreateNodeMutation{4, NodeType::Layout},
            SetPropMutation{4, "measurePolicy", policy("Box")},
            SetModifierMutation{4, boxWithSize(200, 150)},
            InsertChildMutation{3, 4, 0},

            CreateNodeMutation{5, NodeType::Layout},
            SetPropMutation{5, "measurePolicy", policy("Box")},
            SetModifierMutation{5, boxWithSize(200, 100)},
            InsertChildMutation{2, 5, 1},

            CreateNodeMutation{6, NodeType::Layout},
            SetPropMutation{6, "measurePolicy", policy("Box")},
            SetModifierMutation{6, boxWithSize(200, 200)},
            InsertChildMutation{1, 6, 1},
        });
        LayoutEngine engine;
        engine.layout(tree, 1, {0, 200, 0, 100});
        const auto hit = buildHitTestSnapshot(tree, 1);

        InputSession session;
        // 斜向触控板输入（|deltaX|=40 > |deltaY|=5）：主轴选为横向，精确像素直达中层横向容器 2
        WheelInput touchH;
        touchH.deltaX = -40.0f;
        touchH.deltaY = -5.0f;
        touchH.unitX = WheelUnit::LogicalPixels;
        touchH.unitY = WheelUnit::LogicalPixels;
        touchH.precise = true;
        touchH.timeMillis = 100.0;
        const auto rH = session.wheel(tree, 1, {50, 50}, touchH, 1, &hit);
        check(rH.scroll.consumed && rH.scroll.target == 2 && near(rH.scroll.value, 40.0f), "横向主轴精确像素未命中中层横向滚动容器");

        // 等待超过 300ms 释放横向锁定，纵向鼠标滚轮（2 行 = 32px，scrollLineDistance = 16）命中内层容器 3
        WheelInput wheelDown;
        wheelDown.deltaY = -2.0f;
        wheelDown.unitY = WheelUnit::Lines;
        wheelDown.timeMillis = 500.0;
        const auto rInner1 = session.wheel(tree, 1, {50, 50}, wheelDown, 1, &hit);
        check(rInner1.scroll.consumed && rInner1.scroll.target == 3 && near(rInner1.scroll.value, 32.0f), "纵向滚轮未命中内层纵向容器");
        tree.apply({SetModifierMutation{3, scrollContainer(200, 100, LayoutModifierKind::VerticalScroll, rInner1.scroll.value, innerVSlot)}});
        engine.layout(tree, 1, {0, 200, 0, 100});

        // 100ms 内继续向下滚：内层到达 50px 底部边界并被会话锁定，不会立刻串到外层容器 1
        wheelDown.timeMillis = 580.0;
        const auto rInnerBottom = session.wheel(tree, 1, {50, 50}, wheelDown, 2, nullptr);
        check(rInnerBottom.scroll.consumed && rInnerBottom.scroll.target == 3 && near(rInnerBottom.scroll.value, 50.0f), "内层滚动未停在 50px 底部边界");
        tree.apply({SetModifierMutation{3, scrollContainer(200, 100, LayoutModifierKind::VerticalScroll, 50.0f, innerVSlot)}});
        engine.layout(tree, 1, {0, 200, 0, 100});

        wheelDown.timeMillis = 620.0;
        const auto rLockedAtEdge = session.wheel(tree, 1, {50, 50}, wheelDown, 3, nullptr);
        check(!rLockedAtEdge.scroll.consumed, "300ms 会话锁定期内内层触底不应立刻串滚到外层父容器");

        // 静默超过 300ms 后再次向下滚：内层已在底部无可滚空间，自动同轴接力（boundary handoff）驱动外层容器 1！
        WheelInput pageDown;
        pageDown.deltaY = -1.0f;
        pageDown.unitY = WheelUnit::Pages;  // 1 页 = 100px 视口高
        pageDown.timeMillis = 1000.0;
        const auto rOuterHandoff = session.wheel(tree, 1, {50, 50}, pageDown, 3, nullptr);
        check(rOuterHandoff.scroll.consumed && rOuterHandoff.scroll.target == 1 && near(rOuterHandoff.scroll.value, 100.0f), "同轴边界接力或 Pages 翻页单位未驱动外层容器");
    }

    // 4. WheelSmoother 平滑曲线与精确像素直通
    void verifyWheelSmoother() {
        WheelSmoother smoother;
        WheelInput precise;
        precise.deltaY = -24.0f;
        precise.unitY = WheelUnit::LogicalPixels;
        precise.precise = true;
        check(!smoother.push({10, 10}, precise, 0.0) && !smoother.active(), "精确像素触控板不应进入离散滚轮平滑器");

        WheelInput notch;
        notch.deltaY = -3.0f;
        notch.unitY = WheelUnit::Lines;
        check(smoother.push({10, 10}, notch, 100.0) && smoother.active(), "离散鼠标滚轮应进入平滑器");

        float total = 0.0f;
        int steps = 0;
        for (double t = 116.0; t <= 300.0; t += 16.0) {
            while (const auto s = smoother.tick(t)) {
                total += s->second.deltaY;
                ++steps;
            }
        }
        check(steps >= 3 && near(total, -3.0f, 0.01f) && !smoother.active(), "WheelSmoother 未按多帧平滑交付完整位移并在结束后停表");
    }

    // 5. 真实 QuickJS 脚本回调与帧发布贯通
    void verifyScriptScrollEndToEnd() {
        auto host = std::make_unique<canton::quickjs::QuickJsScriptHost>();
        const auto loaded = host->executeModule("scroll-e2e.js", R"JS(
            const n = globalThis.__CANTON_NATIVE__
            n.createNode(1, 'LayoutNode')
            n.updateBinding(n.registerBinding(1, 'measurePolicy'), { kind: 'Column' })
            const chain = n.registerBinding(1, 'modifier')
            n.createNode(2, 'LayoutNode')
            n.insertChild(1, 2, 0)
            const label = n.registerBinding(2, 'modifier')
            function sync(pos, max, vp, cs) {
                n.updateBinding(chain, { elements: [{ type: 'size', value: { width: 120, height: 60 } }, { type: 'verticalScroll', value: { value: pos, onScroll: s => sync(s.value, s.maxValue, s.viewportSize, s.contentSize) } }] })
                n.updateBinding(label, { elements: [{ type: 'size', value: { width: 120, height: 180 } }, { type: 'text', value: { text: `${pos}/${max}/${vp}/${cs}` } }] })
                n.requestFrame(true)
            }
            n.installFrameDriver(() => {}, () => {}, () => {})
            sync(0, 0, 0, 0)
        )JS");
        check(loaded.ok, loaded.error);
        canton::runtime::CantonRuntime runtime;
        auto initial = host->takePendingTransaction();
        runtime.setScriptHost(std::move(host));
        runtime.enqueue(std::move(*initial));
        check(runtime.pumpFrame(1, {0, 120, 0, 60}, 0).ok, "初始滚动场景帧失败");
        // 首帧布局反馈已回传 0/120/60/180，补推一帧上屏
        check(runtime.pumpFrame(1, {0, 120, 0, 60}, 16).ok, "反馈帧失败");
        check(test_support::textOf(runtime.scene().node(2)) == "0/120/60/180", "首帧滚动范围未回传脚本");

        InputSession input;
        WheelInput wheel;
        wheel.deltaY = -2.0f;  // 2 行 * 16px = 32px
        wheel.unitY = WheelUnit::Lines;
        wheel.timeMillis = 50.0;
        const auto res = input.wheel(runtime.scene().tree(), 1, {20, 20}, wheel, runtime.publishedFrame().revision, runtime.publishedFrame().content.hitTest.get());
        check(res.scroll.consumed && res.scroll.eventSlot.valid(), "滚轮未生成脚本滚动事件");
        runtime.enqueueScrollSnapshotEvent(res.scroll.eventSlot, res.scroll);
        check(runtime.pumpFrame(1, {0, 120, 0, 60}, 32).ok, "滚轮驱动重排帧失败");
        check(test_support::textOf(runtime.scene().node(2)) == "32/120/60/180", "滚轮位移未闭环同步至脚本并发布新帧");
    }
}

int main() {
    try {
        verifyRoutingHandoffAndSessionLock();
        verifyWheelSmoother();
        verifyScriptScrollEndToEnd();
        std::cout << "canton_scroll_input：主轴选择、双轴与边界接力、会话目标锁定、WheelSmoother 与脚本闭环全部通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
