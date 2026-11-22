#include <runtime/DiagnosticsScene.h>

#include <core/InputIntent.h>
#include <core/TextLayoutService.h>
#include <runtime/CantonRuntime.h>
#include <runtime/DiagnosticsModel.h>
#include <runtime/DiagnosticsToast.h>
#include <runtime/SceneHost.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <sstream>
#include <utility>

namespace canton::runtime {
    namespace {
        constexpr std::string_view HUD_TAG = "DiagnosticsHud";

        std::uint32_t accentColour(core::LogLevel level) noexcept {
            switch (level) {
                case core::LogLevel::Verbose:
                case core::LogLevel::Debug:
                    return 0xff94a3b8u;
                case core::LogLevel::Info:
                    return 0xff3b82f6u;
                case core::LogLevel::Warn:
                    return 0xfff59e0bu;
                case core::LogLevel::Error:
                    return 0xffef4444u;
            }
            return 0xff3b82f6u;
        }

        DiagnosticsBadgeModel sourceBadgeModel(PackageSource source, bool liveUnavailable) {
            switch (source) {
                case PackageSource::Live:
                    return {"LIVE", 0xff22c55eu};
                case PackageSource::Dist:
                    return liveUnavailable ? DiagnosticsBadgeModel{"DIST FALLBACK", 0xfff59e0bu} : DiagnosticsBadgeModel{"DIST", 0xff3b82f6u};
                case PackageSource::None:
                    break;
            }
            return {"NO APP", 0xffef4444u};
        }

        void replaceHudOps(std::vector<canton::core::DrawOp>& target, std::vector<canton::core::DrawOp> source) {
            for (std::size_t index = 0; index < std::min(target.size(), source.size()); ++index) {
                auto& next = source[index];
                const auto& previous = target[index];
                if (next.type == canton::core::DrawOpType::DrawText && previous.type == canton::core::DrawOpType::DrawText && !next.textLayout && next.text == previous.text && next.textStyle == previous.textStyle && next.rect.width == previous.rect.width && next.maxLines == previous.maxLines && next.overflow == previous.overflow) next.textLayout = previous.textLayout;
            }
            target = std::move(source);
        }
    }

    std::vector<canton::core::DrawOp> DiagnosticsScene::buildErrorScreen(core::Rect editorBounds, const ErrorScreenModel& error, bool detailed, float dpiScale) const {
        return buildErrorScreenOps(editorBounds, error, detailed, dpiScale);
    }

    std::vector<canton::core::DrawOp> DiagnosticsScene::buildBadge(core::Rect editorBounds, const DiagnosticsBadgeModel& model, DiagnosticVisibility visibility, const canton::core::TextLayoutService& textLayout, float dpiScale) const {
        std::vector<canton::core::DrawOp> ops;
        if (!diagnosticVisibilityEnabled(visibility) || model.text.empty()) return ops;

        const float s = dpiScale > 0.0f ? dpiScale : 1.0f;
        const float textWidth = std::round((static_cast<float>(model.text.size()) * 7.0f + 26.0f) * s);
        const float x = std::round(std::max(editorBounds.x + 8.0f * s, editorBounds.x + editorBounds.width - 8.0f * s - textWidth));
        const float y = editorBounds.y + 20.0f * s;
        const canton::core::Rect badge{x, y, textWidth, 20.0f * s};
        ops.push_back(fill(badge, 0xcc111827u, 6.0f * s));
        ops.push_back(stroke(badge, 0x664b5563u, 1.0f * s, 6.0f * s));
        ops.push_back(fill({badge.x + 8.0f * s, badge.y + 7.0f * s, 6.0f * s, 6.0f * s}, model.dot, 3.0f * s));
        ops.push_back(badgeText({badge.x + 20.0f * s, badge.y, badge.width - 20.0f * s, badge.height}, model.text, textLayout, s));
        return ops;
    }

