// 焦点与文本输入全链路原生测试：
// 1. 焦点顺序/方向搜索、focusGroup 分组、focusProperties 覆写、scrollFocusIntoView、节点退休回退与 QuickJS focusCommand 桥接；
// 2. LazyLayout 焦点钉住（prepareLazyInteraction）与跨离屏项方向导航材料化；
// 3. Input 单行提交/多行换行、词级移动/选区/删除、多击选词（双/三/四击）、连续输入分组与 Undo/Redo、剪贴板与 readOnly（禁开 HIMC）；
// 4. 显式 compositionRange 组合模型（预编辑期间不触发 onValueChange、目标子句粗下划线区分、同轮上屏续写、Esc/失焦取消恢复、整次组合合为单条 Undo）。
#include "TextFixtures.h"

#include <core/Focus.h>
#include <core/HitTest.h>
#include <core/InputSession.h>
#include <core/Layout.h>
#include <core/LazyLayout.h>
#include <core/SceneFramePipeline.h>
#include <quickjs/QuickJsScriptHost.h>
#include <runtime/CantonRuntime.h>
#include <runtime/InputTextSession.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace canton::core;

namespace {
    void check(bool cond, const std::string& msg) {
        if (!cond) throw std::runtime_error(msg);
    }

    LayoutModifierSemantics fixedSize(float w, float h) {
        LayoutModifierSemantics s;
        s.kind = LayoutModifierKind::Size;
        s.width = w;
        s.height = h;
        return s;
    }

    FocusModifier makeRequester(std::uint32_t id) {
        FocusModifier f;
        f.kind = FocusModifierKind::Requester;
        f.requester = id;
        return f;
    }

    InputModifierSemantics focusableSemantics() {
        InputModifierSemantics s;
        s.kind = InputModifierKind::Focusable;
        return s;
    }

    PropValue policy(const char* kind) {
        return PropValue::objectValue({{"kind", PropValue::stringValue(kind)}});
    }

