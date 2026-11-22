#pragma once

#include <core/Log.h>
#include <cstdint>
#include <runtime/ErrorScreenModel.h>

#include <filesystem>
#include <string>

namespace canton::runtime {

    enum class DiagnosticVisibility { Hidden, DebugOnly, Always };

    struct DiagnosticsConfig {
        DiagnosticVisibility badge = DiagnosticVisibility::DebugOnly;
        DiagnosticVisibility toasts = DiagnosticVisibility::DebugOnly;
        bool errorScreen = true;
        bool copyFullDiagnosticsInRelease = true;
        bool allowScriptDiagnosticsControl = true;
        DiagnosticVisibility overlay = DiagnosticVisibility::DebugOnly;
    };

    struct DiagnosticsBadgeModel {
        std::string text;
        std::uint32_t dot = 0xffef4444u;
    };

    struct DiagnosticsToastModel {
        core::LogLevel level = core::LogLevel::Info;
        std::string title;
        std::string message;
    };

    struct DiagnosticsTextContext {
        std::string activeSource;
        bool liveRuntimeEnabled = false;
        bool hasLive = false;
        bool hasDist = false;
        std::string devServerUrl;
        std::filesystem::path appPath;
        const ErrorScreenModel* error = nullptr;
    };

    [[nodiscard]] bool diagnosticVisibilityEnabled(DiagnosticVisibility visibility) noexcept;

}
