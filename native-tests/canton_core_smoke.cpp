#include "TextFixtures.h"
#include <core/EventSlot.h>
#include <core/HitTest.h>
#include <core/InputEditing.h>
#include <core/InputIntent.h>
#include <core/Layout.h>
#include <core/LayoutTree.h>
#include <core/Mutation.h>
#include <core/MutationTransaction.h>
#include <core/NativeScene.h>
#include <core/Paint.h>
#include <core/PointerGesture.h>
#include <core/PropSchema.h>
#include <core/PropValue.h>
#include <core/SceneFramePipeline.h>
#include <core/Scroll.h>
#include <core/TextLayoutService.h>
#include <core/Version.h>
#include <runtime/DevServerClient.h>
#include <runtime/ErrorScreenModel.h>
#include <runtime/FramePlanner.h>
#include <runtime/ScenePipelineState.h>

#include <cmath>
#include <iostream>
#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

namespace {
    bool near(float actual, float expected, float epsilon = 0.01f) {
        return std::fabs(actual - expected) <= epsilon;
    }

    bool hasDirty(const canton::core::LayoutNode& node, canton::core::DirtyFlag flag) {
        return (node.dirty & canton::core::dirtyMask(flag)) != 0;
    }

    bool phaseRan(const canton::core::PhaseSet& phases, canton::core::FramePhase phase) {
        return phases.ran(phase);
    }

    canton::core::PropValue object(std::vector<canton::core::PropObjectField> fields) {
        return canton::core::PropValue::objectValue(std::move(fields));
    }

    canton::core::PropObjectField field(std::string key, canton::core::PropValue value) {
        return {std::move(key), std::move(value)};
    }

    canton::core::ModifierDescriptors size(float width, float height) {
        canton::core::ModifierDescriptors modifier;
        canton::core::LayoutModifierSemantics item;
        item.kind = canton::core::LayoutModifierKind::Size;
        item.width = width;
        item.height = height;
        modifier.push_back({item, {}});
        return modifier;
    }

    canton::core::ModifierDescriptors background(float width, float height, std::uint32_t color) {
        auto modifier = size(width, height);
        canton::core::PaintStyleSemantics style;
        style.kind = canton::core::PaintStyleKind::Background;
        style.color = color;
        modifier.push_back({style, {}});
        return modifier;
    }

    canton::core::ModifierDescriptors clickableBox(float width, float height, canton::core::EventSlotId slot) {
        auto modifier = background(width, height, 0xff3a7afeu);
        canton::core::InputModifierSemantics input;
        input.eventSlot = std::move(slot);
        modifier.push_back({input, {}});
        return modifier;
    }

    canton::core::ModifierDescriptors verticalScroll(float width, float height, float value, canton::core::EventSlotId slot) {
        auto modifier = background(width, height, 0xff151922u);
        canton::core::LayoutModifierSemantics scroll;
        scroll.kind = canton::core::LayoutModifierKind::VerticalScroll;
        scroll.scrollValue = value;
        scroll.eventSlot = std::move(slot);
        modifier.push_back({scroll, {}});
        return modifier;
    }