    // 1. 焦点搜索、分组、自定义方向、滚动进视口与 QuickJS focusCommand 贯通
    void verifyFocusSearchScrollAndBridge() {
        LayoutTree tree;
        LayoutModifierSemantics vscroll;
        vscroll.kind = LayoutModifierKind::VerticalScroll;
        vscroll.scrollValue = 0.0f;
        vscroll.eventSlot = makeEventSlotId(1, EventSlotKind::VerticalScroll);

        FocusModifier group;
        group.kind = FocusModifierKind::Group;

        FocusModifier customProps;
        customProps.kind = FocusModifierKind::Properties;
        customProps.directions[static_cast<std::size_t>(FocusDirection::Right)] = 30;

        tree.apply({
            CreateNodeMutation{1, NodeType::Layout},
            SetPropMutation{1, "measurePolicy", policy("Column")},
            SetModifierMutation{1, {{fixedSize(200, 80)}, {vscroll}}},

            CreateNodeMutation{2, NodeType::Layout},
            SetPropMutation{2, "measurePolicy", policy("Column")},
            SetModifierMutation{2, {{group}}},
            InsertChildMutation{1, 2, 0},

            CreateNodeMutation{3, NodeType::Layout},
            SetPropMutation{3, "measurePolicy", policy("MinSize")},
            SetModifierMutation{3, {{fixedSize(120, 50)}, {makeRequester(10)}, {customProps}, {focusableSemantics()}}},
            InsertChildMutation{2, 3, 0},

            CreateNodeMutation{4, NodeType::Layout},
            SetPropMutation{4, "measurePolicy", policy("MinSize")},
            SetModifierMutation{4, {{fixedSize(120, 50)}, {makeRequester(20)}, {focusableSemantics()}}},
            InsertChildMutation{2, 4, 1},

            CreateNodeMutation{5, NodeType::Layout},
            SetPropMutation{5, "measurePolicy", policy("MinSize")},
            SetModifierMutation{5, {{fixedSize(120, 50)}, {makeRequester(30)}, {focusableSemantics()}}},
            InsertChildMutation{1, 5, 1},
        });
        LayoutEngine{}.layout(tree, 1, {0, 200, 0, 80});

        InputSession session;
        canton::runtime::InputTextSession text;
        std::vector<ScrollResult> scrolls;
        InputCallbacks cb;
        cb.invokeScrollEvent = [&](const EventSlotId&, const ScrollResult& s) { scrolls.push_back(s); };

        // 请求聚焦第 1 项，再按自定义 Right 直接跳至第 3 项（y = 100..150 超出 80px 视口，自动触发 scrollFocusIntoView）
        session.synchronizePublishedFocus(tree, true, text, {{FocusCommandKind::Request, 10}});
        session.dispatchCommittedFocus(tree, text, cb);
        check(session.focusedNode() == 3, "requester(10) 未命中节点 3");

        check(session.moveFocus(tree, FocusDirection::Right, text, cb) && session.focusedNode() == 5, "focusProperties 自定义方向未跳转到 requester(30)");
        check(!scrolls.empty() && scrolls.back().value >= 70.0f, "焦点移入视口下方节点未触发 scrollFocusIntoView");

        // 禁用当前聚焦节点，同步发布后自动清除失效焦点
        tree.apply({SetPropMutation{5, "enabled", PropValue::booleanValue(false)}});
        session.synchronizePublishedFocus(tree, true, text, {});
        check(!session.focusedNode().has_value(), "聚焦节点被禁用后未释放焦点");

        // QuickJS focusCommand 桥接贯通
        auto host = std::make_unique<canton::quickjs::QuickJsScriptHost>();
        const auto loaded = host->executeModule("focus-bridge.js", R"JS(
            const n = globalThis.__CANTON_NATIVE__
            n.createNode(1, 'LayoutNode')
            n.updateBinding(n.registerBinding(1, 'measurePolicy'), { kind: 'Column' })
            n.createNode(2, 'LayoutNode')
            n.insertChild(1, 2, 0)
            n.setModifier(2, { elements: [{ type: 'size', value: { width: 80, height: 24 } }, { type: 'focusRequester', value: { requester: 7 } }, { type: 'onFocusChanged', value: { callback: s => n.diagnosticsToast('i', 'focus', 'state', [String(s.isFocused)], false) } }, { type: 'focusable', value: {} }] })
            n.focusCommand('request', 7)
        )JS");
        check(loaded.ok, loaded.error);
        canton::runtime::CantonRuntime runtime;
        auto tx = host->takePendingTransaction();
        const auto cmds = host->takeFocusCommands();
        check(cmds.size() == 1 && cmds[0].kind == FocusCommandKind::Request && cmds[0].requester == 7, "QuickJS focusCommand 未送达宿主队列");
        runtime.setScriptHost(std::move(host));
        runtime.enqueue(std::move(*tx));
        InputSession bridgeSession;
        canton::runtime::InputTextSession bridgeText;
        check(runtime.pumpFrame(1, {0, 200, 0, 100}, 0, [&](const NativeScene& scene, PublishedFrame&) {
            bridgeSession.synchronizePublishedFocus(scene.tree(), true, bridgeText, cmds);
        }).ok, "QuickJS 焦点桥接帧失败");
        InputCallbacks bridgeCb;
        bridgeCb.invokeFocusEvent = [&](const EventSlotId& slot, bool focused, bool hasFocus) {
            runtime.enqueueFocusEvent(slot, focused, hasFocus);
        };
        bridgeSession.dispatchCommittedFocus(runtime.scene().tree(), bridgeText, bridgeCb);
        check(runtime.semanticCheckpoint(16).ok, "焦点通知回调执行失败");
        const auto toasts = runtime.takeDiagnosticToasts();
        check(toasts.size() == 1 && toasts[0].content == "true", "onFocusChanged 未向 JS 交付真实焦点状态");
    }

