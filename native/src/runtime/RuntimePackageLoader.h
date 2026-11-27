#pragma once

#include <core/MutationTransaction.h>
#include <functional>
#include <runtime/AppResolver.h>
#include <runtime/ErrorScreenModel.h>

#include <quickjs/QuickJsScriptHost.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace canton::runtime {
    struct RuntimeLoadResources {
        core::PainterLoader painter;
        core::AssetLoader asset;
        core::AssetExistsChecker assetExists;
    };

    struct RuntimeLoadLog {
        core::LogLevel level = core::LogLevel::Info;
        std::string message;
    };

    struct RuntimeLoadToast {
        core::LogLevel level = core::LogLevel::Info;
        std::string title;
        std::string message;
        bool coalesce = true;
    };

    struct RuntimePackageLoadResult {
        bool ok = false;
        std::filesystem::path packageDir;
        std::optional<ErrorScreenModel> error;
        std::optional<RuntimeLoadLog> log;
        std::optional<RuntimeLoadToast> toast;
        std::optional<canton::core::MutationTransaction> initialTransaction;
        std::unique_ptr<canton::quickjs::QuickJsScriptHost> scriptHost;
    };

    class RuntimePackageLoader final {
       public:
        using LiveResources = std::function<RuntimeLoadResources(std::string_view)>;
        using DistResources = std::function<RuntimeLoadResources(std::shared_ptr<const core::UiPackage>)>;

        RuntimePackageLoader(
            DistResources distResources,
            std::shared_ptr<core::FontRegistry> fonts,
            std::shared_ptr<const core::NativePainterRegistry> painters,
            std::shared_ptr<const core::ShaderCompiler> shaderCompiler = {},
            std::shared_ptr<quickjs::PlatformBridge> platformBridge = {})
            : distResources_(std::move(distResources)),
              fonts_(std::move(fonts)),
              painters_(std::move(painters)),
              shaderCompiler_(std::move(shaderCompiler)),
              platformBridge_(std::move(platformBridge)) {}

        [[nodiscard]] std::unique_ptr<quickjs::QuickJsScriptHost> createHost(RuntimeLoadResources resources) const;
        RuntimePackageLoadResult loadDist(const ResolvedDist& dist) const;

       private:
        DistResources distResources_;
        std::shared_ptr<core::FontRegistry> fonts_;
        std::shared_ptr<const core::NativePainterRegistry> painters_;
        std::shared_ptr<const core::ShaderCompiler> shaderCompiler_;
        std::shared_ptr<quickjs::PlatformBridge> platformBridge_;
    };

}
