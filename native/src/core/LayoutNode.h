#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include "Geometry.h"
#include "Modifier.h"
#include "MeasurePolicy.h"

namespace canton::core {
    enum class NodeType { Root, Layout, Unknown };

    struct LazyLayoutState;
    struct PaintFragment;
    struct HitFragment;
    struct TextLayout;
    struct DrawOp;

    // Flow 测量得到的一行：成员、交叉轴尺寸，以及按线对齐的成员共用的那条线到行顶的距离（没有时为负）
    struct FlowLine {
        std::vector<NodeId> children;
        float cross = 0.0f;
        float alignedLine = -1.0f;
    };

    struct LayoutNode {
        NodeId id = 0;
        NodeType type = NodeType::Unknown;
        MeasurePolicy measurePolicy = MinSizeMeasurePolicy{};
        // 宿主输入 enabled；为 false 时整棵子树不参与命中与交互
        bool enabled = true;
        ModifierChain modifier;
        Rect contentBounds;
        std::uint64_t generation = 0;
        Rect bounds;
        // 首行与末行基线到自身顶边的距离，没有文本内容时为负
        float baseline = -1.0f;
        float lastBaseline = -1.0f;
        std::vector<NodeId> children;
        std::vector<FlowLine> flowLines;
        std::shared_ptr<LazyLayoutState> lazy;
        std::uint32_t dirty = 0;
        std::uint32_t subtreeDirty = 0;
        Constraints measuredConstraints;
        bool measurementValid = false;
        bool placementValid = false;
        std::shared_ptr<const PaintFragment> paintCache;
        std::shared_ptr<const PaintFragment> contentFragment;
        std::shared_ptr<const HitFragment> hitCache;
        // 写入日志已保存原值的轮次，同一帧内重复写入不再查表
        std::uint64_t journalEpoch = 0;
    };

    inline void markDirty(LayoutNode& node, DirtyFlag flag) noexcept {
        node.dirty |= dirtyMask(flag);
    }
}
