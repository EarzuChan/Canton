#pragma once

#include "Modifier.h"
#include "LayoutTree.h"

namespace canton::core {
    std::vector<NodeId> nodePath(const LayoutTree& tree, NodeId node);
    bool nodeInteractionEnabled(const LayoutTree& tree, NodeId node);
    Point rootToNodeContent(const LayoutTree& tree, NodeId node, Point point, ModifierHandle receiver = {});
    Point nodeContentToRoot(const LayoutTree& tree, NodeId node, Point point, ModifierHandle receiver = {});
    Rect nodeContentRectToRoot(const LayoutTree& tree, NodeId node, Rect rect, ModifierHandle receiver = {});
    bool containsRect(Rect bounds, Point point) noexcept;
    bool containsShape(Rect bounds, const PaintStyleSemantics& shape, Point point) noexcept;
    Point inverseLayerPoint(Point point, Rect bounds, const TransformModifierSemantics& transform) noexcept;
    // 绘制和命中使用同一层 bounds；坐标逆变换只在 graphicsLayer 所在层进行
    bool enterModifier(const ModifierInstance& instance, Point& point) noexcept;

    struct PositionedUpdate {
        NodeId node = 0;
        std::size_t modifierIndex = 0;
        EventSlotId callback;
        PositionedGeometry geometry;
    };

    // 自顶向下采集本帧发生几何变更的 onGloballyPositioned 修饰器并写入可回滚日志
    std::vector<PositionedUpdate> collectPositionedUpdates(LayoutTree& tree, NodeId root, Size viewport);
}