    std::vector<canton::core::DrawOp> DiagnosticsScene::buildFpsBadge(core::Rect editorBounds, int fps, const canton::core::TextLayoutService& textLayout, float dpiScale) const {
        const float s = dpiScale > 0.0f ? dpiScale : 1.0f;
        const canton::core::Rect badge{editorBounds.x + 8.0f * s, editorBounds.y + 20.0f * s, 78.0f * s, 20.0f * s};
        return {
            fill(badge, 0xcc111827u, 6.0f * s),
            stroke(badge, 0x664b5563u, 1.0f * s, 6.0f * s),
            badgeText({badge.x + 8.0f * s, badge.y, badge.width - 16.0f * s, badge.height}, fps == 0 ? "UI idle" : "UI " + std::to_string(fps) + " FPS", textLayout, s),
        };
    }

    canton::core::DrawOp DiagnosticsScene::badgeText(canton::core::Rect area, std::string value, const canton::core::TextLayoutService& textLayout, float dpiScale) {
        const float s = dpiScale > 0.0f ? dpiScale : 1.0f;
        auto op = text(area, std::move(value), 0xffe8eaedu, 9.5f * s);
        canton::core::DrawOpsBuilder::prepareText(op, textLayout);
        // 重放从文本框顶部开始；用实际字形墨迹的中点与徽标中点对齐。
        const auto& ink = op.textLayout->inkBounds;
        op.rect.y = area.y + area.height * 0.5f - (ink.y + ink.height * 0.5f);
        return op;
    }

    std::vector<canton::core::DrawOp> DiagnosticsScene::buildToasts(core::Rect editorBounds, const std::vector<DiagnosticsToastModel>& toasts, DiagnosticVisibility visibility, float dpiScale) const {
        std::vector<canton::core::DrawOp> ops;
        if (!diagnosticVisibilityEnabled(visibility) || toasts.empty()) return ops;

        const float s = dpiScale > 0.0f ? dpiScale : 1.0f;
        const float maxToastWidth = std::clamp(editorBounds.width - 32.0f * s, 220.0f * s, 360.0f * s);
        float y = editorBounds.y + 48.0f * s;
        auto drawn = 0;
        for (auto it = toasts.rbegin(); it != toasts.rend() && drawn < 3; ++it, ++drawn) {
            const float titleWidth = static_cast<float>(it->title.size()) * 8.0f * s;
            const float messageWidth = static_cast<float>(it->message.size()) * 7.0f * s;
            const float width = std::round(std::min(maxToastWidth, std::max(180.0f * s, std::max(titleWidth, messageWidth) + 28.0f * s)));
            const float height = std::round((it->message.empty() ? 30.0f : 48.0f) * s);
            const float x = std::round(std::max(editorBounds.x + 12.0f * s, editorBounds.x + editorBounds.width - width - 12.0f * s));
            const canton::core::Rect toast{x, y, width, height};
            ops.push_back(fill(toast, 0xe6111827u, 8.0f * s));
            ops.push_back(stroke(toast, accentColour(it->level), 1.0f * s, 8.0f * s));
            ops.push_back(fill({toast.x, toast.y, 4.0f * s, toast.height}, accentColour(it->level), 4.0f * s));
            ops.push_back(text({toast.x + 12.0f * s, toast.y + 6.0f * s, toast.width - 24.0f * s, 15.0f * s}, it->title, 0xfff8fafcu, 10.0f * s));
            if (!it->message.empty()) {
                ops.push_back(text({toast.x + 12.0f * s, toast.y + 23.0f * s, toast.width - 24.0f * s, toast.height - 29.0f * s}, it->message, 0xffcbd5e1u, 9.5f * s));
            }
            y += height + 8.0f * s;
        }
        return ops;
    }

    canton::core::DrawOp DiagnosticsScene::fill(canton::core::Rect rect, std::uint32_t color, float radius) {
        canton::core::DrawOp op;
        op.type = canton::core::DrawOpType::FillRect;
        op.rect = rect;
        op.color = color;
        op.shape = radius > 0.0f ? canton::core::DrawShapeType::Rounded : canton::core::DrawShapeType::Rectangle;
        op.corners = canton::core::CornerRadii::uniform(radius);
        return op;
    }

