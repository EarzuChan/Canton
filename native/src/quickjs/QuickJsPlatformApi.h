#pragma once

#include <core/PlatformTypes.h>

extern "C" {
#include <quickjs.h>
}

namespace canton::quickjs {
    struct QuickJsRuntimeContext;

    class QuickJsPlatformApi final {
       public:
        static void install(JSContext* context, JSValue nativeObj, JSValue globalObj, QuickJsRuntimeContext& runtime);
        static JSValue makeGraphicsEnvObject(JSContext* context, const QuickJsRuntimeContext& runtime);
        static JSValue makeLaunchRequestObject(JSContext* context, const canton::core::LaunchRequest& request);
        static JSValue makeTrayAnchorObject(JSContext* context, const canton::core::TrayAnchorRect& anchor);
    };
}
