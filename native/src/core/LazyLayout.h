#pragma once

#include "Animation.h"
#include "Geometry.h"
#include "ImmutableVector.h"
#include "LazyGeometryIndex.h"
#include "Modifier.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace canton::core {
    struct PaintFragment;

    // 一个 Lazy 项上次绘制时的状态。坐标是扣除布局补偿后的内容坐标，滚动与锚点修正都不算位移
    struct LazyItemAnimation {
        AnimateItemModifier spec;
        Point position;
        std::shared_ptr<const PaintFragment> fragment;
        // 相对目标位置的偏移，衰减到 0
        VectorAnimation<2> placement;
        VectorAnimation<1> alpha;
    };

    // 已从数据集移除、正在淡出的项：保留最后一次的片段和位置
    struct LazyItemGhost {
        std::string key;
        Point position;
        std::shared_ptr<const PaintFragment> fragment;
        VectorAnimation<1> alpha;
    };

    class LazyKeyIndex {
       public:
        using Values = std::unordered_map<std::string, int>;

        LazyKeyIndex() : values_(std::make_shared<const Values>()) {}

        LazyKeyIndex& operator=(Values values) {
            values_ = std::make_shared<const Values>(std::move(values));
            return *this;
        }

        auto find(const std::string& key) const {
            return values_->find(key);
        }

        auto end() const {
            return values_->end();
        }

       private:
        std::shared_ptr<const Values> values_;
    };

    struct LazyVisibleItem {
        int index = 0;
        float offset = 0;
        float size = 0;
        int span = 1;
        bool operator==(const LazyVisibleItem&) const = default;
    };

    struct LazyScrollSnapshot {
        int firstVisibleItemIndex = 0;
        float firstVisibleItemScrollOffset = 0;
        int totalItemsCount = 0;
        bool needsMoreItems = false;
        std::uint64_t workGeneration = 0;
        std::vector<LazyVisibleItem> visibleItems;
        bool operator==(const LazyScrollSnapshot&) const = default;
    };

    // 候选树复制时按需复制，失败测量不会改写已发布的索引或缓存
    struct LazyLayoutState {
        struct Measurement {
            float main = 0;
            std::string contentType;
            std::uint64_t used = 0;
            int line = 0;
            float crossConstraint = 0;
        };

        std::uint64_t version = 0;
        std::uint64_t requestVersion = 0;
        std::uint64_t clock = 0;
        std::uint64_t workGeneration = 0;
        std::uint64_t measurementCacheHits = 0;
        int cells = 1;
        float crossSize = -1;
        float estimate = 48;
        float spacing = 0;
        float crossSpacing = 0;
        float contentSize = 0;
        float viewportSize = 0;
        // 扣除内容边距后的视口，供项内按视口取比例的 Modifier 读取
        Size itemViewport;
        float scrollOffset = 0;
        float anchorOffset = 0;
        std::string anchorKey;
        std::vector<std::string> pinnedKeys;
        std::string focusRequestKey;
        std::uint64_t focusRequestVersion = 0;
        std::uint64_t consumedFocusRequestVersion = 0;
        ImmutableVector<std::string> keys;
        ImmutableVector<std::string> contentTypes;
        LazyKeyIndex keyToIndex;
        LazyGeometryIndex itemMainSizes;
        ImmutableVector<int> itemLines;
        ImmutableVector<int> itemSlots;
        ImmutableVector<int> itemSpans;
        ImmutableVector<std::vector<int>> lines;
        LazyGeometryIndex prefixDeltas;
        LazyGeometryIndex lineMainSizes;
        std::unordered_map<std::string, Measurement> measurements;
        LazyScrollSnapshot snapshot;
        LazyScrollSnapshot publishedSnapshot;
        // 布局为保持锚点而改的滚动量累计：内容坐标减去它，数据变化前后锚点项的位置不变
        double placementCompensation = 0;
        // animateItem：上次绘制时的数据集与各项状态
        bool itemsPainted = false;
        LazyKeyIndex paintedKeys;
        std::unordered_map<std::string, LazyItemAnimation> itemAnimations;
        std::vector<LazyItemGhost> itemGhosts;

        [[nodiscard]] std::size_t runningItemAnimations() const noexcept {
            std::size_t count = itemGhosts.size();
            for (const auto& [_, item] : itemAnimations)
                if (item.placement.running || item.alpha.running) ++count;
            return count;
        }
    };
}