    canton::core::MutationTransaction initialTreeTransaction() {
        using namespace canton::core;
        MutationTransaction transaction;
        auto title = background(220, 32, 0xff3a7afeu);
        title.push_back({test_support::text("Hello typed transaction", 0xffe8eaedu, 20), {}});
        auto fieldModifier = background(160, 28, 0xff151922u);
        auto editor = test_support::textField("Gain", "Search preset", 0xffffffffu);
        editor.selectAllOnFocus = true;
        editor.onSubmit = makeEventSlotId(6, EventSlotKind::InputSubmit);
        fieldModifier.push_back({editor, {}});
        transaction.operations = {
            CreateNodeMutation{1, canton::core::NodeType::Layout}, canton::core::SetPropMutation{1, "measurePolicy", canton::core::PropValue::objectValue({{"kind", canton::core::PropValue::stringValue("Column")}})}, SetModifierMutation{1, background(320.0f, 180.0f, 0xff000000u)}, canton::core::SetPropMutation{1, "measurePolicy", object({field("kind", PropValue::stringValue("Column")), field("verticalArrangement", object({field("kind", PropValue::stringValue("spacedBy")), field("space", PropValue::numberValue(8.0))}))})}, CreateNodeMutation{2, canton::core::NodeType::Layout}, SetPropMutation{2, "measurePolicy", canton::core::PropValue::objectValue({{"kind", canton::core::PropValue::stringValue("MinSize")}})}, SetModifierMutation{2, title}, InsertChildMutation{1, 2, 0}, CreateNodeMutation{3, canton::core::NodeType::Layout}, canton::core::SetPropMutation{3, "measurePolicy", canton::core::PropValue::objectValue({{"kind", canton::core::PropValue::stringValue("Box")}})}, SetModifierMutation{3, clickableBox(80.0f, 40.0f, makeEventSlotId(3, EventSlotKind::Click))}, InsertChildMutation{1, 3, 1}, CreateNodeMutation{4, canton::core::NodeType::Layout}, canton::core::SetPropMutation{4, "measurePolicy", canton::core::PropValue::objectValue({{"kind", canton::core::PropValue::stringValue("Column")}})}, SetModifierMutation{4, verticalScroll(100.0f, 40.0f, 0.0f, makeEventSlotId(4, EventSlotKind::VerticalScroll))}, InsertChildMutation{1, 4, 2}, CreateNodeMutation{5, canton::core::NodeType::Layout}, canton::core::SetPropMutation{5, "measurePolicy", canton::core::PropValue::objectValue({{"kind", canton::core::PropValue::stringValue("Box")}})}, SetModifierMutation{5, background(100.0f, 80.0f, 0xffffb020u)}, InsertChildMutation{4, 5, 0}, CreateNodeMutation{6, canton::core::NodeType::Layout}, SetPropMutation{6, "measurePolicy", canton::core::PropValue::objectValue({{"kind", canton::core::PropValue::stringValue("MinSize")}})}, SetModifierMutation{6, fieldModifier}, InsertChildMutation{1, 6, 3},
        };
        transaction.operations.emplace_back(RegisterEventSlot{makeEventSlotId(3, EventSlotKind::Click)});
        transaction.operations.emplace_back(RegisterEventSlot{makeEventSlotId(4, EventSlotKind::VerticalScroll)});
        transaction.operations.emplace_back(RegisterEventSlot{makeEventSlotId(6, EventSlotKind::InputSubmit)});
        return transaction;
    }

    int verifyTypedLayoutTreePipeline() {
        canton::core::LayoutTree tree;
        auto transaction = initialTreeTransaction();
        for (const auto& operation : transaction.operations) {
            if (const auto* mutation = std::get_if<canton::core::TreeMutation>(&operation)) tree.applyMutation(*mutation);
        }
        if (!tree.contains(1) || tree.node(1).children.size() != 4) return 1;
        if (test_support::textOf(tree.node(2)) != "Hello typed transaction") return 2;
        if (test_support::textOf(tree.node(6)) != "Gain") return 3;
        if (!hasDirty(tree.node(1), canton::core::DirtyFlag::Structure)) return 4;

        canton::core::LayoutEngine layout;
        layout.layout(tree, 1, {0.0f, 320.0f, 0.0f, 240.0f});
        if (!near(tree.node(1).bounds.width, 320.0f) || !near(tree.node(1).bounds.height, 180.0f)) return 5;
        if (!near(tree.node(2).bounds.x, 0.0f) || !near(tree.node(2).bounds.y, 0.0f)) return 6;
        if (!near(tree.node(3).bounds.y, 40.0f)) return 7;

        const auto ops = canton::core::DrawOpsBuilder{}.exportScene(tree, 1);
        bool sawRoot = false;
        bool sawBlue = false;
        bool sawText = false;
        for (const auto& op : ops) {
            if (op.type == canton::core::DrawOpType::FillRect && op.color == 0xff000000u) sawRoot = true;
            if (op.type == canton::core::DrawOpType::FillRect && op.color == 0xff3a7afeu) sawBlue = true;
            if (op.type == canton::core::DrawOpType::DrawText && op.text == "Hello typed transaction") sawText = true;
        }
        if (!sawRoot || !sawBlue || !sawText) return 8;
        return 0;
    }

