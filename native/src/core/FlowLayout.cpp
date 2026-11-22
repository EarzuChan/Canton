#include <core/Layout.h>

#include <algorithm>
#include <cmath>

namespace canton::core {
    namespace {
        float mainSize(Size size, bool horizontal) {
            return horizontal ? size.width : size.height;
        }

        float crossSize(Size size, bool horizontal) {
            return horizontal ? size.height : size.width;
        }

        Size measuredSize(const LayoutNode& node) {
            return {node.bounds.width, node.bounds.height};
        }

        Constraints childConstraints(bool horizontal, float minMain, float maxMain, float minCross, float maxCross) {
            return horizontal ? Constraints{minMain, maxMain, minCross, maxCross} : Constraints{minCross, maxCross, minMain, maxMain};
        }

        // 权重项与交叉轴填充项的尺寸取决于所在行，分行时先以固有尺寸占位，行确定后再正式测量
        bool dependsOnLine(const ParentDataModifierSemantics& data) {
            return data.weight > 0 || data.fillCrossAxis;
        }

        ArrangementPlacement canton(float available, float used, std::size_t count, const AxisArrangement& arrangement) {
            auto result = distribute(arrangement.arrangement, std::max(0.0f, available - used), count);
            result.spacing += arrangement.spacing;
            return result;
        }
    }

    Size LayoutEngine::measureFlow(LayoutTree& tree, LayoutNode& node, Constraints constraints, const FlowMeasurePolicy& policy) {
        const auto horizontal = policy.horizontal;
        const auto maxMain = horizontal ? constraints.maxWidth : constraints.maxHeight;
        const auto maxCross = horizontal ? constraints.maxHeight : constraints.maxWidth;
        const auto spacing = policy.mainArrangement.spacing;
        const auto intrinsicMain = [&](NodeId child) { return intrinsic(tree, child, 0, horizontal, false, maxCross); };
        node.flowLines.clear();
        float lineMain = 0;
        for (auto childId : node.children) {
            const auto preferred = dependsOnLine(tree.node(childId).modifier.parentData()) ? intrinsicMain(childId) : mainSize(measure(tree, childId, childConstraints(horizontal, 0, maxMain, 0, maxCross)), horizontal);
            if (node.flowLines.empty() || !node.flowLines.back().children.empty() && (node.flowLines.back().children.size() >= static_cast<std::size_t>(policy.maxItems) || lineMain + spacing + preferred > maxMain)) {
                node.flowLines.emplace_back();
                lineMain = 0;
            }
            if (!node.flowLines.back().children.empty()) lineMain += spacing;
            node.flowLines.back().children.push_back(childId);
            lineMain += preferred;
        }
        float contentMain = 0, contentCross = 0;
        for (auto& line : node.flowLines) {
            const auto gaps = spacing * (line.children.size() - 1);
            // 填充项按行的交叉轴尺寸取比例，这个尺寸由已测量的项和待定项的固有尺寸定出
            float fillBasis = 0;
            bool fills = false;
            for (auto childId : line.children) {
                const auto& child = tree.node(childId);
                const auto data = child.modifier.parentData();
                fills |= data.fillCrossAxis.has_value();
                fillBasis = std::max(fillBasis, dependsOnLine(data) ? intrinsic(tree, childId, 0, !horizontal, false, intrinsicMain(childId)) : crossSize(measuredSize(child), horizontal));
            }
            const auto measureInLine = [&](NodeId childId, const ParentDataModifierSemantics& data, float minMain, float childMaxMain) {
                const auto cross = data.fillCrossAxis ? std::min(*data.fillCrossAxis * fillBasis, maxCross) : -1.0f;
                measure(tree, childId, childConstraints(horizontal, minMain, childMaxMain, std::max(cross, 0.0f), cross < 0 ? maxCross : cross));
            };
            float fixed = gaps, totalWeight = 0;
            for (auto childId : line.children) {
                const auto data = tree.node(childId).modifier.parentData();
                if (data.weight > 0) {
                    totalWeight += data.weight;
                    continue;
                }
                if (data.fillCrossAxis) measureInLine(childId, data, 0, maxMain);
                fixed += mainSize(measuredSize(tree.node(childId)), horizontal);
            }
            float used = gaps, beforeLine = -1, afterLine = 0;
            // 有填充项的行，交叉轴尺寸不小于取比例时用的那个尺寸
            line.cross = fills ? std::min(fillBasis, maxCross) : 0;
            for (auto childId : line.children) {
                const auto data = tree.node(childId).modifier.parentData();
                if (data.weight > 0) {
                    const auto share = std::isfinite(maxMain) ? std::max(0.0f, maxMain - fixed) * data.weight / totalWeight : Constraints::Infinity;
                    measureInLine(childId, data, data.weightFill && std::isfinite(share) ? share : 0, share);
                }
                const auto& child = tree.node(childId);
                used += mainSize(measuredSize(child), horizontal);
                line.cross = std::max(line.cross, crossSize(measuredSize(child), horizontal));
                if (const auto aligned = alignedLine(child, policy.itemAlignment)) {
                    beforeLine = std::max(beforeLine, *aligned);
                    afterLine = std::max(afterLine, child.bounds.height - *aligned);
                }
            }
            line.alignedLine = beforeLine;
            if (beforeLine >= 0) line.cross = std::max(line.cross, beforeLine + afterLine);
            contentMain = std::max(contentMain, used);
            contentCross += line.cross;
        }
        if (!node.flowLines.empty()) contentCross += policy.crossArrangement.spacing * (node.flowLines.size() - 1);
        return horizontal ? Size{contentMain, contentCross} : Size{contentCross, contentMain};
    }

    void LayoutEngine::placeFlow(LayoutTree& tree, LayoutNode& node, float x, float y, float width, float height, const FlowMeasurePolicy& policy) {
        const auto horizontal = policy.horizontal;
        const auto availableMain = horizontal ? width : height, availableCross = horizontal ? height : width;
        float totalCross = node.flowLines.empty() ? 0 : policy.crossArrangement.spacing * (node.flowLines.size() - 1);
        for (const auto& line : node.flowLines) totalCross += line.cross;
        const auto crossPlacement = canton(availableCross, totalCross, node.flowLines.size(), policy.crossArrangement);
        float crossCursor = crossPlacement.offset;
        for (const auto& line : node.flowLines) {
            float used = policy.mainArrangement.spacing * (line.children.size() - 1);
            for (auto childId : line.children) used += mainSize(measuredSize(tree.node(childId)), horizontal);
            const auto placement = canton(availableMain, used, line.children.size(), policy.mainArrangement);
            float mainCursor = placement.offset;
            for (auto childId : line.children) {
                const auto& child = tree.node(childId);
                const auto aligned = alignedLine(child, policy.itemAlignment);
                const auto crossOffset = crossCursor + (aligned ? line.alignedLine - *aligned : alignOffset(line.cross, crossSize(measuredSize(child), horizontal), child.modifier.parentData().align.value_or(policy.itemAlignment), !horizontal));
                place(tree, childId, x + (horizontal ? mainCursor : crossOffset), y + (horizontal ? crossOffset : mainCursor));
                mainCursor += mainSize(measuredSize(child), horizontal) + placement.spacing;
            }
            crossCursor += line.cross + crossPlacement.spacing;
        }
    }
}
