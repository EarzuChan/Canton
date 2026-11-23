#include <runtime/DiagnosticsScene.h>

#include <core/DiagnosticsOverlay.h>
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
                    return 0xff38bdf8u;
                case core::LogLevel::Warn:
                    return 0xfffbbf24u;
                case core::LogLevel::Error:
                    return 0xfff87171u;
            }
            return 0xff38bdf8u;
        }

        std::uint32_t withAlpha(std::uint32_t argb, std::uint8_t alpha) noexcept {
            return (static_cast<std::uint32_t>(alpha) << 24) | (argb & 0x00ffffffu);
        }

        DiagnosticsBadgeModel sourceBadgeModel(PackageSource source, bool liveUnavailable) {
            switch (source) {
                case PackageSource::Live:
                    return {"LIVE", 0xff34d399u};
                case PackageSource::Dist:
                    return liveUnavailable ? DiagnosticsBadgeModel{"DIST FALLBACK", 0xfffbbf24u} : DiagnosticsBadgeModel{"DIST", 0xff60a5fau};
                case PackageSource::None:
                    break;
            }
            return {"NO APP", 0xfff87171u};
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
        const float height = std::round(22.0f * s);
        const float radius = height * 0.5f;
        auto labelOp = badgeText({0.0f, 0.0f, 240.0f * s, height}, model.text, textLayout, s);
        const float labelWidth = labelOp.textLayout ? std::ceil(labelOp.textLayout->width) : std::round(static_cast<float>(model.text.size()) * 6.5f * s);
        const float badgeWidth = std::round(std::max(56.0f * s, 24.0f * s + labelWidth + 11.0f * s));
        const float x = std::round(std::max(editorBounds.x + 12.0f * s, editorBounds.x + editorBounds.width - 12.0f * s - badgeWidth));
        const float y = std::round(editorBounds.y + 12.0f * s);
        const canton::core::Rect badge{x, y, badgeWidth, height};
        const canton::core::Rect shadow{x - 1.0f * s, y + 1.5f * s, badgeWidth + 2.0f * s, height + 1.0f * s};

        ops.push_back(fill(shadow, 0x30000000u, radius + 1.0f * s));
        ops.push_back(fill(badge, 0xe0121620u, radius));
        ops.push_back(stroke(badge, 0x26ffffffu, 1.0f * s, radius));

        const float dotCenterX = badge.x + 12.5f * s;
        const float dotCenterY = badge.y + height * 0.5f;
        ops.push_back(fill({dotCenterX - 5.0f * s, dotCenterY - 5.0f * s, 10.0f * s, 10.0f * s}, withAlpha(model.dot, 0x38u), 5.0f * s));
        ops.push_back(fill({dotCenterX - 2.75f * s, dotCenterY - 2.75f * s, 5.5f * s, 5.5f * s}, model.dot, 2.75f * s));

        labelOp.rect.x = std::round(badge.x + 22.0f * s);
        labelOp.rect.width = std::max(0.0f, badge.width - 31.0f * s);
        const auto& ink = labelOp.textLayout->inkBounds;
        labelOp.rect.y = badge.y + height * 0.5f - (ink.y + ink.height * 0.5f);
        ops.push_back(std::move(labelOp));
        return ops;
    }

    std::vector<canton::core::DrawOp> DiagnosticsScene::buildFpsBadge(core::Rect editorBounds, int fps, const canton::core::TextLayoutService& textLayout, float dpiScale) const {
        const float s = dpiScale > 0.0f ? dpiScale : 1.0f;
        const float height = std::round(22.0f * s);
        const float radius = height * 0.5f;
        const std::string label = fps == 0 ? "UI IDLE" : std::to_string(fps) + " FPS";
        const std::uint32_t dot = fps == 0 ? 0xff64748bu : 0xff34d399u;
        auto labelOp = badgeText({0.0f, 0.0f, 160.0f * s, height}, label, textLayout, s);
        const float labelWidth = labelOp.textLayout ? std::ceil(labelOp.textLayout->width) : std::round(static_cast<float>(label.size()) * 6.5f * s);
        const float badgeWidth = std::round(std::max(72.0f * s, 24.0f * s + labelWidth + 11.0f * s));
        const canton::core::Rect badge{std::round(editorBounds.x + 12.0f * s), std::round(editorBounds.y + 12.0f * s), badgeWidth, height};
        const canton::core::Rect shadow{badge.x - 1.0f * s, badge.y + 1.5f * s, badgeWidth + 2.0f * s, height + 1.0f * s};

        const float dotCenterX = badge.x + 12.5f * s;
        const float dotCenterY = badge.y + height * 0.5f;
        labelOp.rect.x = std::round(badge.x + 22.0f * s);
        labelOp.rect.width = std::max(0.0f, badge.width - 31.0f * s);
        const auto& ink = labelOp.textLayout->inkBounds;
        labelOp.rect.y = badge.y + height * 0.5f - (ink.y + ink.height * 0.5f);

        return {
            fill(shadow, 0x30000000u, radius + 1.0f * s),
            fill(badge, 0xe0121620u, radius),
            stroke(badge, 0x26ffffffu, 1.0f * s, radius),
            fill({dotCenterX - 5.0f * s, dotCenterY - 5.0f * s, 10.0f * s, 10.0f * s}, withAlpha(dot, 0x30u), 5.0f * s),
            fill({dotCenterX - 2.75f * s, dotCenterY - 2.75f * s, 5.5f * s, 5.5f * s}, dot, 2.75f * s),
            std::move(labelOp),
        };
    }

    canton::core::DrawOp DiagnosticsScene::badgeText(canton::core::Rect area, std::string value, const canton::core::TextLayoutService& textLayout, float dpiScale) {
        const float s = dpiScale > 0.0f ? dpiScale : 1.0f;
        auto op = text(area, std::move(value), 0xfff1f5f9u, 10.0f * s);
        op.textStyle.fontWeight = 600;
        canton::core::DrawOpsBuilder::prepareText(op, textLayout);
        // 重放从文本框顶部开始；用实际字形墨迹的中点与徽标中点对齐。
        const auto& ink = op.textLayout->inkBounds;
        op.rect.y = area.y + area.height * 0.5f - (ink.y + ink.height * 0.5f);
        return op;
    }

    std::vector<canton::core::DrawOp> DiagnosticsScene::buildToasts(core::Rect editorBounds, const std::vector<DiagnosticsToastModel>& toasts, DiagnosticVisibility visibility, float dpiScale, const canton::core::TextLayoutService* textLayout) const {
        std::vector<canton::core::DrawOp> ops;
        if (!diagnosticVisibilityEnabled(visibility) || toasts.empty()) return ops;

        const float s = dpiScale > 0.0f ? dpiScale : 1.0f;
        const float margin = 12.0f * s;
        const float availWidth = std::max(140.0f * s, editorBounds.width - margin * 2.0f);
        const float maxToastWidth = std::min(availWidth, 340.0f * s);
        const float minToastWidth = std::min(maxToastWidth, 210.0f * s);
        float y = std::round(editorBounds.y + 42.0f * s);
        auto drawn = 0;

        const auto measureWidth = [&](const std::string& value, float fontSize, int weight) -> float {
            if (value.empty()) return 0.0f;
            if (textLayout) {
                auto probe = text({0.0f, 0.0f, maxToastWidth, fontSize * 2.0f}, value, 0xffffffffu, fontSize);
                probe.textStyle.fontWeight = weight;
                canton::core::DrawOpsBuilder::prepareText(probe, *textLayout);
                if (probe.textLayout) return std::ceil(probe.textLayout->width);
            }
            return static_cast<float>(value.size()) * fontSize * 0.62f;
        };

        for (auto it = toasts.rbegin(); it != toasts.rend() && drawn < 3; ++it, ++drawn) {
            const float titleSize = 11.5f * s;
            const float messageSize = 10.5f * s;
            const float contentWidth = std::max(measureWidth(it->title, titleSize, 600), measureWidth(it->message, messageSize, 400));
            const float width = std::round(std::clamp(contentWidth + 36.0f * s, minToastWidth, maxToastWidth));
            const bool hasMessage = !it->message.empty();
            const float height = std::round((hasMessage ? 52.0f : 34.0f) * s);
            const float x = std::round(std::max(editorBounds.x + margin, editorBounds.x + editorBounds.width - width - margin));
            const canton::core::Rect toast{x, y, width, height};
            const canton::core::Rect shadow{x - 1.0f * s, y + 2.0f * s, width + 2.0f * s, height + 1.0f * s};
            const auto accent = accentColour(it->level);

            ops.push_back(fill(shadow, 0x36000000u, 11.0f * s));
            ops.push_back(fill(toast, 0xf2141923u, 10.0f * s));
            ops.push_back(stroke(toast, 0x22ffffffu, 1.0f * s, 10.0f * s));
            ops.push_back(fill({toast.x + 8.0f * s, toast.y + 9.0f * s, 3.0f * s, std::max(8.0f * s, toast.height - 18.0f * s)}, accent, 1.5f * s));

            const float textX = toast.x + 20.0f * s;
            const float textW = std::max(0.0f, toast.width - 32.0f * s);
            auto titleOp = text({textX, toast.y + (hasMessage ? 9.0f : 9.0f) * s, textW, 16.0f * s}, it->title, 0xfff8fafcu, titleSize);
            titleOp.textStyle.fontWeight = 600;
            titleOp.overflow = canton::core::TextOverflow::Ellipsis;
            ops.push_back(std::move(titleOp));

            if (hasMessage) {
                auto msgOp = text({textX, toast.y + 27.0f * s, textW, 16.0f * s}, it->message, 0xff94a3b8u, messageSize);
                msgOp.overflow = canton::core::TextOverflow::Ellipsis;
                ops.push_back(std::move(msgOp));
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
                overlay_.setLayers({.frameTime = false, .status = false, .dirtyFlash = false, .bounds = false, .hit = false});
                overlay_.setEnabled(false);
            }

            bool showToast(core::LogLevel level, std::string_view tag, std::string title, std::string message, bool coalesce) override {
                const auto visible = DiagnosticsToast::show(model_, level, tag, std::move(title), std::move(message), coalesce);
                if (visible) invalidate();
                return visible;
            }

            bool tickToasts(double nowMillis) override {
                if (model_.tick(nowMillis)) {
                    invalidate();
                    return true;
                }
                return false;
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
                if (tickToasts(nowMillis)) changed = true;
                return changed;
            }

            bool hasActiveToasts() const noexcept override {
                return model_.hasActiveToasts();
            }

            bool setFpsEnabled(bool enabled) override {
                if (fpsEnabled_ == enabled) return false;
                fpsEnabled_ = enabled;
                fpsFrames_ = 0;
                fpsWindowStart_ = 0;
                fps_ = enabled ? std::optional<int>(0) : std::nullopt;
                return true;
            }

            bool fpsEnabled() const noexcept override {
                return fpsEnabled_;
            }

            bool recordFrame(bool appChanged, bool ready, double nowMillis) override {
                if (!fpsEnabled_) return false;
                if (appChanged) ++fpsFrames_;
                if (nowMillis - fpsWindowStart_ >= 500.0) {
                    fps_ = ready ? static_cast<int>(fpsFrames_ * 1000.0 / (nowMillis - fpsWindowStart_) + 0.5) : 0;
                    fpsWindowStart_ = nowMillis;
                    fpsFrames_ = 0;
                    return true;
                }
                return false;
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
                    preparedToastOps_ = scene_.buildToasts(bounds, toasts, model_.toastVisibility(), s, &textLayout);
                    preparedSignature_ = signature;
                }
                replaceHudOps(content.diagnosticsBadgeDrawOps, preparedBadgeOps_);
                content.diagnosticsFpsDrawOps = preparedFpsOps_;
                replaceHudOps(content.diagnosticsToastDrawOps, preparedToastOps_);
            }

            void setOverlayFrameTimeEnabled(bool enabled) noexcept override {
                auto layers = overlay_.layers();
                layers.frameTime = enabled;
                layers.status = enabled;
                overlay_.setLayers(layers);
                syncOverlayEnabled();
            }

            bool overlayFrameTimeEnabled() const noexcept override {
                return overlay_.layers().frameTime;
            }

            void setOverlayDirtyFlashEnabled(bool enabled) noexcept override {
                auto layers = overlay_.layers();
                layers.dirtyFlash = enabled;
                overlay_.setLayers(layers);
                syncOverlayEnabled();
            }

            bool overlayDirtyFlashEnabled() const noexcept override {
                return overlay_.layers().dirtyFlash;
            }

            void setOverlayBoundsEnabled(bool enabled) noexcept override {
                auto layers = overlay_.layers();
                layers.bounds = enabled;
                layers.hit = enabled;
                overlay_.setLayers(layers);
                syncOverlayEnabled();
            }

            bool overlayBoundsEnabled() const noexcept override {
                const auto& layers = overlay_.layers();
                return layers.bounds || layers.hit;
            }

            bool overlayNeedsTimeline() const noexcept override {
                const auto& layers = overlay_.layers();
                return layers.frameTime || layers.status || layers.dirtyFlash;
            }

            void setOverlayPointer(std::optional<core::Point> point) noexcept override {
                overlay_.setPointer(point);
            }

            bool updateOverlay(const core::FrameTimeline& timeline, const core::PublishedFrame& frame, const core::LayoutTree& tree, core::NodeId root, const core::TextLayoutService& textLayout, double nowMillis, float dpiScale) override {
                const float scale = dpiScale > 0.0f ? dpiScale : 1.0f;
                const bool scaleChanged = std::abs(overlay_.dpiScale() - scale) > 1e-4f;
                overlay_.setDpiScale(scale);
                const auto damage = overlay_.update(timeline, frame, tree, root, nowMillis);
                const auto fontEnv = textLayout.environment();
                const bool changed = !damage.empty() || scaleChanged || (overlay_.enabled() && preparedOverlayFontEnv_ != fontEnv);
                if (!changed) return false;
                preparedOverlayFontEnv_ = fontEnv;
                preparedOverlayOps_ = overlay_.ops();
                for (auto& op : preparedOverlayOps_) {
                    if (op.type == core::DrawOpType::DrawText) {
                        core::DrawOpsBuilder::prepareText(op, textLayout);
                    }
                }
                return true;
            }

            std::span<const core::DrawOp> overlayOps() const noexcept override {
                return preparedOverlayOps_;
            }

            bool overlayAnimating() const noexcept override {
                return overlay_.enabled() && overlay_.animating();
            }

           private:
            void syncOverlayEnabled() noexcept {
                const auto& layers = overlay_.layers();
                overlay_.setEnabled(layers.frameTime || layers.status || layers.dirtyFlash || layers.bounds || layers.hit);
            }

            void invalidate() noexcept {
                preparedSignature_.clear();
                preparedBadgeOps_.clear();
                preparedToastOps_.clear();
            }

            DiagnosticsModel model_;
            DiagnosticsScene scene_;
            core::DiagnosticsOverlay overlay_;
            std::vector<canton::core::DrawOp> preparedOverlayOps_;
            std::uint64_t preparedOverlayFontEnv_ = 0;
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
        if (diagnostics_.hud()->setFpsEnabled(enabled)) {
            runtime_.enqueueIntent(core::InputIntent::diagnostics());
        }
    }
}
