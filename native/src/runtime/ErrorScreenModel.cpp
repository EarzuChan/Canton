#include <runtime/ErrorScreenModel.h>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>

namespace canton::runtime {
    namespace {
        core::DrawOp errorFill(core::Rect rect, std::uint32_t color, float radius = 0.0f) {
            core::DrawOp op;
            op.type = core::DrawOpType::FillRect;
            op.rect = rect;
            op.color = color;
            op.shape = radius > 0.0f ? core::DrawShapeType::Rounded : core::DrawShapeType::Rectangle;
            op.corners = core::CornerRadii::uniform(radius);
            return op;
        }

        core::DrawOp errorStroke(core::Rect rect, std::uint32_t color, float width, float radius = 0.0f) {
            core::DrawOp op;
            op.type = core::DrawOpType::StrokeRect;
            op.rect = rect;
            op.color = color;
            op.strokeWidth = width;
            op.strokeInside = true;
            op.shape = radius > 0.0f ? core::DrawShapeType::Rounded : core::DrawShapeType::Rectangle;
            op.corners = core::CornerRadii::uniform(radius);
            return op;
        }

        core::DrawOp errorText(core::Rect rect, std::string value, std::uint32_t color, float fontSize, int maxLines = 1, int fontWeight = 400, core::TextAlign align = core::TextAlign::Start) {
            core::DrawOp op;
            op.type = core::DrawOpType::DrawText;
            op.rect = rect;
            op.color = color;
            op.textStyle.fontSize = fontSize;
            op.textStyle.lineHeight = fontSize * 1.42f;
            op.textStyle.fontWeight = fontWeight;
            op.textAlign = align;
            op.maxLines = maxLines;
            op.text = std::move(value);
            op.overflow = core::TextOverflow::Ellipsis;
            return op;
        }

        std::vector<core::DrawOp> buildCompactErrorScreen(core::Rect bounds, const ErrorScreenModel& error, float s) {
            std::vector<core::DrawOp> ops;
            ops.push_back(errorFill(bounds, 0xff0b0f17u, 0.0f));

            const float pad = 14.0f * s;
            const float availW = std::max(0.0f, bounds.width - pad * 2.0f);
            const float availH = std::max(0.0f, bounds.height - pad * 2.0f);
            const float cardW = std::round(std::clamp(availW, std::min(availW, 136.0f * s), 288.0f * s));
            const bool showSubtitle = availH >= 68.0f * s && cardW >= 180.0f * s;
            const float cardH = std::round(std::min(availH, (showSubtitle ? 74.0f : 44.0f) * s));
            if (cardW < 64.0f * s || cardH < 28.0f * s) return ops;

            const float cx = std::round(bounds.x + (bounds.width - cardW) * 0.5f);
            const float cy = std::round(bounds.y + (bounds.height - cardH) * 0.5f);
            const core::Rect card{cx, cy, cardW, cardH};
            const core::Rect shadow{cx - 1.0f * s, cy + 3.0f * s, cardW + 2.0f * s, cardH + 2.0f * s};

            ops.push_back(errorFill(shadow, 0x44000000u, 13.0f * s));
            ops.push_back(errorFill(card, 0xf2141923u, 12.0f * s));
            ops.push_back(errorStroke(card, 0x26ffffffu, 1.0f * s, 12.0f * s));

            const float dotX = card.x + 18.0f * s;
            const float dotY = showSubtitle ? card.y + 23.0f * s : card.y + card.height * 0.5f;
            ops.push_back(errorFill({dotX - 5.5f * s, dotY - 5.5f * s, 11.0f * s, 11.0f * s}, 0x38f87171u, 5.5f * s));
            ops.push_back(errorFill({dotX - 3.0f * s, dotY - 3.0f * s, 6.0f * s, 6.0f * s}, 0xfff87171u, 3.0f * s));

            const float textX = card.x + 32.0f * s;
            const float textW = std::max(0.0f, card.width - 46.0f * s);
            const float titleY = showSubtitle ? card.y + 14.0f * s : card.y + (card.height - 17.0f * s) * 0.5f;
            ops.push_back(errorText({textX, titleY, textW, 18.0f * s}, "应用运行异常", 0xfff8fafcu, 12.5f * s, 1, 600));
            if (showSubtitle) {
                const std::string sub = error.retryAvailable ? "请查看主错误屏 · F5 重试" : "请查看主错误屏";
                ops.push_back(errorText({textX, card.y + 39.0f * s, textW, 18.0f * s}, sub, 0xff94a3b8u, 10.5f * s, 1, 400));
            }
            return ops;
        }
    }

