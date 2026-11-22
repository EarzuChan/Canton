#pragma once

#include <core/EventSlot.h>
#include <core/InputEvent.h>
#include <core/LayoutNode.h>
#include <core/Scroll.h>

#include <quickjs/QuickJsScriptHost.h>

#include <cstdint>
#include <string>

namespace canton::runtime {

    struct ScriptEventInvokeResult {
        bool invoked = false;
        bool ok = true;
        std::string error;
        bool consumed = false;
    };

    class ScriptEventDispatcher final {
       public:
        ScriptEventInvokeResult invoke(canton::quickjs::QuickJsScriptHost* host, const canton::core::EventSlotId& slot, double nowMillis) const;
        ScriptEventInvokeResult invoke(canton::quickjs::QuickJsScriptHost* host, const canton::core::EventSlotId& slot, double nowMillis, const canton::quickjs::CallbackInvokeOptions& options) const;
        ScriptEventInvokeResult invokeString(canton::quickjs::QuickJsScriptHost* host, const canton::core::EventSlotId& slot, double nowMillis, const std::string& value) const;
        ScriptEventInvokeResult invokeScroll(canton::quickjs::QuickJsScriptHost* host, const canton::core::EventSlotId& slot, double nowMillis, const canton::core::ScrollResult& result) const;
        ScriptEventInvokeResult invokeInput(canton::quickjs::QuickJsScriptHost* host, const canton::core::EventSlotId& slot, double nowMillis, const canton::core::InputEventPayload& payload) const;
    };

}
