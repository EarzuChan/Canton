#pragma once

#include <core/Paint.h>
#include <runtime/DiagnosticsState.h>
#include <runtime/DiagnosticsTypes.h>
#include <runtime/ErrorScreenModel.h>

#include <memory>
#include <vector>

namespace canton::runtime {

    class DiagnosticsScene final {
       public:
        [[nodiscard]] std::vector<canton::core::DrawOp> buildErrorScreen(core::Rect editorBounds, const ErrorScreenModel& error, bool detailed, float dpiScale = 1.0f) const;
        [[nodiscard]] std::vector<canton::core::DrawOp> buildBadge(core::Rect editorBounds, const DiagnosticsBadgeModel& model, DiagnosticVisibility visibility, const canton::core::TextLayoutService& textLayout, float dpiScale = 1.0f) const;
        [[nodiscard]] std::vector<canton::core::DrawOp> buildFpsBadge(core::Rect editorBounds, int fps, const canton::core::TextLayoutService& textLayout, float dpiScale = 1.0f) const;
        [[nodiscard]] std::vector<canton::core::DrawOp> buildToasts(core::Rect editorBounds, const std::vector<DiagnosticsToastModel>& toasts, DiagnosticVisibility visibility, float dpiScale = 1.0f) const;

       private:
        static canton::core::DrawOp fill(canton::core::Rect rect, std::uint32_t color, float radius = 0.0f);
        static canton::core::DrawOp stroke(canton::core::Rect rect, std::uint32_t color, float width = 1.0f, float radius = 0.0f);
        static canton::core::DrawOp badgeText(canton::core::Rect area, std::string value, const canton::core::TextLayoutService& textLayout, float dpiScale = 1.0f);
        static canton::core::DrawOp text(canton::core::Rect rect, std::string value, std::uint32_t color, float fontSize, int maxLines = 1);
    };

    [[nodiscard]] std::unique_ptr<DiagnosticsHud> createDiagnosticsHud(DiagnosticsConfig config = {});

}
