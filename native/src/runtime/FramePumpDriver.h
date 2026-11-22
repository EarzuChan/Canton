#pragma once

#include <core/SceneFramePipeline.h>
#include <runtime/LoadPolicy.h>

#include <filesystem>

namespace canton::runtime {

    class CantonRuntime;
    class DiagnosticsState;
    class RuntimeSessionState;

    class FramePumpDriver final {
       public:
        [[nodiscard]] bool pumpFrame(CantonRuntime& runtime, RuntimeSessionState& session, DiagnosticsState& diagnostics, canton::core::NodeId root, const std::filesystem::path& frameErrorPath, core::Rect diagnosticsBounds, bool detailedErrorScreen, PackageSource activeSource, bool liveUnavailable, const canton::core::TextLayoutService& textLayout, double nowMillis, const core::FrameFinalizer& finalizeContent = {}) const;
    };

}
