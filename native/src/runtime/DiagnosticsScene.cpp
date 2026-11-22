#include <runtime/DiagnosticsScene.h>

#include <core/TextLayoutService.h>
#include <algorithm>
#include <cmath>

namespace canton::runtime {
    namespace {
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
    }

    std::vector<canton::core::DrawOp> DiagnosticsScene::buildErrorScreen(core::Rect editorBounds, const ErrorScreenModel& error, bool detailed) const {
        std::vector<canton::core::DrawOp> ops;
        core::Rect area{editorBounds.x + 20, editorBounds.y + 20, std::max(0.0f, editorBounds.width - 40), std::max(0.0f, editorBounds.height - 40)};
        ops.push_back(fill(area, detailed ? 0xff2a1014u : 0xff111827u, 10.0f));
        if (detailed) {
            const auto titleArea = canton::core::Rect{area.x, area.y, area.width, 34.0f};
            ops.push_back(text(titleArea, error.title.empty() ? "Canton Error" : error.title, 0xffff6b6bu, 15.0f));
            area.y += 34.0f;
            area.height = std::max(0.0f, area.height - 34.0f);
            auto body = error.diagnosticText();
            if (error.retryAvailable) body += "\nClick anywhere to retry.";
            ops.push_back(text(area, std::move(body), 0xe0ffffffu, 11.0f, 8));
            return ops;
        }

        const auto titleArea = canton::core::Rect{area.x, area.y, area.width, 32.0f};
        ops.push_back(text(titleArea, "Canton error screen hidden", 0xfff59e0bu, 14.5f));
        area.y += 32.0f;
        area.height = std::max(0.0f, area.height - 32.0f);
        ops.push_back(text(area, error.summary + "\nError details are still available in logs and diagnostics.", 0xc7ffffffu, 11.0f, 4));
        return ops;
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
}
