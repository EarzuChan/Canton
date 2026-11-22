#pragma once

#include <runtime/PackageRuntimeSource.h>
#include <string_view>

namespace canton::runtime {

    class CantonRuntime;
    class DiagnosticsState;
    class RuntimeSessionState;

    class RuntimePackageBinder final {
       public:
        void apply(PackageLoadOutcome outcome, RuntimeSessionState& session, CantonRuntime& runtime, DiagnosticsState& diagnostics) const;

       private:
        static constexpr std::string_view TAG = "RuntimePackageBinder";

        static void deliverToast(DiagnosticsState& diagnostics, CantonRuntime& runtime, RuntimeLoadToast toast);
    };

}
