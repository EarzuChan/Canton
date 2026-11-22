#include "TextFixtures.h"
#include <core/EventSlot.h>
#include <core/Layout.h>
#include <core/NativeScene.h>
#include <core/LayoutTree.h>
#include <core/Mutation.h>
#include <core/Paint.h>
#include <core/SceneFramePipeline.h>
#include <quickjs/QuickJsScriptHost.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <string>
#include <variant>

namespace {
    bool treeContainsBackgroundColor(const canton::core::LayoutTree& tree, canton::core::NodeId root, std::uint32_t color) {
        canton::core::DrawOpsBuilder paint;
        for (const auto& op : paint.exportScene(tree, root)) {
            if (op.type == canton::core::DrawOpType::FillRect && op.color == color) return true;
        }
        return false;
    }

    std::optional<canton::core::EventSlotId> firstEventSlotInTree(const canton::core::LayoutTree& tree, canton::core::EventSlotKind kind) {
        for (const auto id : tree.nodeIds()) {
            for (const auto& instance : tree.node(id).modifier.elements()) {
                canton::core::EventSlotId slot;
                if (const auto* input = std::get_if<canton::core::InputModifierSemantics>(&instance.descriptor.value)) slot = input->eventSlot;
                if (const auto* input = std::get_if<canton::core::LayoutModifierSemantics>(&instance.descriptor.value)) slot = input->eventSlot;
                if (slot.kind == kind && slot.valid()) return slot;
            }
        }
        return std::nullopt;
    }

    // 夹具的页面底色与卡片白底
    bool hasInitialVisuals(const canton::core::LayoutTree& tree) {
        return treeContainsBackgroundColor(tree, 1, 0xfff2f2f7u) && treeContainsBackgroundColor(tree, 1, 0xffffffffu);
    }

    // 卡片可点击，页面可滚动
    bool hasInitialEventSlots(const canton::core::LayoutTree& tree) {
        const auto click = firstEventSlotInTree(tree, canton::core::EventSlotKind::Click);
        const auto scroll = firstEventSlotInTree(tree, canton::core::EventSlotKind::VerticalScroll);
        return click && click->valid() && scroll && scroll->valid();
    }

    bool transactionContainsTypedModifier(const canton::core::MutationTransaction& transaction) {
        for (const auto& operation : transaction.operations) {
            const auto* update = std::get_if<canton::core::SlotUpdate>(&operation);
            if (update && std::holds_alternative<canton::core::ModifierDescriptors>(update->value)) return true;
        }
        return false;
    }

    std::string strictModifierSmokeSource(std::string_view modifierExpression) {
        return std::string("const native = globalThis.__CANTON_NATIVE__;\n") + "native.createNode(1, 'LayoutNode');\n" + "if (!native.setModifier) throw new Error('缺少 native.setModifier');\n" + "native.setModifier(1, " + std::string(modifierExpression) + ");\n" + "native.createNode(2, 'LayoutNode');\n" + "native.setModifier(2, { elements: [] });\n" + "native.insertChild(1, 2, 0);\n";
    }

    bool expectScriptDiagnostics(canton::quickjs::QuickJsScriptHost& host) {
        const auto source =
            "const native = globalThis.__CANTON_NATIVE__;\n"
            "native.log('d', 'Smoke', ['调试日志', '详情']);\n"
            "native.diagnosticsToast('i', 'Smoke', '提示日志', ['详情'], true);\n"
            "native.diagnosticsSetToastsEnabled(false);\n"
            "native.diagnosticsRequestReload({ path: 'src/App.cta', timestamp: 12 });\n"
            "native.createNode(1, 'LayoutNode');\n"
            "native.createNode(2, 'LayoutNode');\n"
            "native.setModifier(2, { elements: [] });\n"
            "native.insertChild(1, 2, 0);\n";
        const auto result = host.executeModule("diagnostics-smoke.js", source);
        if (!result.ok) {
            std::cerr << result.error << "\n";
            return false;
        }
        auto toasts = host.takeDiagnosticToasts();
        auto actions = host.takeDiagnosticActions();
        if (toasts.size() != 1) return false;
        if (toasts[0].level != canton::core::LogLevel::Info || toasts[0].tag != "Smoke" || toasts[0].title != "提示日志" || toasts[0].content != "详情") return false;
        if (actions.size() != 2) return false;
        if (actions[0].kind != canton::quickjs::QuickJsDiagnosticActionKind::SetToastsEnabled || actions[0].enabled) return false;
        if (actions[1].kind != canton::quickjs::QuickJsDiagnosticActionKind::RequestReload || actions[1].path != "src/App.cta" || actions[1].timestamp != 12.0) return false;
        if (host.hasPendingDiagnostics()) return false;
        return true;
    }

    bool expectScriptDiagnosticsRejection(canton::quickjs::QuickJsScriptHost& host) {
        {
            const auto result = host.executeModule("diagnostics-invalid-level-smoke.js",
                                                   "const native = globalThis.__CANTON_NATIVE__;\n"
                                                   "native.createNode(1, 'LayoutNode');\n"
                                                   "native.setModifier(1, { elements: [] });\n"
                                                   "native.log('x', 'Smoke', ['非法测试']);\n");
            if (result.ok || result.error.find("Log 级别") == std::string::npos || !host.takeDiagnosticToasts().empty()) {
                std::cerr << "非法日志级别调用结果=" << (result.ok ? "true" : "false") << " 错误=[" << result.error << "]\n";
                return false;
            }
        }
        return true;
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "用法：canton_quickjs_app_smoke <ui/app.js> [测试选项]\n";
        return 2;
    }

