#pragma once

#include <core/Paint.h>
#include <runtime/DiagnosticsTypes.h>
#include <runtime/DiagnosticsToast.h>
#include <runtime/DiagnosticsModel.h>
#include <runtime/DiagnosticsScene.h>
#include <runtime/ErrorScreenModel.h>

#include <optional>
#include <string>
#include <vector>

namespace canton::runtime {

    class DiagnosticsState final {
       public:
        void configure(DiagnosticsConfig config);

        bool hasError() const noexcept;
        const ErrorScreenModel* error() const noexcept;
        bool errorRetryAvailable() const noexcept;
        void setError(ErrorScreenModel error);
        void clearError() noexcept;

        bool addToast(core::LogLevel level, std::string title, std::string message, double nowMillis, bool coalesce = true);
        bool tick(double nowMillis);
        bool hasActiveToasts() const noexcept;
        [[nodiscard]] std::vector<DiagnosticsToastModel> activeToastModels() const;
        [[nodiscard]] DiagnosticVisibility badgeVisibility() const noexcept;
        [[nodiscard]] DiagnosticVisibility toastVisibility() const noexcept;
        void setToastsEnabled(bool enabled) noexcept;
        void invalidatePreparedFrame() noexcept;
        void setFps(std::optional<int> fps) noexcept { fps_ = fps; }
        [[nodiscard]] bool prepareFrame(core::Rect bounds, bool detailedErrorScreen, const DiagnosticsBadgeModel& badgeModel, const canton::core::TextLayoutService& textLayout);
        [[nodiscard]] std::vector<canton::core::DrawOp> fpsOpsSnapshot() const { return preparedFpsOps_; }

        [[nodiscard]] std::vector<canton::core::DrawOp> errorOpsSnapshot() const {
            return preparedErrorOps_;
        }

        [[nodiscard]] std::vector<canton::core::DrawOp> badgeOpsSnapshot() const {
            return preparedBadgeOps_;
        }

        [[nodiscard]] std::vector<canton::core::DrawOp> toastOpsSnapshot() const {
            return preparedToastOps_;
        }

        std::string diagnosticsText(DiagnosticsTextContext context) const;
        static std::string currentLocalTimeLabel();

       private:
        DiagnosticsModel model_;
        DiagnosticsScene diagnosticsScene_;
        std::optional<ErrorScreenModel> error_;
        std::optional<int> fps_;
        std::optional<int> preparedFps_;
        core::Rect preparedFpsBounds_{};
        std::vector<canton::core::DrawOp> preparedErrorOps_;
        std::vector<canton::core::DrawOp> preparedBadgeOps_;
        std::vector<canton::core::DrawOp> preparedFpsOps_;
        std::vector<canton::core::DrawOp> preparedToastOps_;
        std::string preparedSignature_;
    };

}