    const char* toString(ErrorSource source) noexcept {
        switch (source) {
            case ErrorSource::AppPackage:
                return "AppPackage";
            case ErrorSource::ScriptLoad:
                return "ScriptLoad";
            case ErrorSource::ScriptRuntime:
                return "ScriptRuntime";
            case ErrorSource::NativeTransaction:
                return "NativeTransaction";
            case ErrorSource::Hmr:
                return "HMR";
            case ErrorSource::Resource:
                return "Resource";
            case ErrorSource::Unknown:
                return "Unknown";
        }
        return "Unknown";
    }

    std::string ErrorScreenModel::diagnosticText() const {
        std::ostringstream out;
        out << "Canton Error\n";
        out << "source: " << toString(source) << "\n";
        out << "title: " << title << "\n";
        out << "summary: " << summary << "\n";
        if (!detail.empty()) out << "detail: " << detail << "\n";
        if (!relatedPath.empty()) out << "path: " << relatedPath.string() << "\n";
        out << "retryAvailable: " << (retryAvailable ? "true" : "false") << "\n";
        return out.str();
    }

    ErrorScreenModel makeErrorScreenModel(ErrorSource source, std::string summary, std::string detail, std::filesystem::path relatedPath, bool retryAvailable) {
        ErrorScreenModel model;
        model.source = source;
        model.title = std::string("Canton ") + toString(source) + " Error";
        model.summary = std::move(summary);
        model.detail = std::move(detail);
        model.relatedPath = std::move(relatedPath);
        model.retryAvailable = retryAvailable;
        return model;
    }

