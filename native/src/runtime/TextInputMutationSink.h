#pragma once

#include <core/InputSession.h>

namespace canton::runtime {

    class CantonRuntime;

    class TextInputMutationSink final {
       public:
        [[nodiscard]] canton::core::InputCallbacks callbacks(CantonRuntime& runtime) const;
    };

}
