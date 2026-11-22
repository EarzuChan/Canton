#include <runtime/RuntimePackageLoader.h>

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

        std::string display(const std::filesystem::path& path) {
            const auto text = path.u8string();
            return {text.begin(), text.end()};
        }
    }

    void RuntimePackageLoader::configureHost(quickjs::QuickJsScriptHost& host, RuntimeLoadResources resources) const {
        if (fonts_) host.setFontRegistry(fonts_);
        host.setNativePainterRegistry(painters_);
        host.setPainterLoader(std::move(resources.painter));
        host.setAssetLoader(std::move(resources.asset));
    }

    RuntimePackageLoadResult RuntimePackageLoader::loadLiveSnapshot(const std::string& devServerUrl, const quickjs::LiveModuleSnapshot& snapshot, const std::string& error) const {
        RuntimePackageLoadResult result;
        if (devServerUrl.empty()) {
            result.error = makeErrorScreenModel(ErrorSource::AppPackage, "Canton 开发服务器地址无效");
            setToast(result, core::LogLevel::Error, "Live 源无效", result.error->summary);
            return result;
        }

        if (!error.empty()) {
            result.error = makeErrorScreenModel(ErrorSource::ScriptLoad, error, {}, devServerUrl);
            setToast(result, core::LogLevel::Error, "Live 模块加载失败", error);
            return result;
        }

        auto scriptHost = std::make_unique<canton::quickjs::QuickJsScriptHost>();
        try {
            configureHost(*scriptHost, liveResources_ ? liveResources_(devServerUrl) : RuntimeLoadResources{});
        } catch (const std::exception& failure) {
            result.error = makeErrorScreenModel(ErrorSource::AppPackage, failure.what(), {}, devServerUrl);
            setToast(result, core::LogLevel::Error, "Live 资源来源无效", failure.what());
            return result;
        }
        const auto executed = scriptHost->executeLiveModules(snapshot);
        if (!executed.ok) {
            result.error = makeErrorScreenModel(ErrorSource::ScriptRuntime, executed.error, {}, devServerUrl);
            setToast(result, core::LogLevel::Error, "Live 运行时异常", executed.error);
            return result;
        }
        auto initialTransaction = scriptHost->takePendingTransaction();
        result.initialTransaction = std::move(initialTransaction);
        result.scriptHost = std::move(scriptHost);
        result.ok = true;
        setToast(result, core::LogLevel::Info, "Live App 已启动", devServerUrl);
        return result;
    }

    RuntimePackageLoadResult RuntimePackageLoader::loadDist(const ResolvedDist& dist) const {
        RuntimePackageLoadResult result;
        result.packageDir = dist.location;
        if (!dist.ok) {
            result.error = makeErrorScreenModel(ErrorSource::AppPackage, dist.error, {}, dist.location);
            setToast(result, core::LogLevel::Error, "Dist 应用源无效", dist.error);
            return result;
        }

        auto scriptHost = std::make_unique<canton::quickjs::QuickJsScriptHost>();
        try {
            configureHost(*scriptHost, distResources_ ? distResources_(dist.package) : RuntimeLoadResources{});
        } catch (const std::exception& failure) {
            result.error = makeErrorScreenModel(ErrorSource::AppPackage, failure.what(), {}, dist.location);
            setToast(result, core::LogLevel::Error, "Dist 资源来源无效", failure.what());
            return result;
        }
        const auto loaded = scriptHost->executePackage(dist.package);
        if (!loaded.ok) {
            result.error = makeErrorScreenModel(ErrorSource::ScriptRuntime, loaded.error, {}, dist.location);
            setToast(result, core::LogLevel::Error, "Dist 运行时异常", loaded.error);
            return result;
        }
        auto initialTransaction = scriptHost->takePendingTransaction();
        result.initialTransaction = std::move(initialTransaction);
        result.scriptHost = std::move(scriptHost);
        result.ok = true;
        setLog(result, core::LogLevel::Info, "Dist App 已启动 " + display(dist.location));
        return result;
    }
}
