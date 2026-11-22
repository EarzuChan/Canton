#pragma once

#include <runtime/DiagnosticsTypes.h>

#include <string>
#include <vector>

namespace canton::runtime {

    class DiagnosticsModel final {
       public:
        void configure(DiagnosticsConfig config);

        bool tick(double nowMillis);
        bool hasActiveToasts() const noexcept;
        [[nodiscard]] std::vector<DiagnosticsToastModel> activeToastModels() const;

        [[nodiscard]] DiagnosticVisibility badgeVisibility() const noexcept {
            return config_.badge;
        }

        [[nodiscard]] DiagnosticVisibility toastVisibility() const noexcept {
            return config_.toasts;
        }

        void setToastsEnabled(bool enabled) noexcept;
        bool addToast(core::LogLevel level, std::string title, std::string message, double nowMillis, bool coalesce);

        std::string diagnosticsText(const DiagnosticsTextContext& context) const;

        static std::string currentLocalTimeLabel();

        static bool visibilityEnabled(DiagnosticVisibility visibility) noexcept {
            return diagnosticVisibilityEnabled(visibility);
        }

       private:
        struct Toast {
            core::LogLevel level = core::LogLevel::Info;
            std::string title;
            std::string message;
            double expiresAtMs = 0.0;
        };

        DiagnosticsConfig config_;
        std::vector<Toast> toasts_;
    };

}
