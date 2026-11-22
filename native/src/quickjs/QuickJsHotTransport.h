#pragma once

#include "QuickJsRuntimeContext.h"

namespace canton::quickjs {
    void installHotTransport(JSContext* context);
    JSValue hotMessageValue(JSContext* context, const HotMessage& message);
}
