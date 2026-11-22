#include <core/Clock.h>
#include <runtime/TextInputMutationSink.h>


#include <core/InputIntent.h>
#include <core/MutationTransaction.h>
#include <runtime/CantonRuntime.h>

#include <utility>

namespace canton::runtime {
    canton::core::InputCallbacks TextInputMutationSink::callbacks(CantonRuntime& runtime) const {
        canton::core::InputCallbacks result;
        result.invokeStringEvent = [&runtime](const canton::core::EventSlotId& slot, const std::string& value) {
            if (slot.valid()) runtime.enqueueStringEvent(slot, value);
        };
        result.invokeFocusEvent = [&runtime](const auto& slot, bool focused, bool hasFocus) {
            if (slot.valid()) runtime.enqueueFocusEvent(slot, focused, hasFocus);
        };
        result.invokeEvent = [&runtime](const auto& slot) {
            if (slot.valid()) runtime.enqueueEvent(slot);
        };
        result.invokeScrollEvent = [&runtime](const auto& slot, const auto& scroll) {
            if (slot.valid()) runtime.enqueueScrollSnapshotEvent(slot, scroll);
        };
        result.dispatchKeyEvent = [&runtime](const canton::core::EventSlotId& slot, const canton::core::KeyEventPayload& payload) {
            return runtime.dispatchInputEventNow(slot, payload, core::monotonicMillis());
        };
        result.invalidateNativeState = [&runtime](canton::core::NodeId nodeId, canton::core::DirtyFlag flag) {
            canton::core::MutationTransaction transaction;
            transaction.operations.emplace_back(canton::core::NativeInvalidationMutation{nodeId, flag});
            runtime.enqueueIntent(canton::core::InputIntent::jsCommit(std::move(transaction)));
        };
        result.enqueueKeyIntent = [&runtime](canton::core::NodeId) {
            runtime.enqueueIntent(canton::core::InputIntent::key());
        };
        result.enqueueTextInputIntent = [&runtime](canton::core::NodeId) {
            runtime.enqueueIntent(canton::core::InputIntent::textInput());
        };
        result.enqueueImeCompositionIntent = [&runtime](canton::core::NodeId) {
            runtime.enqueueIntent(canton::core::InputIntent::imeComposition());
        };
        return result;
    }
}

