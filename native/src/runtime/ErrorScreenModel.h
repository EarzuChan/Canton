#pragma once

#include <core/Paint.h>

#include <filesystem>
#include <string>
#include <vector>

namespace canton::runtime {
    enum class ErrorSource {
        Unknown,
        AppPackage,
        ScriptLoad,
        ScriptRuntime,
        NativeTransaction,
        Hmr,
        Resource,
    };

    struct ErrorScreenModel {
        ErrorSource source = ErrorSource::Unknown;
        std::string title;
        std::string summary;
        std::string detail;
        std::filesystem::path relatedPath;
        bool retryAvailable = true;

        std::string diagnosticText() const;
    };

    const char* toString(ErrorSource source) noexcept;
    ErrorScreenModel makeErrorScreenModel(ErrorSource source, std::string summary, std::string detail = {}, std::filesystem::path relatedPath = {}, bool retryAvailable = true);
    [[nodiscard]] std::vector<core::DrawOp> buildErrorScreenOps(core::Rect editorBounds, const ErrorScreenModel& error, bool detailed, float dpiScale = 1.0f);
}
