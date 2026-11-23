#include <core/Layout.h>
#include <core/LazyLayout.h>
#include <core/Paint.h>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using namespace canton::core;

namespace {
    void check(bool condition, const std::string& message) {
        if (!condition) throw std::runtime_error(message);
    }

    bool near(float a, float b) {
        return std::abs(a - b) < 0.01f;
    }

    AnimationSpec linear(float duration) {
        AnimationSpec spec;
        spec.kind = AnimationKind::Tween;
        spec.durationMillis = duration;
        spec.bezier = {0, 0, 1, 1};
        return spec;
    }

    // 每项是一层包装（对应 LazyItem），内容根上写 animateItem；只按 key 取材料化的项
    struct Fixture {
        LayoutTree tree;
        LayoutEngine layout;
        DrawOpsBuilder builder;
        PaintWorkCounters counters;
        LazyMeasurePolicy policy;
        std::unordered_map<std::string, std::pair<NodeId, NodeId>> nodes;
        NodeId nextId = 2;
        AnimateItemModifier spec{linear(100), linear(100), linear(100)};

        explicit Fixture(int count) {
            tree.apply({CreateNodeMutation{1, NodeType::Layout}});
            std::vector<std::string> keys;
            for (int index = 0; index < count; ++index) keys.push_back("k" + std::to_string(index));
            dataset(std::move(keys));
            LayoutModifierSemantics scroll;
            scroll.kind = LayoutModifierKind::VerticalScroll;
            tree.setModifierChain(1, {{scroll}});
            layout.setMaterializer([&](NodeId, const std::vector<int>& indices) { materialize(indices); });
        }

        // 与 TS 一侧一致：按 key 复用项，数据集变化后按现有范围重新交付
        void materialize(const std::vector<int>& indices) {
            for (const auto child : std::vector<NodeId>(tree.node(1).children)) tree.apply({RemoveChildMutation{1, child}});
            std::unordered_map<std::string, std::pair<NodeId, NodeId>> selected;
            for (std::size_t position = 0; position < indices.size(); ++position) {
                const auto& key = policy.keys[indices[position]];
                auto found = nodes.find(key);
                std::pair<NodeId, NodeId> ids;
                if (found == nodes.end()) {
                    ids = {nextId, nextId + 1};
                    nextId += 2;
                    tree.apply({CreateNodeMutation{ids.first, NodeType::Layout}, CreateNodeMutation{ids.second, NodeType::Layout}, InsertChildMutation{ids.first, ids.second, 0}});
                    tree.mutableNode(ids.first).measurePolicy = BoxMeasurePolicy{};
                } else
                    ids = found->second;
                tree.apply({InsertChildMutation{1, ids.first, static_cast<std::uint32_t>(position)}});
                LayoutModifierSemantics size;
                size.kind = LayoutModifierKind::Size;
                size.width = 18;
                size.height = 10;
                tree.setModifierChain(ids.second, {{size}, {spec}});
                selected[key] = ids;
            }
            for (const auto& [key, ids] : nodes)
                if (!selected.contains(key)) tree.apply({DeleteNodeMutation{ids.first}});
            nodes = std::move(selected);
            policy.indices = indices;
            tree.mutableNode(1).measurePolicy = policy;
        }

        void dataset(std::vector<std::string> keys) {
            policy.keys = std::move(keys);
            policy.contentTypes = std::vector<std::string>(policy.keys.size());
            policy.spans = std::vector<int>(policy.keys.size(), 1);
            ++policy.version;
            std::vector<int> indices;
            for (int index : policy.indices)
                if (index < static_cast<int>(policy.keys.size())) indices.push_back(index);
            if (tree.contains(1)) materialize(indices);
        }

        void scrollTo(float value) {
            auto scroll = std::get<LayoutModifierSemantics>(tree.node(1).modifier.elements()[0].descriptor.value);
            scroll.scrollValue = value;
            tree.setModifierChain(1, {{scroll}});
        }

        void frame(double time) {
            tree.advanceAnimations(time);
            ++policy.workVersion;
            tree.mutableNode(1).measurePolicy = policy;
            tree.mutableNode(1).measurementValid = false;
            markDirty(tree.mutableNode(1), DirtyFlag::Layout);
            layout.layout(tree, 1, {0, 100, 0, 100});
            policy = std::get<LazyMeasurePolicy>(tree.node(1).measurePolicy);
            builder.build(tree, 1, counters);
            tree.clearDirty();
        }

        const LazyLayoutState& state() const {
            return *tree.node(1).lazy;
        }

        // 容器内容片段里某项的显示位置与透明度；残影没有节点，按片段身份找
        struct Shown {
            float y = -1;
            float alpha = 0;
        };

        Shown shown(const std::shared_ptr<const PaintFragment>& fragment) const {
            for (const auto& child : tree.node(1).contentFragment->children) {
                if (child.fragment == fragment) return {child.offset.y, 1};
                if (child.fragment->layer && child.fragment->children.size() == 1 && child.fragment->children.front().fragment == fragment) {
                    const auto& l = *child.fragment->layer;
                    return {child.offset.y, l.useOffscreenGroup ? l.groupAlpha : l.contentAlpha};
                }
            }
            return {};
        }