    int verifyTypedDirtyPrecision() {
        canton::core::LayoutTree tree;
        auto transaction = initialTreeTransaction();
        for (const auto& operation : transaction.operations) {
            if (const auto* mutation = std::get_if<canton::core::TreeMutation>(&operation)) tree.applyMutation(*mutation);
        }
        tree.clearDirty();
        (void)tree.takeInvalidation();

        tree.apply(std::vector<canton::core::TreeMutation>{
            canton::core::SetModifierMutation{3, clickableBox(80.0f, 40.0f, canton::core::makeEventSlotId(3, canton::core::EventSlotKind::Click, "updated"))},
        });
        auto snapshot = tree.invalidationSnapshot();
        if (!snapshot.affects(canton::core::DirtyFlag::EventSlot)) return 11;
        if (snapshot.affects(canton::core::DirtyFlag::Layout)) return 12;
        if (snapshot.affects(canton::core::DirtyFlag::Paint)) return 13;

        tree.clearDirty();
        (void)tree.takeInvalidation();
        auto recolored = clickableBox(80.0f, 40.0f, canton::core::makeEventSlotId(3, canton::core::EventSlotKind::Click, "updated"));
        std::get<canton::core::PaintStyleSemantics>(recolored[1].value).color = 0xff00ff00u;
        tree.apply(std::vector<canton::core::TreeMutation>{canton::core::SetModifierMutation{3, recolored}});
        snapshot = tree.invalidationSnapshot();
        if (!snapshot.affects(canton::core::DirtyFlag::Paint)) return 14;
        if (snapshot.affects(canton::core::DirtyFlag::Layout)) return 15;

        tree.clearDirty();
        (void)tree.takeInvalidation();
        tree.apply(std::vector<canton::core::TreeMutation>{
            canton::core::SetModifierMutation{3, background(90.0f, 44.0f, 0xff00ff00u)},
        });
        snapshot = tree.invalidationSnapshot();
        if (!snapshot.affects(canton::core::DirtyFlag::Layout)) return 16;
        if (!snapshot.affects(canton::core::DirtyFlag::Paint)) return 17;
        return 0;
    }

    int verifyPropSchema() {
        using namespace canton::core;
        std::string error;
        if (validateSetPropMutation(NodeType::Layout, "unknownProp", PropValue::stringValue("非法输入"), error)) return 103;

        struct PolicyCase {
            const char* kind;
            const char* key;
            const char* valid;
            const char* invalid;
        };

        for (const auto& input : std::vector<PolicyCase>{{"Box", "contentAlignment", "CenterEnd", "Centre"}, {"Row", "verticalAlignment", "Baseline", "End"}, {"Column", "horizontalAlignment", "CenterHorizontally", "Bottom"}, {"Row", "horizontalArrangement", "SpaceBetween", "Bottom"}, {"Column", "verticalArrangement", "Bottom", "End"}}) {
            const auto policy = [&](const char* value) {
                return object({field("kind", PropValue::stringValue(input.kind)), field(input.key, PropValue::stringValue(value))});
            };
            if (!validateSetPropMutation(NodeType::Layout, "measurePolicy", policy(input.valid), error)) return 206;
            if (validateSetPropMutation(NodeType::Layout, "measurePolicy", policy(input.invalid), error)) return 207;
        }
        for (const auto* kind : {"Row", "Column"}) {
            const auto horizontal = std::string(kind) == "Row";
            const auto policy = [&](const char* alignment) {
                return object({field("kind", PropValue::stringValue(kind)), field(horizontal ? "horizontalArrangement" : "verticalArrangement", object({field("kind", PropValue::stringValue("spacedBy")), field("space", PropValue::numberValue(8)), field("alignment", PropValue::stringValue(alignment))}))});
            };
            if (!validateSetPropMutation(NodeType::Layout, "measurePolicy", policy(horizontal ? "CenterHorizontally" : "Bottom"), error)) return 208;
            if (validateSetPropMutation(NodeType::Layout, "measurePolicy", policy(horizontal ? "Top" : "End"), error)) return 209;
        }
        return 0;
    }

