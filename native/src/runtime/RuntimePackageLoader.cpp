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

    std::unique_ptr<quickjs::QuickJsScriptHost> RuntimePackageLoader::createHost(RuntimeLoadResources resources) const {
        auto host = std::make_unique<canton::quickjs::QuickJsScriptHost>();
        if (fonts_) host->setFontRegistry(fonts_);
        host->setNativePainterRegistry(painters_);
        host->setShaderCompiler(shaderCompiler_);
        host->setPainterLoader(std::move(resources.painter));
        host->setAssetLoader(std::move(resources.asset));
        return host;
    }

    RuntimePackageLoadResult RuntimePackageLoader::loadDist(const ResolvedDist& dist) const {
        RuntimePackageLoadResult result;
        result.packageDir = dist.location;
        if (!dist.ok) {
            result.error = makeErrorScreenModel(ErrorSource::AppPackage, dist.error, {}, dist.location);
            setToast(result, core::LogLevel::Error, "Dist 应用源无效", dist.error);
            return result;
        }

        std::unique_ptr<canton::quickjs::QuickJsScriptHost> scriptHost;
        try {
            scriptHost = createHost(distResources_ ? distResources_(dist.package) : RuntimeLoadResources{});
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
