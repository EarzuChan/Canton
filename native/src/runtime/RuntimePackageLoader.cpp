#include <runtime/RuntimePackageLoader.h>


#include <runtime/AppResolver.h>
#include <runtime/DevServerClient.h>

#include <filesystem>
#include <string>

namespace canton::runtime {
    namespace {
        void setLog(RuntimePackageLoadResult& result, core::LogLevel level, std::string message) {
            result.log = RuntimeLoadLog{level, std::move(message)};
        }

        void setToast(RuntimePackageLoadResult& result, core::LogLevel level, std::string title, std::string message, bool coalesce = true) {
            result.toast = RuntimeLoadToast{level, std::move(title), std::move(message), coalesce};
        }
    }

    void RuntimePackageLoader::configureHost(quickjs::QuickJsScriptHost& host, RuntimeLoadResources resources) const {
        if (fonts_) host.setFontRegistry(fonts_);
        host.setNativePainterRegistry(painters_);
        host.setPainterLoader(std::move(resources.painter));
        host.setAssetLoader(std::move(resources.asset));
    }

    RuntimePackageLoadResult RuntimePackageLoader::loadLiveSnapshot(const PackageRuntimeConfig& config, AppResolver& resolver, const quickjs::LiveModuleSnapshot& snapshot, const std::string& error) const {
        RuntimePackageLoadResult result;
        const auto resolved = resolver.resolveDebug(config.app, config.devServerUrl);
        const auto bundleUrl = resolved.devServerUrl;
        result.packageDir = resolved.packageDir;

        if (!resolved.ok || bundleUrl.empty()) {
            result.error = makeErrorScreenModel(ErrorSource::AppPackage, resolved.error.empty() ? "Canton 开发服务器地址无效" : resolved.error, {}, resolved.devServerUrl);
            setToast(result, core::LogLevel::Error, "Live 源无效", result.error->summary);
            return result;
        }

        if (!error.empty()) {
            result.error = makeErrorScreenModel(ErrorSource::ScriptRuntime, error, {}, bundleUrl);
            setToast(result, core::LogLevel::Error, "Live 模块加载失败", error);
            return result;
        }

        auto scriptHost = std::make_unique<canton::quickjs::QuickJsScriptHost>();
        try {
            configureHost(*scriptHost, liveResources_ ? liveResources_(bundleUrl) : RuntimeLoadResources{});
        } catch (const std::exception& failure) {
            result.error = makeErrorScreenModel(ErrorSource::AppPackage, failure.what(), {}, bundleUrl);
            setToast(result, core::LogLevel::Error, "Live 资源来源无效", failure.what());
            return result;
        }
        const auto executed = scriptHost->executeLiveModules(snapshot);
        if (!executed.ok) {
            result.error = makeErrorScreenModel(ErrorSource::ScriptRuntime, executed.error, {}, bundleUrl);
            setToast(result, core::LogLevel::Error, "Live 运行时异常", executed.error);
            return result;
        }
        auto initialTransaction = scriptHost->takePendingTransaction();
        result.initialTransaction = std::move(initialTransaction);
        result.scriptHost = std::move(scriptHost);
        result.ok = true;
        setToast(result, core::LogLevel::Info, "Live App 已启动", bundleUrl);
        return result;
    }

    RuntimePackageLoadResult RuntimePackageLoader::loadDist(const PackageRuntimeConfig& config, AppResolver& resolver) const {
        RuntimePackageLoadResult result;
        const auto resolved = resolver.resolveRelease(config.app);
        if (!resolved.ok) {
            result.error = makeErrorScreenModel(ErrorSource::AppPackage, resolved.error, {}, resolved.packageDir);
            setToast(result, core::LogLevel::Error, "Dist 应用源无效", resolved.error);
            return result;
        }
        result.packageDir = resolved.packageDir;

        auto scriptHost = std::make_unique<canton::quickjs::QuickJsScriptHost>();
        try {
            configureHost(*scriptHost, distResources_ ? distResources_(resolved.package) : RuntimeLoadResources{});
        } catch (const std::exception& failure) {
            result.error = makeErrorScreenModel(ErrorSource::AppPackage, failure.what(), {}, resolved.packageDir);
            setToast(result, core::LogLevel::Error, "Dist 资源来源无效", failure.what());
            return result;
        }
        const auto loaded = scriptHost->executePackage(resolved.package);
        if (!loaded.ok) {
            result.error = makeErrorScreenModel(ErrorSource::ScriptRuntime, loaded.error, {}, resolved.packageDir);
            setToast(result, core::LogLevel::Error, "Dist 运行时异常", loaded.error);
            return result;
        }
        auto initialTransaction = scriptHost->takePendingTransaction();
        result.initialTransaction = std::move(initialTransaction);
        result.scriptHost = std::move(scriptHost);
        result.ok = true;
        setLog(result, core::LogLevel::Info, "Dist App 已启动 " + resolved.packageDir.string());
        return result;
    }
}

