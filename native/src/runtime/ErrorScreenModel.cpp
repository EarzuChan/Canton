#include <runtime/ErrorScreenModel.h>

#include <algorithm>
#include <sstream>
#include <utility>

namespace canton::runtime {
    namespace {
        core::DrawOp errorFill(core::Rect rect, std::uint32_t color, float radius) {
            core::DrawOp op;
            op.type = core::DrawOpType::FillRect;
            op.rect = rect;
            op.color = color;
            op.shape = core::DrawShapeType::Rounded;
            op.corners = core::CornerRadii::uniform(radius);
            return op;
        }

        core::DrawOp errorText(core::Rect rect, std::string value, std::uint32_t color, float fontSize, int maxLines = 1) {
            core::DrawOp op;
            op.type = core::DrawOpType::DrawText;
            op.rect = rect;
            op.color = color;
            op.textStyle.fontSize = fontSize;
            op.textStyle.lineHeight = fontSize * core::defaultLineHeightRatio;
            op.maxLines = maxLines;
            op.text = std::move(value);
            op.overflow = core::TextOverflow::Clip;
            return op;
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

    std::vector<core::DrawOp> buildErrorScreenOps(core::Rect editorBounds, const ErrorScreenModel& error, bool detailed, float dpiScale) {
        const float s = dpiScale > 0.0f ? dpiScale : 1.0f;
        std::vector<core::DrawOp> ops;
        core::Rect area{editorBounds.x + 20.0f * s, editorBounds.y + 20.0f * s, std::max(0.0f, editorBounds.width - 40.0f * s), std::max(0.0f, editorBounds.height - 40.0f * s)};
        ops.push_back(errorFill(area, detailed ? 0xff2a1014u : 0xff111827u, 10.0f * s));
        if (detailed) {
            const core::Rect titleArea{area.x, area.y, area.width, 34.0f * s};
            ops.push_back(errorText(titleArea, error.title.empty() ? "Canton Error" : error.title, 0xffff6b6bu, 15.0f * s));
            area.y += 34.0f * s;
            area.height = std::max(0.0f, area.height - 34.0f * s);
            auto body = error.diagnosticText();
            if (error.retryAvailable) body += "\nClick anywhere to retry.";
            ops.push_back(errorText(area, std::move(body), 0xe0ffffffu, 11.0f * s, 8));
            return ops;
        }
        const core::Rect titleArea{area.x, area.y, area.width, 32.0f * s};
        ops.push_back(errorText(titleArea, "Canton error screen hidden", 0xfff59e0bu, 14.5f * s));
        area.y += 32.0f * s;
        area.height = std::max(0.0f, area.height - 32.0f * s);
        ops.push_back(errorText(area, error.summary + "\nError details are still available in logs and diagnostics.", 0xc7ffffffu, 11.0f * s, 4));
        return ops;
    }
}
