#include "QuickJsEventRegistry.h"

#include <algorithm>
#include <stdexcept>

namespace canton::quickjs {
    void QuickJsEventRegistry::reset(JSContext* context) {
        commitCandidate();
        if (context_ != nullptr) {
            for (auto& [_, callback] : eventSlots_) JS_FreeValue(context_, callback);
        }
        eventSlots_.clear();
        retiredEventSlots_.clear();
        publishedEventSlots_.clear();
        publishedByWindow_.clear();
        context_ = context;
    }

    void QuickJsEventRegistry::beginCandidate() {
        if (checkpoint_) throw std::logic_error("事件注册已有未完成候选");
        checkpoint_ = CandidateCheckpoint{eventSlots_, retiredEventSlots_, publishedEventSlots_, publishedByWindow_};
        for (auto& [_, callback] : checkpoint_->callbacks) callback = JS_DupValue(context_, callback);
    }

    void QuickJsEventRegistry::commitCandidate() {
        if (!checkpoint_) return;
        for (auto& [_, callback] : checkpoint_->callbacks) JS_FreeValue(context_, callback);
        checkpoint_.reset();
    }

    void QuickJsEventRegistry::abortCandidate() {
        if (!checkpoint_) return;
        for (auto& [_, callback] : eventSlots_) JS_FreeValue(context_, callback);
        eventSlots_ = std::move(checkpoint_->callbacks);
        retiredEventSlots_ = std::move(checkpoint_->retired);
        publishedEventSlots_ = std::move(checkpoint_->published);
        publishedByWindow_ = std::move(checkpoint_->byWindow);
        checkpoint_.reset();
    }

    JSValueConst QuickJsEventRegistry::callback(const canton::core::EventSlotId& slot) const noexcept {
        if (!publishedEventSlots_.contains(slot)) return JS_UNDEFINED;
        const auto it = eventSlots_.find(slot);
        return it == eventSlots_.end() ? JS_UNDEFINED : it->second;
    }

    JSValueConst QuickJsEventRegistry::candidateCallback(const canton::core::EventSlotId& slot) const noexcept {
        const auto found = eventSlots_.find(slot);
        return found == eventSlots_.end() || retiredEventSlots_.contains(slot) ? JS_UNDEFINED : found->second;
    }

    canton::core::EventSlotId QuickJsEventRegistry::retain(canton::core::NodeId node, canton::core::EventSlotKind kind, JSValueConst callbackValue, canton::core::MutationTransaction* transaction) {
        // 资源不可变。新闭包获得新 token，旧画面不会提前调用替换后的闭包
        const canton::core::EventSlotId slot{node, kind, {}, canton::core::allocateRuntimeIdentity(), 1};
        eventSlots_.emplace(slot, JS_DupValue(context_, callbackValue));
        if (transaction) transaction->operations.emplace_back(canton::core::RegisterEventSlot{slot});
        return slot;
    }

    void QuickJsEventRegistry::release(const canton::core::EventSlotId& slot, canton::core::MutationTransaction* transaction) {
        releaseWithoutTreeWalk(slot, transaction);
    }

    void QuickJsEventRegistry::releaseWithoutTreeWalk(const canton::core::EventSlotId& slot, canton::core::MutationTransaction* transaction) {
        if (context_ == nullptr || !slot.valid()) return;
        if (transaction != nullptr) {
            transaction->operations.emplace_back(canton::core::RetireEventSlot{slot});
        }
        retiredEventSlots_.insert(slot);
    }

    void QuickJsEventRegistry::releaseNodes(const std::unordered_set<canton::core::NodeId>& nodes, canton::core::MutationTransaction* transaction) {
        for (const auto& [slot, _] : eventSlots_)
            if (nodes.contains(slot.node)) releaseWithoutTreeWalk(slot, transaction);
    }

    void QuickJsEventRegistry::releaseAll(canton::core::MutationTransaction* transaction) {
        if (transaction != nullptr) {
            for (const auto& [slot, _] : eventSlots_) {
                if (!slot.valid()) continue;
                transaction->operations.emplace_back(canton::core::RetireEventSlot{slot});
            }
        }
        for (const auto& [slot, _] : eventSlots_) retiredEventSlots_.insert(slot);
    }

    void QuickJsEventRegistry::rebuildPublishedUnion() {
        publishedEventSlots_.clear();
        for (const auto& [_, active] : publishedByWindow_) {
            publishedEventSlots_.insert(active.begin(), active.end());
        }
    }

    void QuickJsEventRegistry::sweepWindowSlots(canton::core::WindowId windowId, const std::unordered_map<canton::core::NodeId, canton::core::WindowId>& nodeWindows) {
        for (auto it = eventSlots_.begin(); it != eventSlots_.end();) {
            if (publishedEventSlots_.contains(it->first)) {
                ++it;
                continue;
            }
            const auto ownerIt = nodeWindows.find(it->first.node);
            const auto belongsToWindow = windowId == 0 ? (ownerIt == nodeWindows.end() || ownerIt->second == 0) : (ownerIt == nodeWindows.end() || ownerIt->second == windowId);
            if (!belongsToWindow && !retiredEventSlots_.contains(it->first)) {
                ++it;
                continue;
            }
            retiredEventSlots_.erase(it->first);
            JS_FreeValue(context_, it->second);
            it = eventSlots_.erase(it);
        }
    }

    void QuickJsEventRegistry::publish(const canton::core::EventSlotSet& active) {
        publishedByWindow_[0] = active;
        rebuildPublishedUnion();
        sweepWindowSlots(0, {});
        if (publishedByWindow_.size() <= 1) retiredEventSlots_.clear();
    }

    void QuickJsEventRegistry::publishWindow(canton::core::WindowId windowId, const canton::core::EventSlotSet& active, const std::unordered_map<canton::core::NodeId, canton::core::WindowId>& nodeWindows) {
        publishedByWindow_[windowId] = active;
        rebuildPublishedUnion();
        sweepWindowSlots(windowId, nodeWindows);
    }

    void QuickJsEventRegistry::unpublishWindow(canton::core::WindowId windowId, const std::unordered_map<canton::core::NodeId, canton::core::WindowId>& nodeWindows) {
        publishedByWindow_.erase(windowId);
        rebuildPublishedUnion();
        sweepWindowSlots(windowId, nodeWindows);
    }

    canton::core::EventSlotId QuickJsEventRegistry::updateModifierCallback(canton::core::NodeId node, canton::core::EventSlotKind kind, JSValueConst callbackValue, const canton::core::EventSlotId& previous, canton::core::MutationTransaction* transaction) {
        if (const auto found = eventSlots_.find(previous); found != eventSlots_.end() && !retiredEventSlots_.contains(previous) && JS_IsStrictEqual(context_, found->second, callbackValue)) return previous;
        const auto next = JS_IsFunction(context_, callbackValue) ? retain(node, kind, callbackValue, transaction) : canton::core::EventSlotId{};
        if (previous.valid()) release(previous, transaction);
        return next;
    }

    void QuickJsEventRegistry::releaseModifierCallbacksExcept(canton::core::NodeId node, const std::vector<canton::core::EventSlotId>& retained, canton::core::MutationTransaction* transaction) {
        for (const auto& [slot, _] : eventSlots_) {
            if (slot.node == node && !retiredEventSlots_.contains(slot) && std::find(retained.begin(), retained.end(), slot) == retained.end()) release(slot, transaction);
        }
    }
}