    int verifyAlignmentAndArrangement() {
        using namespace canton::core;
        LayoutEngine layout;
        LayoutTree box;
        box.apply({CreateNodeMutation{1, canton::core::NodeType::Layout}, canton::core::SetPropMutation{1, "measurePolicy", canton::core::PropValue::objectValue({{"kind", canton::core::PropValue::stringValue("Box")}})}, CreateNodeMutation{2, canton::core::NodeType::Layout}, canton::core::SetPropMutation{2, "measurePolicy", canton::core::PropValue::objectValue({{"kind", canton::core::PropValue::stringValue("MinSize")}})}, InsertChildMutation{1, 2, 0}, SetModifierMutation{1, size(100, 100)}, SetModifierMutation{2, size(10, 10)}});

        struct AlignmentCase {
            const char* name;
            float x;
            float y;
        };

        for (const auto& alignment : std::vector<AlignmentCase>{{"TopStart", 0, 0}, {"TopCenter", 45, 0}, {"TopEnd", 90, 0}, {"CenterStart", 0, 45}, {"Center", 45, 45}, {"CenterEnd", 90, 45}, {"BottomStart", 0, 90}, {"BottomCenter", 45, 90}, {"BottomEnd", 90, 90}}) {
            box.setHostInput(1, HostInput::MeasurePolicy, object({field("kind", PropValue::stringValue("Box")), field("contentAlignment", PropValue::stringValue(alignment.name))}));
            layout.layout(box, 1, {0, 100, 0, 100});
            if (!near(box.node(2).bounds.x, alignment.x) || !near(box.node(2).bounds.y, alignment.y)) return 211;
            box.clearDirty();
        }

        for (const bool horizontal : {true, false}) {
            LayoutTree tree;
            tree.apply({CreateNodeMutation{1, NodeType::Layout}, CreateNodeMutation{2, canton::core::NodeType::Layout}, canton::core::SetPropMutation{2, "measurePolicy", canton::core::PropValue::objectValue({{"kind", canton::core::PropValue::stringValue("MinSize")}})}, CreateNodeMutation{3, canton::core::NodeType::Layout}, canton::core::SetPropMutation{3, "measurePolicy", canton::core::PropValue::objectValue({{"kind", canton::core::PropValue::stringValue("MinSize")}})}, InsertChildMutation{1, 2, 0}, InsertChildMutation{1, 3, 1}, SetModifierMutation{1, size(100, 100)}, SetModifierMutation{2, size(10, 10)}, SetModifierMutation{3, size(10, 10)}});
            const auto* input = horizontal ? "horizontalArrangement" : "verticalArrangement";

            struct PlacementCase {
                PropValue value;
                float first;
                float second;
            };

            for (const auto& arrangement : std::vector<PlacementCase>{
                     {PropValue::stringValue(horizontal ? "Start" : "Top"), 0, 10},
                     {PropValue::stringValue("Center"), 40, 50},
                     {PropValue::stringValue(horizontal ? "End" : "Bottom"), 80, 90},
                     {PropValue::stringValue("SpaceBetween"), 0, 90},
                     {PropValue::stringValue("SpaceAround"), 20, 70},
                     {PropValue::stringValue("SpaceEvenly"), 80.0f / 3.0f, 190.0f / 3.0f},
                     {object({field("kind", PropValue::stringValue("spacedBy")), field("space", PropValue::numberValue(10)), field("alignment", PropValue::stringValue("Center"))}), 35, 55},
                     {object({field("kind", PropValue::stringValue("spacedBy")), field("space", PropValue::numberValue(10)), field("alignment", PropValue::stringValue(horizontal ? "End" : "Bottom"))}), 70, 90},
                 }) {
                tree.setHostInput(1, HostInput::MeasurePolicy, object({field("kind", PropValue::stringValue(horizontal ? "Row" : "Column")), field(input, arrangement.value)}));
                layout.layout(tree, 1, {0, 100, 0, 100});
                if (!near(horizontal ? tree.node(2).bounds.x : tree.node(2).bounds.y, arrangement.first)) return 212;
                if (!near(horizontal ? tree.node(3).bounds.x : tree.node(3).bounds.y, arrangement.second)) return 213;
                tree.clearDirty();
            }
        }
        return 0;
    }