    canton::core::DrawOp DiagnosticsScene::stroke(canton::core::Rect rect, std::uint32_t color, float width, float radius) {
        canton::core::DrawOp op;
        op.type = canton::core::DrawOpType::StrokeRect;
        op.rect = rect;
        op.color = color;
        op.strokeWidth = width;
        op.strokeInside = true;
        op.shape = radius > 0.0f ? canton::core::DrawShapeType::Rounded : canton::core::DrawShapeType::Rectangle;
        op.corners = canton::core::CornerRadii::uniform(radius);
        return op;
    }

    canton::core::DrawOp DiagnosticsScene::text(canton::core::Rect rect, std::string value, std::uint32_t color, float fontSize, int maxLines) {
        canton::core::DrawOp op;
        op.type = canton::core::DrawOpType::DrawText;
        op.rect = rect;
        op.color = color;
        op.textStyle.fontSize = fontSize;
        op.textStyle.lineHeight = fontSize * canton::core::defaultLineHeightRatio;
        op.maxLines = maxLines;
        op.text = std::move(value);
        op.overflow = canton::core::TextOverflow::Clip;
        return op;
    }

    namespace {
        class DiagnosticsHudImpl final : public DiagnosticsHud {
           public:
            explicit DiagnosticsHudImpl(DiagnosticsConfig config) {
                model_.configure(std::move(config));
            }

            bool showToast(core::LogLevel level, std::string_view tag, std::string title, std::string message, bool coalesce) override {
                const auto visible = DiagnosticsToast::show(model_, level, tag, std::move(title), std::move(message), coalesce);
                if (visible) invalidate();
                return visible;
            }

            bool tick(CantonRuntime& runtime, double nowMillis) override {
                auto changed = false;
                for (auto toast : runtime.takeDiagnosticToasts()) {
                    changed = showToast(toast.level, toast.tag, std::move(toast.title), std::move(toast.content), toast.coalesce) || changed;
                }
                for (const auto& action : runtime.takeDiagnosticActions()) {
                    switch (action.kind) {
                        case canton::quickjs::QuickJsDiagnosticActionKind::RequestReload:
                            (void)showToast(core::LogLevel::Info, HUD_TAG, "脚本请求重新加载", action.path, true);
                            runtime.requestReload();
                            break;
                        case canton::quickjs::QuickJsDiagnosticActionKind::TriggerFakeError:
                            changed = showToast(core::LogLevel::Error, HUD_TAG, action.message.empty() ? std::string("脚本请求诊断错误") : action.message, "脚本诊断已触发错误屏路径", true) || changed;
                            break;
                        case canton::quickjs::QuickJsDiagnosticActionKind::SetToastsEnabled:
                            model_.setToastsEnabled(action.enabled);
                            invalidate();
                            changed = true;
                            core::Log::i(HUD_TAG, "诊断提示显示状态已更新", action.enabled ? "已启用" : "已关闭");
                            break;
                    }
                }
                if (model_.tick(nowMillis)) {
                    invalidate();
                    changed = true;
                }
                return changed;
            }

            bool hasActiveToasts() const noexcept override {
                return model_.hasActiveToasts();
            }

            void setFpsEnabled(bool enabled, CantonRuntime& runtime) override {
                if (fpsEnabled_ == enabled) return;
                fpsEnabled_ = enabled;
                fpsFrames_ = 0;
                fpsWindowStart_ = 0;
                fps_ = enabled ? std::optional<int>(0) : std::nullopt;
                runtime.enqueueIntent(core::InputIntent::diagnostics());
            }

            bool fpsEnabled() const noexcept override {
                return fpsEnabled_;
            }