        Shown item(const std::string& key) const {
            if (!nodes.contains(key)) {
                std::string have;
                for (const auto& [name, _] : nodes) have += name + " ";
                throw std::runtime_error("没有材料化 " + key + "，现有 " + have);
            }
            return shown(tree.node(nodes.at(key).first).paintCache);
        }

        Shown ghost(const std::string& key) const {
            for (const auto& ghost : state().itemGhosts)
                if (ghost.key == key) return shown(ghost.fragment);
            return {};
        }
    };

    void firstFrameAndRemoval() {
        Fixture fixture(5);
        fixture.frame(0);
        check(fixture.tree.activeAnimationCount() == 0 && fixture.state().itemGhosts.empty(), "首帧不应有项动画");
        check(near(fixture.item("k2").y, 20) && near(fixture.item("k2").alpha, 1), "首帧位置错误");
        fixture.dataset({"k0", "k2", "k3", "k4"});
        fixture.frame(1000);
        check(near(fixture.item("k2").y, 20) && near(fixture.ghost("k1").y, 10) && near(fixture.ghost("k1").alpha, 1), "移除项的残影或后续项起点错误");
        check(fixture.tree.activeAnimationCount() > 0, "进行中的项动画没有计入活动动画");
        fixture.frame(1050);
        check(near(fixture.item("k2").y, 15) && near(fixture.ghost("k1").alpha, 0.5f), "移除动画中点错误：位移 " + std::to_string(fixture.item("k2").y) + "，残影 " + std::to_string(fixture.ghost("k1").alpha));
        fixture.frame(1100);
        check(near(fixture.item("k2").y, 10) && fixture.state().itemGhosts.empty() && fixture.tree.activeAnimationCount() == 0, "移除动画没有在结束时收尾");
    }

    void insertionAndScroll() {
        Fixture fixture(20);
        fixture.frame(0);
        fixture.dataset({"k0", "k1", "new", "k2", "k3", "k4", "k5", "k6", "k7", "k8", "k9", "k10", "k11"});
        fixture.frame(100);
        check(near(fixture.item("new").alpha, 0) && near(fixture.item("new").y, 20) && near(fixture.item("k2").y, 20), "插入项应从透明开始，后续项从原位出发");
        fixture.frame(150);
        check(near(fixture.item("new").alpha, 0.5f) && near(fixture.item("k2").y, 25), "插入动画中点错误");
        fixture.frame(200);
        check(fixture.tree.activeAnimationCount() == 0, "插入动画没有结束");
        // 滚动只改视口，不算位移；滚进视口的项也不淡入
        fixture.scrollTo(30);
        fixture.frame(300);
        check(fixture.tree.activeAnimationCount() == 0 && near(fixture.item("k11").alpha, 1), "滚动触发了项动画");
        // 锚点项之上插入：保持锚点位置，不算位移
        fixture.dataset({"k0", "top", "k1", "new", "k2", "k3", "k4", "k5", "k6", "k7", "k8", "k9", "k10", "k11"});
        fixture.frame(400);
        const auto& k5 = fixture.state().itemAnimations.at("k5");
        check(!k5.placement.running && near(fixture.item("k5").alpha, 1), "锚点之上的插入被当成了位移");
    }

    void disabledSpecs() {
        Fixture fixture(5);
        fixture.spec = {std::nullopt, linear(100), std::nullopt};
        fixture.frame(0);
        fixture.dataset({"k0", "new", "k2", "k3", "k4"});
        fixture.frame(100);
        check(near(fixture.item("new").alpha, 1) && fixture.state().itemGhosts.empty() && near(fixture.item("k2").y, 20), "关掉淡入淡出后不应有透明度动画");
    }

    void placementCheck() {
        LayoutTree tree;
        LayoutEngine layout;
        DrawOpsBuilder builder;
        PaintWorkCounters counters;
        tree.apply({CreateNodeMutation{1, NodeType::Layout}, CreateNodeMutation{2, NodeType::Layout}, CreateNodeMutation{3, NodeType::Layout}, InsertChildMutation{1, 2, 0}, InsertChildMutation{2, 3, 0}});
        tree.setModifierChain(3, {{AnimateItemModifier{}}});
        layout.layout(tree, 1, {0, 100, 0, 100});
        bool rejected = false;
        try {
            builder.build(tree, 1, counters);
        } catch (const std::invalid_argument& error) {
            rejected = std::string(error.what()).find("animateItem") != std::string::npos;
        }
        check(rejected, "Lazy 项之外的 animateItem 没有报错");
    }
}

int main() {
    const char* step = "首帧与移除";
    try {
        firstFrameAndRemoval();
        step = "插入与滚动";
        insertionAndScroll();
        step = "关掉的动画";
        disabledSpecs();
        step = "错误父级";
        placementCheck();
        std::cout << "Lazy 项的位移、淡入淡出与残影通过\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << step << "：" << error.what() << '\n';
        return 1;
    }
}