    int verifyNativeSceneAndFramePipeline() {
        canton::core::NativeScene scene;
        canton::core::PublishedFrame published;
        canton::core::SceneFramePipeline pipeline;
        auto transaction = initialTreeTransaction();
        auto result = pipeline.run(scene, 1, {0.0f, 320.0f, 0.0f, 240.0f}, &transaction, true, published);
        if (result.error || !result.ran) return 21;
        if (!scene.contains(1) || !scene.hasEventSlot(canton::core::makeEventSlotId(3, canton::core::EventSlotKind::Click))) return 22;
        if (!phaseRan(published.phases, canton::core::FramePhase::ApplyMutations)) return 23;
        if (!phaseRan(published.phases, canton::core::FramePhase::Measure)) return 24;
        if (!phaseRan(published.phases, canton::core::FramePhase::BuildPaint)) return 25;
        if (exportDrawOps(published.content.scenePaint).empty()) return 26;

        canton::core::MutationTransaction eventOnly;
        eventOnly.operations.emplace_back(canton::core::RegisterEventSlot{canton::core::makeEventSlotId(3, canton::core::EventSlotKind::Click, "updated")});
        result = pipeline.run(scene, 1, {0.0f, 320.0f, 0.0f, 240.0f}, &eventOnly, false, published);
        if (result.error) return 27;
        if (phaseRan(published.phases, canton::core::FramePhase::Measure)) return 28;
        if (phaseRan(published.phases, canton::core::FramePhase::BuildPaint)) return 29;

        canton::core::MutationTransaction nativeInvalidation;
        nativeInvalidation.operations.emplace_back(canton::core::NativeInvalidationMutation{6, canton::core::DirtyFlag::Paint});
        result = pipeline.run(scene, 1, {0.0f, 320.0f, 0.0f, 240.0f}, &nativeInvalidation, false, published);
        if (result.error) return 30;
        if (phaseRan(published.phases, canton::core::FramePhase::Measure)) return 31;
        if (!phaseRan(published.phases, canton::core::FramePhase::BuildPaint)) return 32;
        return 0;
    }

