#pragma once

#include <core/SceneFramePipeline.h>
#include <runtime/DiagnosticsTypes.h>

#include <filesystem>
#include <string_view>

namespace canton::runtime {

    class CantonRuntime;
    class DiagnosticsState;
    class RuntimeSessionState;

    class FramePumpDriver final {
       public:
        [[nodiscard]] bool pumpFrame(CantonRuntime& runtime, RuntimeSessionState& session, DiagnosticsState& diagnostics, canton::core::NodeId root, const std::filesystem::path& frameErrorPath, core::Rect diagnosticsBounds, bool detailedErrorScreen, const DiagnosticsBadgeModel& badgeModel, const canton::core::TextLayoutService& textLayout, double nowMillis, const core::FrameFinalizer& finalizeContent = {}) const;

       private:
        static constexpr std::string_view TAG = "FramePumpDriver";
        [[nodiscard]] static bool tickDiagnostics(DiagnosticsState& diagnostics, CantonRuntime& runtime, double nowMillis);
    };

}