            void recordFrame(bool changed, bool ready, double nowMillis, CantonRuntime& runtime) override {
                if (!fpsEnabled_) return;
                if (changed) ++fpsFrames_;
                if (nowMillis - fpsWindowStart_ >= 500.0) {
                    fps_ = ready ? static_cast<int>(fpsFrames_ * 1000.0 / (nowMillis - fpsWindowStart_) + 0.5) : 0;
                    fpsWindowStart_ = nowMillis;
                    fpsFrames_ = 0;
                    runtime.enqueueIntent(core::InputIntent::diagnostics());
                }
            }

            void prepare(core::Rect bounds, bool hasError, PackageSource activeSource, bool liveUnavailable, const core::TextLayoutService& textLayout, core::PublishedFrameContent& content, float dpiScale) override {
                const float s = dpiScale > 0.0f ? dpiScale : 1.0f;
                const auto environment = textLayout.environment();
                const auto fps = hasError ? std::nullopt : fps_;
                if (preparedFps_ != fps || preparedFpsScale_ != s || (fps && (preparedFpsBounds_ != bounds || preparedFpsOps_.back().textLayout->environment != environment))) {
                    preparedFpsOps_ = fps ? scene_.buildFpsBadge(bounds, *fps, textLayout, s) : std::vector<canton::core::DrawOp>{};
                    preparedFpsBounds_ = bounds;
                    preparedFps_ = fps;
                    preparedFpsScale_ = s;
                }
                const auto badge = sourceBadgeModel(activeSource, liveUnavailable);
                std::ostringstream sig;
                sig << bounds.x << ',' << bounds.y << ',' << bounds.width << ',' << bounds.height << "|s=" << s << "|font=" << environment << "|badge=" << static_cast<int>(model_.badgeVisibility()) << ':' << badge.text << ':' << badge.dot << "|toastVis=" << static_cast<int>(model_.toastVisibility());
                const auto toasts = model_.activeToastModels();
                for (const auto& toast : toasts) sig << "|toast=" << static_cast<int>(toast.level) << ':' << toast.title << ':' << toast.message;
                if (const auto signature = sig.str(); signature != preparedSignature_) {
                    preparedBadgeOps_ = scene_.buildBadge(bounds, badge, model_.badgeVisibility(), textLayout, s);
                    preparedToastOps_ = scene_.buildToasts(bounds, toasts, model_.toastVisibility(), s);
                    preparedSignature_ = signature;
                }
                replaceHudOps(content.diagnosticsBadgeDrawOps, preparedBadgeOps_);
                content.diagnosticsFpsDrawOps = preparedFpsOps_;
                replaceHudOps(content.diagnosticsToastDrawOps, preparedToastOps_);
            }

           private:
            void invalidate() noexcept {
                preparedSignature_.clear();
                preparedBadgeOps_.clear();
                preparedToastOps_.clear();
            }

            DiagnosticsModel model_;
            DiagnosticsScene scene_;
            bool fpsEnabled_ = false;
            double fpsWindowStart_ = 0;
            int fpsFrames_ = 0;
            std::optional<int> fps_;
            std::optional<int> preparedFps_;
            float preparedFpsScale_ = 1.0f;
            core::Rect preparedFpsBounds_{};
            std::vector<canton::core::DrawOp> preparedBadgeOps_;
            std::vector<canton::core::DrawOp> preparedFpsOps_;
            std::vector<canton::core::DrawOp> preparedToastOps_;
            std::string preparedSignature_;
        };
    }

    std::unique_ptr<DiagnosticsHud> createDiagnosticsHud(DiagnosticsConfig config) {
        return std::make_unique<DiagnosticsHudImpl>(std::move(config));
    }

    void SceneHost::setFpsBadgeEnabled(bool enabled) {
        if (!diagnostics_.hud()) {
            if (!enabled) return;
            DiagnosticsConfig config;
            config.badge = DiagnosticVisibility::Hidden;
            config.toasts = DiagnosticVisibility::Hidden;
            config.overlay = DiagnosticVisibility::Hidden;
            diagnostics_.setHud(createDiagnosticsHud(config));
        }
        diagnostics_.hud()->setFpsEnabled(enabled, runtime_);
    }
}