    // 2. Lazy 焦点钉住与跨项导航
    void verifyLazyFocusNavigation() {
        LayoutTree tree;
        LazyMeasurePolicy lazy;
        lazy.keys = {"k0", "k1", "k2", "k3", "k4"};
        lazy.contentTypes = {"t", "t", "t", "t", "t"};
        lazy.spans = {1, 1, 1, 1, 1};
        lazy.indices = {0, 1};

        LayoutModifierSemantics scroll;
        scroll.kind = LayoutModifierKind::VerticalScroll;
        scroll.eventSlot = makeEventSlotId(1, EventSlotKind::VerticalScroll);

        tree.apply({
            CreateNodeMutation{1, NodeType::Layout},
            SetModifierMutation{1, {{fixedSize(100, 60)}, {scroll}}},
            CreateNodeMutation{10, NodeType::Layout},
            SetPropMutation{10, "measurePolicy", policy("MinSize")},
            SetModifierMutation{10, {{fixedSize(100, 40)}, {makeRequester(1)}, {focusableSemantics()}}},
            CreateNodeMutation{11, NodeType::Layout},
            SetPropMutation{11, "measurePolicy", policy("MinSize")},
            SetModifierMutation{11, {{fixedSize(100, 40)}, {makeRequester(2)}, {focusableSemantics()}}},
            CreateNodeMutation{12, NodeType::Layout},
            SetPropMutation{12, "measurePolicy", policy("MinSize")},
            SetModifierMutation{12, {{fixedSize(100, 40)}, {makeRequester(3)}, {focusableSemantics()}}},
            CreateNodeMutation{13, NodeType::Layout},
            SetPropMutation{13, "measurePolicy", policy("MinSize")},
            SetModifierMutation{13, {{fixedSize(100, 40)}, {makeRequester(4)}, {focusableSemantics()}}},
            CreateNodeMutation{14, NodeType::Layout},
            SetPropMutation{14, "measurePolicy", policy("MinSize")},
            SetModifierMutation{14, {{fixedSize(100, 40)}, {makeRequester(5)}, {focusableSemantics()}}},
            InsertChildMutation{1, 10, 0},
            InsertChildMutation{1, 11, 1},
        });
        tree.mutableNode(1).measurePolicy = lazy;
        LayoutEngine engine;
        engine.setMaterializer([&](NodeId, const std::vector<int>& req) {
            auto next = std::get<LazyMeasurePolicy>(tree.node(1).measurePolicy);
            next.indices = req;
            tree.mutableNode(1).measurePolicy = next;
            const auto existing = tree.node(1).children;
            for (const auto cid : existing) tree.apply({RemoveChildMutation{1, cid}});
            for (std::uint32_t i = 0; i < req.size(); ++i) {
                tree.apply({InsertChildMutation{1, static_cast<NodeId>(10 + req[i]), i}});
            }
        });
        engine.layout(tree, 1, {0, 100, 0, 60});

        InputSession session;
        canton::runtime::InputTextSession text;
        InputCallbacks cb;
        session.synchronizePublishedFocus(tree, true, text, {{FocusCommandKind::Request, 1}});
        session.dispatchCommittedFocus(tree, text, cb);
        check(session.focusedNode() == 10, "Lazy 首项未获得焦点");

        // prepareLazyInteraction 把聚焦项钉入 pinnedKeys；跨 Lazy 项移动发起 pendingLazyFocus，经下一拍布局材料化后由 synchronizePublishedFocus 落焦（含离屏项材料化）
        const auto stepLazyDown = [&](NodeId expected) {
            check(session.moveFocus(tree, FocusDirection::Down, text, cb), "Lazy moveFocus(Down) 未发起请求");
            session.prepareLazyInteraction(tree);
            engine.layout(tree, 1, {0, 100, 0, 60});
            session.synchronizePublishedFocus(tree, true, text, {});
            session.dispatchCommittedFocus(tree, text, cb);
            check(session.focusedNode() == expected, "Lazy 跨项焦点材料化与落焦失败");
        };
        session.prepareLazyInteraction(tree);
        const auto& pinned = tree.node(1).lazy->pinnedKeys;
        check(tree.node(1).lazy && std::find(pinned.begin(), pinned.end(), "k0") != pinned.end(), "Lazy 未钉住当前拥有焦点的项 key");

        stepLazyDown(11);
        stepLazyDown(12);
        stepLazyDown(13);
    }

