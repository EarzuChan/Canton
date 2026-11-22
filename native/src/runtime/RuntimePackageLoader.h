#pragma once

#include <core/MutationTransaction.h>
#include <runtime/App.h>
#include <functional>
#include <runtime/ErrorScreenModel.h>

#include <quickjs/QuickJsScriptHost.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace canton::runtime {
    class AppResolver;

    struct PackageRuntimeConfig {
        App app;
        bool preferDevServer = false;
        std::string devServerUrl;
    };

    struct RuntimeLoadResources {
        core::PainterLoader painter;
        core::AssetLoader asset;
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
        bool serverUnavailable = false;
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

        RuntimePackageLoader(LiveResources liveResources, DistResources distResources,
                             std::shared_ptr<core::FontRegistry> fonts, std::shared_ptr<const core::NativePainterRegistry> painters)
            : liveResources_(std::move(liveResources)), distResources_(std::move(distResources)),
              fonts_(std::move(fonts)), painters_(std::move(painters)) {}

        RuntimePackageLoadResult loadLiveSnapshot(const PackageRuntimeConfig& config, AppResolver& resolver, const quickjs::LiveModuleSnapshot& snapshot, const std::string& error) const;
        RuntimePackageLoadResult loadDist(const PackageRuntimeConfig& config, AppResolver& resolver) const;
       private:
        void configureHost(quickjs::QuickJsScriptHost& host, RuntimeLoadResources resources) const;
        LiveResources liveResources_;
        DistResources distResources_;
        std::shared_ptr<core::FontRegistry> fonts_;
        std::shared_ptr<const core::NativePainterRegistry> painters_;
    };

}
