#pragma once

extern "C" {
#include <quickjs.h>
}

namespace canton::quickjs {
    struct QuickJsRuntimeContext;

    class QuickJsNativeApi {
       public:
        static void install(JSContext* context, QuickJsRuntimeContext& runtime);
    };
}