    int verifyPointerScrollAndHitTest() {
        canton::core::LayoutTree tree;
        auto transaction = initialTreeTransaction();
        for (const auto& operation : transaction.operations) {
            if (const auto* mutation = std::get_if<canton::core::TreeMutation>(&operation)) tree.applyMutation(*mutation);
        }
        canton::core::LayoutEngine{}.layout(tree, 1, {0.0f, 320.0f, 0.0f, 240.0f});

        canton::core::PointerGestureSession pointer;
        const auto snapshot = canton::core::buildHitTestSnapshot(tree, 1);
        canton::core::PointerGestureOutput down;
        canton::core::PointerGestureOutput up;
        pointer.primaryDown(tree, snapshot, {{10.0f, 48.0f}}, 1, down);
        pointer.primaryUp(tree, snapshot, {{10.0f, 48.0f}}, up);
        if (!down.consumed || up.events.empty() || up.events.front().slot.kind != canton::core::EventSlotKind::Click) return 41;

        const auto scroll = canton::core::ScrollDispatcher{}.verticalWheel(tree, 1, {10.0f, 100.0f}, -1.0f, 24.0f);
        if (!scroll.consumed || scroll.target != 4 || !near(scroll.value, 24.0f)) return 42;

        tree.apply(std::vector<canton::core::TreeMutation>{
            canton::core::SetModifierMutation{4, verticalScroll(100.0f, 40.0f, scroll.value, canton::core::makeEventSlotId(4, canton::core::EventSlotKind::VerticalScroll))},
        });
        if (!near(canton::core::ScrollDispatcher::verticalScrollValue(tree.node(4)), 24.0f)) return 43;
        return 0;
    }

    int verifyPropValueAndTextInput() {
        auto value = object({
            field("text", canton::core::PropValue::stringValue("hello")),
            field("count", canton::core::PropValue::numberValue(12.0)),
            field("enabled", canton::core::PropValue::booleanValue(true)),
        });
        canton::core::PropObject objectValue(&value);
        if (objectValue.string("text") != "hello") return 51;
        if (objectValue.integer("count") != 12) return 52;
        if (!objectValue.boolean("enabled")) return 53;

        canton::core::TextInputState input;
        input.begin("Preset A", true);
        if (!input.hasSelection()) return 54;
        const auto replace = input.replaceSelectionWithText("B");
        if (!replace.consumed || !replace.textChanged || input.text() != "B") return 55;
        input.insertCodepoint(U'好');
        if (input.text() != "B好") return 56;
        if (!input.undo().textChanged || input.text() != "B") return 57;
        if (!input.redo().textChanged || input.text() != "B好") return 58;
        if (!input.submit().submitRequested) return 59;
        return 0;
    }

    class ExactTextMeasurer final : public canton::core::ApproximateTextMeasurer {
       public:
        float advance(std::string_view, char32_t, const canton::core::TextStyle& style) const override {
            return style.fontSize * 0.5f;
        }
    };

    int verifyTextLayoutAndOverlay() {
        ExactTextMeasurer measurer;
        canton::core::TextLayoutService service(measurer);
        const auto layout = service.layout("abcd\nef", {10.0f, 12.0f}, {0, 0.0f, false});
        if (layout->lines.size() != 2 || !near(layout->width, 20.0f) || !near(layout->height, 24.0f)) return 61;
        const auto caret = service.caretRect(*layout, 2, {4.0f, 5.0f});
        if (!near(caret.x, 14.0f) || !near(caret.y, 5.0f)) return 62;

        canton::core::ModifierInstance input;
        input.descriptor.value = test_support::textField("abcd");
        input.bounds = {0, 0, 120, 28};
        canton::core::TextInputOverlayState state;
        state.text = "abcd";
        state.cursorIndex = 2;
        state.selectionStart = 1;
        state.selectionEnd = 3;
        state.temporaryUnderlines.push_back({0, 1});
        const auto ops = canton::core::TextInputOverlayBuilder{}.build(9, input, state, service);
        bool sawCaret = false;
        bool sawSelection = false;
        for (const auto& op : ops) {
            if (op.type == canton::core::DrawOpType::DrawLine) sawCaret = true;
            if (op.type == canton::core::DrawOpType::FillRect && op.color == 0x663a7afeu) sawSelection = true;
        }
        if (!sawCaret || !sawSelection) return 63;
        return 0;
    }

