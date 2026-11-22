#include <runtime/DiagnosticsState.h>

#include <core/InputIntent.h>
#include <core/TextLayoutService.h>
#include <runtime/CantonRuntime.h>

#include <algorithm>
#include <sstream>
#include <utility>

namespace canton::runtime {
    namespace {
        void replacePreparedOps(std::vector<canton::core::DrawOp>& target, std::vector<canton::core::DrawOp> source) {
            for (std::size_t index = 0; index < std::min(target.size(), source.size()); ++index) {
                auto& next = source[index];
                const auto& previous = target[index];
                if (next.type == canton::core::DrawOpType::DrawText && previous.type == canton::core::DrawOpType::DrawText && !next.textLayout && next.text == previous.text && next.textStyle == previous.textStyle && next.rect.width == previous.rect.width && next.maxLines == previous.maxLines && next.overflow == previous.overflow) next.textLayout = previous.textLayout;
            }
            target = std::move(source);
        }

        std::string errorFrameSignature(core::Rect bounds, const ErrorScreenModel* error, bool detailedErrorScreen, std::uint64_t environment) {
            std::ostringstream out;
            out << bounds.x << ',' << bounds.y << ',' << bounds.width << ',' << bounds.height << "|detail=" << (detailedErrorScreen ? 1 : 0) << "|font=" << environment;
            if (error) out << "|error=" << (detailedErrorScreen ? error->diagnosticText() : error->summary);
            return out.str();
        }
    }

    void DiagnosticsState::setHud(std::unique_ptr<DiagnosticsHud> hud) noexcept {
        hud_ = std::move(hud);
        invalidatePreparedFrame();
    }

    bool DiagnosticsState::hasError() const noexcept {
        return error_.has_value();
    }

    const ErrorScreenModel* DiagnosticsState::error() const noexcept {
        return error_ ? &*error_ : nullptr;
    }

    bool DiagnosticsState::errorRetryAvailable() const noexcept {
        return error_ && error_->retryAvailable;
    }

    void DiagnosticsState::setError(ErrorScreenModel error) {
        error_ = std::move(error);
        invalidatePreparedFrame();
    }

    void DiagnosticsState::clearError() noexcept {
        error_.reset();
        invalidatePreparedFrame();
    }

    bool DiagnosticsState::notify(core::LogLevel level, std::string_view tag, std::string title, std::string message, bool coalesce) {
        if (hud_) return hud_->showToast(level, tag, std::move(title), std::move(message), coalesce);
        core::Log::write(level, tag, message.empty() ? title : title + "：" + message);
        return false;
    }

    bool DiagnosticsState::tick(CantonRuntime& runtime, double nowMillis) {
        if (hud_) {
            if (!hud_->tick(runtime, nowMillis)) return false;
            runtime.enqueueIntent(canton::core::InputIntent::diagnostics());
            return true;
        }
        (void)runtime.takeDiagnosticToasts();
        for (const auto& action : runtime.takeDiagnosticActions())
            if (action.kind == canton::quickjs::QuickJsDiagnosticActionKind::RequestReload) runtime.requestReload();
        return false;
    }

    bool DiagnosticsState::hasActiveToasts() const noexcept {
        return hud_ && hud_->hasActiveToasts();
    }

    bool DiagnosticsState::fpsEnabled() const noexcept {
        return hud_ && hud_->fpsEnabled();
    }

    void DiagnosticsState::recordFrame(bool changed, bool ready, double nowMillis, CantonRuntime& runtime) {
        if (hud_) hud_->recordFrame(changed, ready, nowMillis, runtime);
    }

    void DiagnosticsState::invalidatePreparedFrame() noexcept {
        preparedSignature_.clear();
        preparedErrorOps_.clear();
    }

    void DiagnosticsState::prepareFrame(core::Rect bounds, bool detailedErrorScreen, PackageSource activeSource, bool liveUnavailable, const canton::core::TextLayoutService& textLayout, core::PublishedFrameContent& content) {
        const auto signature = errorFrameSignature(bounds, error(), detailedErrorScreen, textLayout.environment());
        if (signature != preparedSignature_) {
            preparedErrorOps_ = error_ ? buildErrorScreenOps(bounds, *error_, detailedErrorScreen) : std::vector<canton::core::DrawOp>{};
            preparedSignature_ = signature;
        }
        replacePreparedOps(content.diagnosticsErrorDrawOps, preparedErrorOps_);
        if (hud_) {
            hud_->prepare(bounds, hasError(), activeSource, liveUnavailable, textLayout, content);
            return;
        }
        content.diagnosticsBadgeDrawOps.clear();
        content.diagnosticsFpsDrawOps.clear();
        content.diagnosticsToastDrawOps.clear();
    }
}