    canton::quickjs::QuickJsScriptHost host;
    if (argc >= 3 && std::string(argv[2]) == "--expect-script-diagnostics") {
        if (!expectScriptDiagnostics(host)) return 43;
        std::cout << "QuickJS 日志直达统一日志点，Toast 请求正确交付\n";
        return 0;
    }
    if (argc >= 3 && std::string(argv[2]) == "--expect-script-diagnostics-rejection") {
        if (!expectScriptDiagnosticsRejection(host)) return 44;
        std::cout << "QuickJS 诊断测试拒绝非法日志级别\n";
        return 0;
    }
    if (argc >= 4 && std::string(argv[2]) == "--expect-strict-modifier-ok") {
        const auto source = strictModifierSmokeSource(argv[3]);
        const auto result = host.executeModule("strict-modifier-smoke.js", source);
        if (!result.ok) {
            std::cerr << result.error << "\n";
            return 38;
        }
        const auto transaction = host.takePendingTransaction();
        if (!transaction) return 39;
        std::uint32_t modifiers = 0;
        for (const auto& operation : transaction->operations) {
            const auto* update = std::get_if<canton::core::SlotUpdate>(&operation);
            if (update && std::holds_alternative<canton::core::ModifierDescriptors>(update->value)) ++modifiers;
        }
        if (modifiers != 2) return 40;
        std::cout << "QuickJS Modifier 测试接受合法直接对象\n";
        return 0;
    }
    if (argc >= 4 && std::string(argv[2]) == "--expect-strict-modifier-error") {
        const auto result = host.executeModule("strict-modifier-smoke.js", strictModifierSmokeSource(argv[3]));
        if (result.ok) {
            std::cerr << "Modifier 应拒绝表达式：" << argv[3] << "\n";
            return 41;
        }
        if (result.error.find("TypeError:") == std::string::npos || result.error.find("setModifier") == std::string::npos) {
            std::cerr << result.error << "\n";
            return 42;
        }
        std::cout << "QuickJS Modifier 已拒绝：" << result.error.substr(0, result.error.find('\n')) << "\n";
        return 0;
    }

    if (argc != 2) {
        std::cerr << "未知或不完整的测试选项：" << argv[2] << "\n";
        return 37;
    }

    const auto path = std::filesystem::u8path(argv[1]);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        std::cerr << "无法读取 app-smoke 夹具\n";
        return 3;
    }
    const std::string source(std::istreambuf_iterator<char>{input}, {});
    const auto loaded = host.executeModule(path, source);
    if (!loaded.ok) {
        std::cerr << loaded.error << "\n";
        return 3;
    }

    if (const auto prepared = host.prepareVisualFrame(0); !prepared.ok) {
        std::cerr << prepared.error << '\n';
        return 8;
    }
    auto initialTransaction = host.takePendingTransaction();
    if (!initialTransaction || !initialTransaction->hasTreeMutations()) return 4;
    const auto initialEventSlotUpdates = std::count_if(initialTransaction->operations.begin(), initialTransaction->operations.end(), [](const auto& op) { return std::holds_alternative<canton::core::RegisterEventSlot>(op); });
    if (initialEventSlotUpdates == 0 || static_cast<std::size_t>(initialEventSlotUpdates) != host.eventSlotCount()) {
        std::cerr << "初始事务的事件槽注册与宿主账本不一致\n";
        return 21;
    }

    if (!transactionContainsTypedModifier(*initialTransaction)) return 6;

    canton::core::NativeScene scene;
    canton::core::SceneFramePipeline pipeline;
    canton::core::PublishedFrame published;
    const canton::core::FramePreparation preparation{
        [&](canton::core::NodeId id, const std::vector<int>& indices) {
            const auto materialized = host.materializeLayout(id, indices);
            if (!materialized.ok) throw std::runtime_error(materialized.error);
            auto transaction = host.takePendingTransaction();
            if (!transaction) throw std::runtime_error("Layout 材料化未提交原生候选");
            return std::move(*transaction);
        },
        [&](canton::core::NativeScene& candidate) {
            const auto prepared = host.prepareDrawModifiers(candidate);
            if (!prepared.ok) throw std::runtime_error(prepared.error);
        },
    };
    const auto frame = pipeline.run(scene, 1, {0.0f, 520.0f, 0.0f, 300.0f}, &*initialTransaction, true, published, {}, 0, preparation);
    if (frame.error) {
        (void)host.completeRearrange(frame.rearrange, *frame.error);
        (void)host.completeVisualFrame(false);
        std::cerr << *frame.error << "\n";
        return 7;
    }
    auto& tree = scene.tree();
    if (!tree.contains(1) || !hasInitialVisuals(tree) || !hasInitialEventSlots(tree)) {
        std::cerr << "app-smoke 初始树不符：根节点=" << tree.contains(1) << " 首页颜色=" << hasInitialVisuals(tree) << " 事件槽=" << hasInitialEventSlots(tree) << '\n';
        return 7;
    }

    host.publishScene(scene);
    if (const auto result = host.completeRearrange(initialTransaction->rearrange); !result.ok) throw std::runtime_error(result.error);

    if (const auto completed = host.completeVisualFrame(true); !completed.ok) throw std::runtime_error(completed.error);

    std::cout << "QuickJS 应用加载与类型化事务验证通过，操作数=" << initialTransaction->operations.size() << "\n";
    return 0;
}