    int verifyClippingAndOverflowOps() {
        using namespace canton::core;
        LayoutTree tree;
        tree.apply(std::vector<TreeMutation>{
            CreateNodeMutation{1, canton::core::NodeType::Layout},
            canton::core::SetPropMutation{1, "measurePolicy", canton::core::PropValue::objectValue({{"kind", canton::core::PropValue::stringValue("Box")}})},
            SetModifierMutation{1, size(80.0f, 40.0f)},
            CreateNodeMutation{2, canton::core::NodeType::Layout},
            canton::core::SetPropMutation{2, "measurePolicy", canton::core::PropValue::objectValue({{"kind", canton::core::PropValue::stringValue("Box")}})},
            SetModifierMutation{2, background(120.0f, 20.0f, 0xffabcdefu)},
            InsertChildMutation{1, 2, 0},
        });
        LayoutEngine{}.layout(tree, 1, {0.0f, 80.0f, 0.0f, 40.0f});
        const auto unclipped = DrawOpsBuilder{}.exportScene(tree, 1);
        for (const auto& op : unclipped) {
            if (op.type == DrawOpType::PushClip) return 111;
        }

        tree.apply(std::vector<TreeMutation>{
            SetModifierMutation{1, verticalScroll(80.0f, 40.0f, 0.0f, makeEventSlotId(1, EventSlotKind::VerticalScroll))},
        });
        LayoutEngine{}.layout(tree, 1, {0.0f, 80.0f, 0.0f, 40.0f});
        const auto scrollOps = DrawOpsBuilder{}.exportScene(tree, 1);
        bool sawClip = false;
        for (const auto& op : scrollOps) {
            if (op.type == DrawOpType::PushClip && near(op.rect.width, 80.0f) && near(op.rect.height, 40.0f)) sawClip = true;
        }
        if (!sawClip) return 112;

        tree = {};
        tree.apply(std::vector<TreeMutation>{
            CreateNodeMutation{1, canton::core::NodeType::Layout},
            canton::core::SetPropMutation{1, "measurePolicy", canton::core::PropValue::objectValue({{"kind", canton::core::PropValue::stringValue("MinSize")}})},
            SetModifierMutation{1,
                                [&] {
                                    auto chain = size(40, 12);
                                    auto text = test_support::text("overflow text");
                                    text.overflow = TextOverflow::Visible;
                                    chain.push_back({text, {}});
                                    return chain;
                                }()},
        });
        LayoutEngine{}.layout(tree, 1, {0.0f, 40.0f, 0.0f, 12.0f});
        const auto visibleOps = DrawOpsBuilder{}.exportScene(tree, 1);
        for (const auto& op : visibleOps) {
            if (op.type == DrawOpType::PushClip) return 113;
        }
        tree.apply(std::vector<TreeMutation>{
            SetModifierMutation{1,
                                [&] {
                                    auto chain = size(40, 12);
                                    chain.push_back({test_support::text("overflow text"), {}});
                                    return chain;
                                }()},
        });
        LayoutEngine{}.layout(tree, 1, {0.0f, 40.0f, 0.0f, 12.0f});
        const auto clipOps = DrawOpsBuilder{}.exportScene(tree, 1);
        sawClip = false;
        for (const auto& op : clipOps) {
            if (op.type == DrawOpType::DrawText && op.overflow == TextOverflow::Clip) sawClip = true;
        }
        if (!sawClip) return 114;

        tree = {};
        auto content = std::make_shared<PainterContent>();
        content->intrinsicSize = Size{24, 24};
        PaintModifier paint;
        paint.painter = {1, 1, 1, content};
        paint.tint = 0xff000000u;
        auto painted = size(24, 24);
        painted.push_back({paint, {}});
        tree.apply(std::vector<TreeMutation>{
            CreateNodeMutation{1, NodeType::Layout},
            SetModifierMutation{1, painted},
        });
        LayoutEngine{}.layout(tree, 1, {0.0f, 24.0f, 0.0f, 24.0f});
        const auto iconOps = DrawOpsBuilder{}.exportScene(tree, 1);
        bool sawResourceObjectIcon = false;
        for (const auto& op : iconOps) {
            if (op.type == DrawOpType::DrawPainter && op.painter.content == content && op.hasTint && op.color == 0xff000000u) sawResourceObjectIcon = true;
        }
        if (!sawResourceObjectIcon) return 115;
        return 0;
    }

