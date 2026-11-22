#pragma once

#include <filesystem>
#include <functional>
#include <utility>
#include <core/UiPackage.h>
#include <memory>
#include <string>
#include <string_view>
#include "App.h"

namespace canton::runtime {
    struct ResolvedApp {
        bool ok = false;
        bool devServer = false;
        std::filesystem::path packageDir;
        std::shared_ptr<const core::UiPackage> package;
        std::string devServerUrl;
        std::string error;
    };

    class AppResolver {
       public:
        static constexpr const char* DefaultDevServer = "http://127.0.0.1:9178";

        using MountPackage = std::function<std::shared_ptr<const core::UiPackage>(const std::filesystem::path&)>;

        AppResolver(std::filesystem::path resourceRoot, MountPackage mountPackage)
            : resourceRoot_(std::move(resourceRoot)), mountPackage_(std::move(mountPackage)) {}

        ResolvedApp resolvePackage(const App& app) const;

        ResolvedApp resolveRelease(const App& app) const {
            return resolvePackage(app);
        }

        ResolvedApp resolveDebug(const App& app, std::string_view explicitDevServerUrl = {}) const;
        std::string devServerUrlFromEnvironment(std::string_view explicitDevServerUrl = {}) const;
       private:
        std::filesystem::path resolvePackageDir(const std::filesystem::path& configuredPath) const;
        std::filesystem::path resourceRoot_;
        MountPackage mountPackage_;
    };
}
