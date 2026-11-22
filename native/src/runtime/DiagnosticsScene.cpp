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

    std::vector<canton::core::DrawOp> DiagnosticsScene::buildErrorScreen(core::Rect editorBounds, const ErrorScreenModel& error, bool detailed) const {
        return buildErrorScreenOps(editorBounds, error, detailed);
    }

    std::vector<canton::core::DrawOp> DiagnosticsScene::buildBadge(core::Rect editorBounds, const DiagnosticsBadgeModel& model, DiagnosticVisibility visibility, const canton::core::TextLayoutService& textLayout) const {
        std::vector<canton::core::DrawOp> ops;
        if (!diagnosticVisibilityEnabled(visibility) || model.text.empty()) return ops;

        const auto textWidth = static_cast<int>(model.text.size()) * 7 + 26;
        const auto x = static_cast<int>(std::round(std::max(editorBounds.x + 8.0f, editorBounds.x + editorBounds.width - 8.0f - textWidth)));
        const auto y = editorBounds.y + 20;
        const canton::core::Rect badge{static_cast<float>(x), static_cast<float>(y), static_cast<float>(textWidth), 20.0f};
        ops.push_back(fill(badge, 0xcc111827u, 6.0f));
        ops.push_back(stroke(badge, 0x664b5563u, 1.0f, 6.0f));
        ops.push_back(fill({badge.x + 8.0f, badge.y + 7.0f, 6.0f, 6.0f}, model.dot, 3.0f));
        ops.push_back(badgeText({badge.x + 20.0f, badge.y, badge.width - 20.0f, badge.height}, model.text, textLayout));
        return ops;
    }

    std::vector<canton::core::DrawOp> DiagnosticsScene::buildFpsBadge(core::Rect editorBounds, int fps, const canton::core::TextLayoutService& textLayout) const {
        const canton::core::Rect badge{editorBounds.x + 8.0f, editorBounds.y + 20.0f, 78.0f, 20.0f};
        return {
            fill(badge, 0xcc111827u, 6.0f),
            stroke(badge, 0x664b5563u, 1.0f, 6.0f),
            badgeText({badge.x + 8.0f, badge.y, badge.width - 16.0f, badge.height}, fps == 0 ? "UI idle" : "UI " + std::to_string(fps) + " FPS", textLayout),
        };
    }

    canton::core::DrawOp DiagnosticsScene::badgeText(canton::core::Rect area, std::string value, const canton::core::TextLayoutService& textLayout) {
        auto op = text(area, std::move(value), 0xffe8eaedu, 9.5f);
        canton::core::DrawOpsBuilder::prepareText(op, textLayout);
        // 重放从文本框顶部开始；用实际字形墨迹的中点与徽标中点对齐。
        const auto& ink = op.textLayout->inkBounds;
        op.rect.y = area.y + area.height * 0.5f - (ink.y + ink.height * 0.5f);
        return op;
    }

    std::vector<canton::core::DrawOp> DiagnosticsScene::buildToasts(core::Rect editorBounds, const std::vector<DiagnosticsToastModel>& toasts, DiagnosticVisibility visibility) const {
        std::vector<canton::core::DrawOp> ops;
        if (!diagnosticVisibilityEnabled(visibility) || toasts.empty()) return ops;

        const auto maxToastWidth = std::clamp(static_cast<int>(editorBounds.width) - 32, 220, 360);
        auto y = editorBounds.y + 48;
        auto drawn = 0;
        for (auto it = toasts.rbegin(); it != toasts.rend() && drawn < 3; ++it, ++drawn) {
            const auto titleWidth = static_cast<int>(it->title.size()) * 8;
            const auto messageWidth = static_cast<int>(it->message.size()) * 7;
            const auto width = std::min(maxToastWidth, std::max(180, std::max(titleWidth, messageWidth) + 28));
            const auto height = it->message.empty() ? 30 : 48;
            const auto x = static_cast<int>(std::round(std::max(editorBounds.x + 12.0f, editorBounds.x + editorBounds.width - width - 12.0f)));
            const canton::core::Rect toast{static_cast<float>(x), static_cast<float>(y), static_cast<float>(width), static_cast<float>(height)};
            ops.push_back(fill(toast, 0xe6111827u, 8.0f));
            ops.push_back(stroke(toast, accentColour(it->level), 1.0f, 8.0f));
            ops.push_back(fill({toast.x, toast.y, 4.0f, toast.height}, accentColour(it->level), 4.0f));
            ops.push_back(text({toast.x + 12.0f, toast.y + 6.0f, toast.width - 24.0f, 15.0f}, it->title, 0xfff8fafcu, 10.0f));
            if (!it->message.empty()) {
                ops.push_back(text({toast.x + 12.0f, toast.y + 23.0f, toast.width - 24.0f, toast.height - 29.0f}, it->message, 0xffcbd5e1u, 9.5f));
            }
            y += height + 8;
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

            void prepare(core::Rect bounds, bool hasError, PackageSource activeSource, bool liveUnavailable, const core::TextLayoutService& textLayout, core::PublishedFrameContent& content) override {
                const auto environment = textLayout.environment();
                const auto fps = hasError ? std::nullopt : fps_;
                if (preparedFps_ != fps || (fps && (preparedFpsBounds_ != bounds || preparedFpsOps_.back().textLayout->environment != environment))) {
                    preparedFpsOps_ = fps ? scene_.buildFpsBadge(bounds, *fps, textLayout) : std::vector<canton::core::DrawOp>{};
                    preparedFpsBounds_ = bounds;
                    preparedFps_ = fps;
                }
                const auto badge = sourceBadgeModel(activeSource, liveUnavailable);
                std::ostringstream sig;
                sig << bounds.x << ',' << bounds.y << ',' << bounds.width << ',' << bounds.height << "|font=" << environment << "|badge=" << static_cast<int>(model_.badgeVisibility()) << ':' << badge.text << ':' << badge.dot << "|toastVis=" << static_cast<int>(model_.toastVisibility());
                const auto toasts = model_.activeToastModels();
                for (const auto& toast : toasts) sig << "|toast=" << static_cast<int>(toast.level) << ':' << toast.title << ':' << toast.message;
                if (const auto signature = sig.str(); signature != preparedSignature_) {
                    preparedBadgeOps_ = scene_.buildBadge(bounds, badge, model_.badgeVisibility(), textLayout);
                    preparedToastOps_ = scene_.buildToasts(bounds, toasts, model_.toastVisibility());
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
