#pragma once

#include <core/EventSlot.h>
#include <core/MutationTransaction.h>

#include <cstddef>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

extern "C" {
#include <quickjs.h>
}

namespace canton::quickjs {
    class QuickJsEventRegistry {
       public:
        void reset(JSContext* context);
        void beginCandidate();
        void commitCandidate();
        void abortCandidate();

        void clearContext() noexcept {
            context_ = nullptr;
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return eventSlots_.size();
        }

        [[nodiscard]] JSValueConst callback(const canton::core::EventSlotId& slot) const noexcept;
        // 仅候选准备阶段使用；调用者必须先确认候选场景仍引用此资源。
        [[nodiscard]] JSValueConst candidateCallback(const canton::core::EventSlotId& slot) const noexcept;

        void release(const canton::core::EventSlotId& slot, canton::core::MutationTransaction* transaction);
        void releaseNodes(const std::unordered_set<canton::core::NodeId>& nodes, canton::core::MutationTransaction* transaction);
        canton::core::EventSlotId updateModifierCallback(canton::core::NodeId node, canton::core::EventSlotKind kind, JSValueConst callback, const canton::core::EventSlotId& previous, canton::core::MutationTransaction* transaction);
        void releaseModifierCallbacksExcept(canton::core::NodeId node, const std::vector<canton::core::EventSlotId>& retained, canton::core::MutationTransaction* transaction);
        void releaseAll(canton::core::MutationTransaction* transaction);
        void publish(const canton::core::EventSlotSet& active);

       private:
        canton::core::EventSlotId retain(canton::core::NodeId node, canton::core::EventSlotKind kind, JSValueConst callback, canton::core::MutationTransaction* transaction);
        void releaseWithoutTreeWalk(const canton::core::EventSlotId& slot, canton::core::MutationTransaction* transaction);

        JSContext* context_ = nullptr;
        std::unordered_map<canton::core::EventSlotId, JSValue, canton::core::EventSlotIdHash> eventSlots_;
        canton::core::EventSlotSet retiredEventSlots_;
        canton::core::EventSlotSet publishedEventSlots_;

        struct CandidateCheckpoint {
            decltype(eventSlots_) callbacks;
            decltype(retiredEventSlots_) retired;
            decltype(publishedEventSlots_) published;
        };

        std::optional<CandidateCheckpoint> checkpoint_;
    };
}