    // 3. Input 编辑增强、多击选词、撤销重做、剪贴板与 readOnly
    void verifyInputEditingAndReadOnly() {
        class FixedMeasurer final : public ApproximateTextMeasurer {
           protected:
            float advance(std::string_view, char32_t, const TextStyle&) const override { return 8.0f; }
        } measurer;
        TextLayoutService textService(measurer);

        LayoutTree tree;
        auto single = test_support::textField("hello world foo", "hint");
        single.presentation.singleLine = true;
        single.onValueChange = makeEventSlotId(2, EventSlotKind::InputUpdate);
        single.onSubmit = makeEventSlotId(2, EventSlotKind::InputSubmit);
        single.onChange = makeEventSlotId(2, EventSlotKind::InputChange);
        single.onBlur = makeEventSlotId(2, EventSlotKind::InputBlur);

        auto multiline = test_support::textField("ab\ncdefgh\nij", "");
        multiline.presentation.singleLine = false;
        multiline.presentation.minLines = 3;
        multiline.presentation.maxLines = 5;

        auto ro = test_support::textField("secret-token", "");
        ro.readOnly = true;
        ro.selectAllOnFocus = true;

        tree.apply({
            CreateNodeMutation{1, NodeType::Layout},
            SetPropMutation{1, "measurePolicy", policy("Column")},
            SetModifierMutation{1, {{fixedSize(300, 300)}}},
            CreateNodeMutation{2, NodeType::Layout},
            SetPropMutation{2, "measurePolicy", policy("MinSize")},
            SetModifierMutation{2, {{fixedSize(240, 32)}, {single}}},
            InsertChildMutation{1, 2, 0},
            CreateNodeMutation{3, NodeType::Layout},
            SetPropMutation{3, "measurePolicy", policy("MinSize")},
            SetModifierMutation{3, {{fixedSize(240, 90)}, {multiline}}},
            InsertChildMutation{1, 3, 1},
            CreateNodeMutation{4, NodeType::Layout},
            SetPropMutation{4, "measurePolicy", policy("MinSize")},
            SetModifierMutation{4, {{fixedSize(240, 32)}, {ro}}},
            InsertChildMutation{1, 4, 2},
        });
        LayoutEngine(textService).layout(tree, 1, {0, 300, 0, 300});

        canton::runtime::InputTextSession text(textService);
        std::string clipboard = "pasted";
        std::vector<std::string> updates, submits, changes, blurs;
        InputCallbacks cb;
        cb.readClipboard = [&] { return clipboard; };
        cb.writeClipboard = [&](const std::string& v) { clipboard = v; };
        cb.invokeStringEvent = [&](const EventSlotId& slot, const std::string& v) {
            if (slot.kind == EventSlotKind::InputUpdate) updates.push_back(v);
            if (slot.kind == EventSlotKind::InputSubmit) submits.push_back(v);
            if (slot.kind == EventSlotKind::InputChange) changes.push_back(v);
            if (slot.kind == EventSlotKind::InputBlur) blurs.push_back(v);
        };

        const auto singleHandle = tree.node(2).modifier.elements()[1].handle;
        text.focus(tree, 2, singleHandle, cb);
        check(text.isEditableActive(tree, true), "普通可编辑 Input 应允许开启输入法上下文");

        // 词级跳转、词级选区与词删除
        KeyModifierState ctrl{}, ctrlShift{};
        ctrl.ctrl = ctrl.command = true;
        ctrlShift.ctrl = ctrlShift.command = ctrlShift.shift = true;
        check(text.keyPressed(tree, true, {0, "ArrowLeft", {}, ctrl}, cb) && text.state().cursorIndex() == 12, "Ctrl+ArrowLeft 未跳至最后一个词首");
        check(text.keyPressed(tree, true, {0, "ArrowLeft", {}, ctrlShift}, cb) && text.state().selectedText() == "world ", "Ctrl+Shift+ArrowLeft 未选中上一词区间");
        check(text.keyPressed(tree, true, {0, "End", {}, {}}, cb), "End 移动失败");
        check(text.keyPressed(tree, true, {0, "Backspace", {}, ctrl}, cb) && text.state().text() == "hello world ", "Ctrl+Backspace 未删除前一词");

        // 双击选词、四击全选
        const auto hitSnapshot = buildHitTestSnapshot(tree, 1);
        const auto hitSingle = HitTester{}.hitTest(hitSnapshot, {12.0f, 10.0f});
        text.pointerDown(tree, hitSingle, 12.0f, 10.0f, cb, {2, false});
        check(text.state().selectedText() == "hello", "双击未按词边界选中单词");
        text.pointerDown(tree, hitSingle, 12.0f, 10.0f, cb, {4, false});
        check(text.state().selectedText() == "hello world ", "四击未选中全文");

        // 粘贴、剪切、撤销/重做与单行 Enter 提交
        check(text.keyPressed(tree, true, {0, "v", {}, ctrl}, cb) && text.state().text() == "pasted", "Ctrl+V 未粘贴剪贴板文本");
        check(text.keyPressed(tree, true, {0, "z", {}, ctrl}, cb) && text.state().text() == "hello world ", "Ctrl+Z 未回退粘贴");
        check(text.keyPressed(tree, true, {0, "y", {}, ctrl}, cb) && text.state().text() == "pasted", "Ctrl+Y 未重做粘贴");
        check(text.keyPressed(tree, true, {0, "Enter", {}, {}}, cb) && submits.size() == 1 && submits[0] == "pasted", "单行 Enter 未触发 onSubmit");

        // 多行 Enter 换行与上下移动保持 preferredX
        const auto multiHandle = tree.node(3).modifier.elements()[1].handle;
        text.focus(tree, 3, multiHandle, cb);
        check(changes.size() == 1 && blurs.size() == 1, "切换焦点未对旧输入框补发 onChange 与 onBlur");
        // 光标从第 2 行 "cdefgh" 末尾（x 较宽）上移到短行 "ab" 末尾，再下移回第 2 行应恢复原列位置（preferredX）
        (void)text.state().moveCursorTo(9);  // "ab\ncdefgh" 末尾
        check(text.keyPressed(tree, true, {0, "ArrowUp", {}, {}}, cb) && text.state().cursorIndex() == 2, "上移到短行未夹到行尾");
        check(text.keyPressed(tree, true, {0, "ArrowDown", {}, {}}, cb) && text.state().cursorIndex() == 9, "下移回长行未恢复 preferredX 列位置");
        check(text.keyPressed(tree, true, {0, "Enter", {}, {}}, cb) && text.state().text().find("cdefgh\n\nij") != std::string::npos, "多行 Enter 未插入换行");

        // readOnly：允许聚焦、全选与 Ctrl+C 复制，禁止修改且 isEditableActive 为 false（禁用 HIMC）
        const auto roHandle = tree.node(4).modifier.elements()[1].handle;
        text.focus(tree, 4, roHandle, cb);
        check(text.isActive(tree, true) && !text.isEditableActive(tree, true), "readOnly Input 必须允许聚焦但禁用输入法上下文");
        check(text.state().selectedText() == "secret-token", "readOnly selectAllOnFocus 未生效");
        check(text.keyPressed(tree, true, {0, "c", {}, ctrl}, cb) && clipboard == "secret-token", "readOnly Input 必须允许 Ctrl+C 复制");
        check(!text.insertText(tree, true, "hack", cb) && text.state().text() == "secret-token", "readOnly Input 不得接受字符输入");
        (void)text.keyPressed(tree, true, {0, "Backspace", {}, {}}, cb);
        (void)text.keyPressed(tree, true, {0, "v", {}, ctrl}, cb);
        check(text.state().text() == "secret-token", "readOnly Input 不得响应删除或粘贴修改");
    }