    std::vector<core::DrawOp> buildErrorScreenOps(core::Rect editorBounds, const ErrorScreenModel& error, ErrorScreenPresentation presentation, float dpiScale) {
        const float s = dpiScale > 0.0f ? dpiScale : 1.0f;
        if (presentation == ErrorScreenPresentation::Compact) {
            return buildCompactErrorScreen(editorBounds, error, s);
        }

        std::vector<core::DrawOp> ops;
        ops.push_back(errorFill(editorBounds, 0xff0b0f17u, 0.0f));

        const bool detailed = presentation == ErrorScreenPresentation::Detailed;
        const float outerPad = 24.0f * s;
        const float maxCardW = (detailed ? 680.0f : 520.0f) * s;
        const float maxCardH = (detailed ? 480.0f : 230.0f) * s;
        const float availW = std::max(0.0f, editorBounds.width - outerPad * 2.0f);
        const float availH = std::max(0.0f, editorBounds.height - outerPad * 2.0f);
        const float cardW = std::round(std::min(availW, maxCardW));
        const float cardH = std::round(std::min(availH, maxCardH));
        if (cardW < 120.0f * s || cardH < 90.0f * s) {
            return buildCompactErrorScreen(editorBounds, error, s);
        }

        const float cardX = std::round(editorBounds.x + (editorBounds.width - cardW) * 0.5f);
        const float cardY = std::round(editorBounds.y + (editorBounds.height - cardH) * 0.5f);
        const core::Rect card{cardX, cardY, cardW, cardH};
        const core::Rect shadow{cardX - 2.0f * s, cardY + 6.0f * s, cardW + 4.0f * s, cardH + 4.0f * s};

        ops.push_back(errorFill(shadow, 0x55000000u, 15.0f * s));
        ops.push_back(errorFill(card, 0xff131824u, 14.0f * s));
        ops.push_back(errorStroke(card, 0x22ffffffu, 1.0f * s, 14.0f * s));

        const std::uint32_t accent = detailed ? 0xfff87171u : 0xfffbbf24u;
        const std::uint32_t pillFill = detailed ? 0x24ef4444u : 0x24f59e0bu;
        const std::uint32_t pillStroke = detailed ? 0x52ef4444u : 0x52f59e0bu;
        const std::uint32_t pillTextColor = detailed ? 0xfffca5a5u : 0xfffde68au;

        const float innerPad = 24.0f * s;
        const float contentX = card.x + innerPad;
        const float contentW = std::max(0.0f, card.width - innerPad * 2.0f);
        float cursorY = card.y + 20.0f * s;

        // 顶部柔和色条
        ops.push_back(errorFill({contentX, card.y, 52.0f * s, 3.0f * s}, accent, 1.5f * s));

        // 来源标签胶囊
        const std::string tagLabel = detailed ? (std::string("ERROR · ") + toString(error.source)) : std::string("APP ERROR");
        const float tagW = std::round(std::min(contentW, std::max(84.0f * s, static_cast<float>(tagLabel.size()) * 6.8f * s + 26.0f * s)));
        const float tagH = std::round(22.0f * s);
        const core::Rect tagRect{contentX, cursorY, tagW, tagH};
        ops.push_back(errorFill(tagRect, pillFill, tagH * 0.5f));
        ops.push_back(errorStroke(tagRect, pillStroke, 1.0f * s, tagH * 0.5f));
        ops.push_back(errorFill({tagRect.x + 9.0f * s, tagRect.y + (tagH - 5.0f * s) * 0.5f, 5.0f * s, 5.0f * s}, accent, 2.5f * s));
        ops.push_back(errorText({tagRect.x + 19.0f * s, tagRect.y + 3.5f * s, std::max(0.0f, tagW - 26.0f * s), 16.0f * s}, tagLabel, pillTextColor, 10.0f * s, 1, 600));
        cursorY += tagH + 14.0f * s;

        // 标题
        const std::string heading = detailed ? (error.title.empty() ? std::string("Canton 运行异常") : error.title) : std::string("应用遇到错误");
        ops.push_back(errorText({contentX, cursorY, contentW, 26.0f * s}, heading, 0xfff8fafcu, 17.0f * s, 1, 600));
        cursorY += 30.0f * s;

        // 摘要
        const std::string summaryText = error.summary.empty() ? std::string("发生未预期错误，请查看诊断信息。") : error.summary;
        const float summaryH = 44.0f * s;
        ops.push_back(errorText({contentX, cursorY, contentW, summaryH}, summaryText, 0xffe2e8f0u, 12.5f * s, 2, 400));
        cursorY += summaryH + 10.0f * s;

        // 底栏预留高度
        const float footerH = 44.0f * s;
        const float footerTop = card.y + card.height - footerH;

        if (detailed) {
            const float panelBottom = footerTop - 14.0f * s;
            const float panelH = panelBottom - cursorY;
            if (panelH >= 48.0f * s) {
                const core::Rect panel{contentX, cursorY, contentW, panelH};
                ops.push_back(errorFill(panel, 0xff0b0f17u, 8.0f * s));
                ops.push_back(errorStroke(panel, 0x1affffffu, 1.0f * s, 8.0f * s));

                float innerY = panel.y + 12.0f * s;
                const float innerX = panel.x + 14.0f * s;
                const float innerW = std::max(0.0f, panel.width - 28.0f * s);
                if (!error.relatedPath.empty() && panelH >= 80.0f * s) {
                    ops.push_back(errorText({innerX, innerY, innerW, 18.0f * s}, error.relatedPath.string(), 0xff94a3b8u, 10.5f * s, 1, 500));
                    innerY += 22.0f * s;
                    ops.push_back(errorFill({innerX, innerY, innerW, 1.0f * s}, 0x16ffffffu, 0.0f));
                    innerY += 10.0f * s;
                }
                const float bodyH = std::max(0.0f, panel.y + panel.height - 12.0f * s - innerY);
                const float bodyFont = 11.0f * s;
                const int maxLines = std::max(1, static_cast<int>(std::floor(bodyH / (bodyFont * 1.42f))));
                const std::string detailBody = !error.detail.empty() ? error.detail : error.diagnosticText();
                ops.push_back(errorText({innerX, innerY, innerW, bodyH}, detailBody, 0xffcbd5e1u, bodyFont, maxLines, 400));
            }
        }

        // 底部分隔线与操作提示
        ops.push_back(errorFill({contentX, footerTop, contentW, 1.0f * s}, 0x16ffffffu, 0.0f));
        const float hintY = footerTop + 13.0f * s;
        if (detailed) {
            const std::string leftHint = error.retryAvailable ? "点击任意处或按 F5 重新加载" : "请修复后重启应用";
            const std::uint32_t leftColor = error.retryAvailable ? 0xff93c5fdu : 0xff94a3b8u;
            ops.push_back(errorText({contentX, hintY, contentW * 0.62f, 18.0f * s}, leftHint, leftColor, 11.0f * s, 1, 500));
            ops.push_back(errorText({contentX + contentW * 0.55f, hintY, contentW * 0.45f, 18.0f * s}, "Ctrl+C 复制诊断", 0xff64748bu, 11.0f * s, 1, 400, core::TextAlign::End));
        } else {
            ops.push_back(errorText({contentX, hintY, contentW, 18.0f * s}, "详细信息已写入诊断日志", 0xff94a3b8u, 11.0f * s, 1, 400));
        }
        return ops;
    }

    std::vector<core::DrawOp> buildErrorScreenOps(core::Rect editorBounds, const ErrorScreenModel& error, bool detailed, float dpiScale) {
        return buildErrorScreenOps(editorBounds, error, detailed ? ErrorScreenPresentation::Detailed : ErrorScreenPresentation::Hidden, dpiScale);
    }
}