    int verifyDevServerAndFramePlanner() {
        const auto endpoint = canton::runtime::parseDevServerUrl("http://127.0.0.1:9178");
        if (!endpoint.ok || endpoint.host != "127.0.0.1" || endpoint.port != 9178) return 81;
        if (canton::runtime::parseDevServerUrl("http://127.0.0.1:invalid").ok) return 82;

        canton::runtime::FramePlanner planner;
        if (planner.planTick({}).hasTickWork) return 84;
        planner.requestFramePipelineRun();
        if (!planner.planTick({}).runPipeline || !planner.hasPendingFrameWork({})) return 85;
        planner.clearFramePipelineRunRequest();
        if (!planner.planTick({true, false, false, false}).runEvents) return 86;
        if (!planner.planTick({false, false, true, false}).applyMutations) return 87;
        return 0;
    }

    int verifyScenePipelineStateIntentContract() {
        canton::runtime::ScenePipelineState pipelineState;
        pipelineState.enqueueIntent(canton::core::InputIntent::packageLoad(initialTreeTransaction()));
        auto result = pipelineState.run(1, {0.0f, 320.0f, 0.0f, 240.0f}, true);
        if (result.error || !pipelineState.scene().contains(1)) return 91;

        const auto initialRevision = pipelineState.publishedFrame().revision;
        pipelineState.enqueueIntent(canton::core::InputIntent::textInput());
        result = pipelineState.run(1, {0.0f, 320.0f, 0.0f, 240.0f}, false);
        if (result.error) return 92;
        if (result.ran || pipelineState.publishedFrame().revision != initialRevision) return 93;

        pipelineState.enqueueIntent(canton::core::InputIntent::resize());
        result = pipelineState.run(1, {0.0f, 640.0f, 0.0f, 480.0f}, false);
        if (result.error) return 94;
        if (!pipelineState.publishedFrame().invalidation.affects(canton::core::DirtyFlag::Layout)) return 95;

        const auto resizedRevision = pipelineState.publishedFrame().revision;
        const auto resizedHit = pipelineState.publishedFrame().content.hitTest;
        pipelineState.enqueueIntent(canton::core::InputIntent::pointer());
        result = pipelineState.run(1, {0.0f, 640.0f, 0.0f, 480.0f}, false);
        if (result.error) return 97;
        if (result.ran || pipelineState.publishedFrame().revision != resizedRevision || pipelineState.publishedFrame().content.hitTest != resizedHit) return 98;
        return 0;
    }
}  // namespace

int main() {
#define RUN_SMOKE(name)                                          \
    do {                                                         \
        if (const auto result = name(); result != 0) {           \
            std::cerr << #name << " failed: " << result << '\n'; \
            return result;                                       \
        }                                                        \
    } while (false)
    RUN_SMOKE(verifyTypedLayoutTreePipeline);
    RUN_SMOKE(verifyTypedDirtyPrecision);
    RUN_SMOKE(verifyPropSchema);
    RUN_SMOKE(verifyAlignmentAndArrangement);
    RUN_SMOKE(verifyNativeSceneAndFramePipeline);
    RUN_SMOKE(verifyPointerScrollAndHitTest);
    RUN_SMOKE(verifyPropValueAndTextInput);
    RUN_SMOKE(verifyTextLayoutAndOverlay);
    RUN_SMOKE(verifyClippingAndOverflowOps);
    RUN_SMOKE(verifyDevServerAndFramePlanner);
    RUN_SMOKE(verifyScenePipelineStateIntentContract);
#undef RUN_SMOKE
    if (canton::core::RuntimeVersion == 0) return 99;
    return 0;
}
