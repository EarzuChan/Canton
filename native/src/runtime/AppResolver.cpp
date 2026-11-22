#include <runtime/AppResolver.h>

#include <cstdlib>
#include <string>

namespace canton::runtime {
    std::filesystem::path AppResolver::resolvePackageDir(const std::filesystem::path& configuredPath) const {
        if (configuredPath.is_absolute()) return configuredPath.lexically_normal();
        if (resourceRoot_.empty()) return {};
        return (resourceRoot_ / configuredPath).lexically_normal();
    }

    ResolvedApp AppResolver::resolvePackage(const App& app) const {
        ResolvedApp result;
        if (!app.hasDist()) {
            result.error = "你啥也没给我给你加载啥app（笑），请调用`config.app.useDist(...)`喵";
            return result;
        }

        result.packageDir = resolvePackageDir(app.distPath());
        if (result.packageDir.empty())
            result.error = "找不到 Canton 产品资源根";
        else if (!mountPackage_)
            result.error = "宿主未提供 UI 包挂载器";
        else
            try {
                result.package = mountPackage_(result.packageDir);
                result.ok = static_cast<bool>(result.package);
                if (!result.ok) result.error = "无法挂载 UI 包";
            } catch (const std::exception& error) {
                result.error = error.what();
            }
        return result;
    }

    ResolvedApp AppResolver::resolveDebug(const App& app, std::string_view explicitDevServerUrl) const {
        ResolvedApp result;
        result.ok = true;
        result.devServer = true;
        result.devServerUrl = devServerUrlFromEnvironment(explicitDevServerUrl.empty() ? app.liveUrl() : explicitDevServerUrl);

        const auto distPath = app.hasDist() ? app.distPath() : std::filesystem::path("ui");
        result.packageDir = resolvePackageDir(distPath);
        return result;
    }

    std::string AppResolver::devServerUrlFromEnvironment(std::string_view explicitDevServerUrl) const {
        if (!explicitDevServerUrl.empty()) return std::string(explicitDevServerUrl);
        if (const char* env = std::getenv("CANTON_DEV_SERVER"))
            if (*env != '\0') return std::string(env);
        return DefaultDevServer;
    }

}