    // 4. 显式 compositionRange 与分段目标标记、同轮上屏续写、取消恢复、单条 Undo
    void verifyExplicitCompositionRange() {
        class FixedMeasurer final : public ApproximateTextMeasurer {
           protected:
            float advance(std::string_view, char32_t, const TextStyle&) const override { return 10.0f; }
        } measurer;
        TextLayoutService textService(measurer);

        LayoutTree tree;
        auto field = test_support::textField("前缀", "");
        field.onValueChange = makeEventSlotId(1, EventSlotKind::InputUpdate);
        tree.apply({
            CreateNodeMutation{1, NodeType::Layout},
            SetPropMutation{1, "measurePolicy", policy("MinSize")},
            SetModifierMutation{1, {{fixedSize(260, 36)}, {field}}},
        });
        LayoutEngine(textService).layout(tree, 1, {0, 260, 0, 36});

        canton::runtime::InputTextSession text(textService);
        std::vector<std::string> jsUpdates;
        int platformCancels = 0;
        text.setCancelCompositionHook([&] { ++platformCancels; });
        InputCallbacks cb;
        cb.invokeStringEvent = [&](const EventSlotId& slot, const std::string& val) {
            if (slot.kind == EventSlotKind::InputUpdate) jsUpdates.push_back(val);
        };

        text.focus(tree, 1, tree.node(1).modifier.elements()[1].handle, cb);

        // 组合预编辑包含两个子句：第 1 段已转换目标段（target=true），第 2 段普通未定段（target=false）
        const std::string preedit = "世界nihao";
        check(text.setComposition(tree, true, preedit, preedit.size(), {{0, 6, true}, {6, preedit.size(), false}}, cb), "setComposition 未消费");
        check(text.state().composing() && text.state().compositionRange() == std::optional<TextSelectionRange>{{6, 6 + preedit.size()}}, "compositionRange UTF-8 区间不正确");
        check(jsUpdates.empty(), "IME 组合进行中不得触发 JS onValueChange");

        // 叠层绘制必须区分目标子句粗下划线（2px）与普通子句细下划线（1px）
        const auto overlayOps = text.buildOverlayOps(tree, true);
        bool sawTargetLine = false, sawNormalLine = false;
        for (const auto& op : overlayOps) {
            if (op.type == DrawOpType::DrawLine && op.lineEnd.x > op.rect.x) {
                if (std::abs(op.strokeWidth - 2.0f) < 0.01f) sawTargetLine = true;
                if (std::abs(op.strokeWidth - 1.0f) < 0.01f) sawNormalLine = true;
            }
        }
        check(sawTargetLine && sawNormalLine, "TextInputOverlayBuilder 未区分目标组合段粗下划线与普通段细下划线");

        // 同轮先上屏前半句“世界”，紧接着续写后半句“你好”（模拟同一条 WM_IME_COMPOSITION 携带 GCS_RESULTSTR | GCS_COMPSTR）
        check(text.commitComposition(tree, true, std::string("世界"), cb), "前半句上屏失败");
        check(jsUpdates.size() == 1 && jsUpdates.back() == "前缀世界", "GCS_RESULTSTR 上屏后未精确触发一次 onValueChange");
        check(text.setComposition(tree, true, "nihao", 5, {{0, 5, false}}, cb), "同轮续写预编辑失败");
        check(jsUpdates.size() == 1 && text.state().text() == "前缀世界nihao", "续写预编辑误触发了 onValueChange 或丢失上屏文本");

        // 按 Esc 取消未完成续写，恢复至已上屏状态“前缀世界”并通知平台收起候选框
        check(text.keyPressed(tree, true, {27, "Escape", {}, {}}, cb), "Escape 未取消活跃组合");
        check(!text.state().composing() && text.state().text() == "前缀世界" && platformCancels == 1, "Escape 取消组合后未恢复文本或未通知平台 CPS_CANCEL");

        // 再次完整组合上屏“和平”，单次 Ctrl+Z 必须把整次组合“和平”一次性撤销回“前缀世界”
        (void)text.setComposition(tree, true, "hp", 2, {}, cb);
        (void)text.setComposition(tree, true, "heping", 6, {}, cb);
        (void)text.commitComposition(tree, true, std::string("和平"), cb);
        check(jsUpdates.back() == "前缀世界和平", "完整组合上屏文本不正确");
        KeyModifierState ctrl{};
        ctrl.ctrl = ctrl.command = true;
        check(text.keyPressed(tree, true, {0, "z", {}, ctrl}, cb) && text.state().text() == "前缀世界", "整次 IME 组合未合并为单条 Undo 记录");
    }
}

int main() {
    try {
        verifyFocusSearchScrollAndBridge();
        verifyLazyFocusNavigation();
        verifyInputEditingAndReadOnly();
        verifyExplicitCompositionRange();
        std::cout << "canton_focus_input：焦点搜索/分组/滚动、Lazy 焦点钉住、词级与多击编辑、readOnly 与显式 compositionRange 全部通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
