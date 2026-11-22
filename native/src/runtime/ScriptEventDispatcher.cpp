#include <runtime/ScriptEventDispatcher.h>

namespace canton::runtime {

    ScriptEventInvokeResult ScriptEventDispatcher::invoke(canton::quickjs::QuickJsScriptHost* host, const canton::core::EventSlotId& slot, double nowMillis) const {
        if (host == nullptr || !slot.valid()) return {};
        host->setFrameTimeMillis(nowMillis);
        const auto invoked = host->invokeEventSlot(slot);
        if (!invoked.ok) return {true, false, invoked.error};
        return {true, true, {}};
    }

    ScriptEventInvokeResult ScriptEventDispatcher::invoke(canton::quickjs::QuickJsScriptHost* host, const canton::core::EventSlotId& slot, double nowMillis, const canton::quickjs::CallbackInvokeOptions& options) const {
        if (host == nullptr || !slot.valid()) return {};
        host->setFrameTimeMillis(nowMillis);
        const auto invoked = host->invokeEventSlot(slot, options);
        if (!invoked.ok) return {true, false, invoked.error};
        return {true, true, {}};
    }

    ScriptEventInvokeResult ScriptEventDispatcher::invokeString(canton::quickjs::QuickJsScriptHost* host, const canton::core::EventSlotId& slot, double nowMillis, const std::string& value) const {
        canton::quickjs::CallbackInvokeOptions options;
        options.hasStringArgument = true;
        options.stringArgument = value;
        return invoke(host, slot, nowMillis, options);
    }

    ScriptEventInvokeResult ScriptEventDispatcher::invokeScroll(canton::quickjs::QuickJsScriptHost* host, const canton::core::EventSlotId& slot, double nowMillis, const canton::core::ScrollResult& result) const {
        if (host == nullptr || !slot.valid()) return {};
        host->setFrameTimeMillis(nowMillis);
        const auto invoked = host->invokeEventSlot(slot, result);
        if (!invoked.ok) return {true, false, invoked.error};
        return {true, true, {}};
    }

    ScriptEventInvokeResult ScriptEventDispatcher::invokeInput(canton::quickjs::QuickJsScriptHost* host, const canton::core::EventSlotId& slot, double nowMillis, const canton::core::InputEventPayload& payload) const {
        if (host == nullptr || !slot.valid()) return {};
        host->setFrameTimeMillis(nowMillis);
        const auto invoked = host->invokeEventSlot(slot, payload);
        if (!invoked.ok) return {true, false, invoked.error};
        return {true, true, {}, invoked.consumed};
    }
}

