#include <runtime/PackageRuntimeSource.h>

#include <utility>

namespace canton::runtime {
    namespace {
        RuntimeLoadToast makeToast(core::LogLevel level, std::string title, std::string message, bool coalesce = true) {
            return RuntimeLoadToast{level, std::move(title), std::move(message), coalesce};
        }
    }

    const char* packageSourceLabel(PackageSource source) noexcept {
        switch (source) {
            case PackageSource::Live:
                return "live";
            case PackageSource::Dist:
                return "dist";
            case PackageSource::None:
                return "none";
        }
        return "none";
    }

    PackageLoadOutcome PackageRuntimeSource::fromLoadResult(RuntimePackageLoadResult loaded, PackageSource source) {
        PackageLoadOutcome outcome;
        outcome.loaded = loaded.ok;
        outcome.activeSource = loaded.ok ? source : PackageSource::None;
        outcome.packageDir = std::move(loaded.packageDir);
        outcome.error = std::move(loaded.error);
        if (loaded.log) outcome.logs.push_back(std::move(*loaded.log));
        if (loaded.toast) outcome.toasts.push_back(std::move(*loaded.toast));
        outcome.initialTransaction = std::move(loaded.initialTransaction);
        outcome.scriptHost = std::move(loaded.scriptHost);
        return outcome;
    }

    PackageRuntimeSource::~PackageRuntimeSource() = default;

    PackageLoadOutcome PackageRuntimeSource::configure(AppSourceConfig config) {
        config_ = std::move(config);
        live_.reset();
        activeSource_ = PackageSource::None;

        if (config_.policy == LoadPolicy::OnlyDist) {
            auto outcome = loadDistPackage();
            outcome.intentKind = PackageLoadOutcome::IntentKind::PackageLoad;
            return outcome;
        }
        if (liveFactory_) live_ = liveFactory_();
        if (!live_) {
            PackageLoadOutcome outcome;
            outcome.error = makeErrorScreenModel(ErrorSource::AppPackage, std::string("这个宿主不含 Live 客户端，不能使用 ") + loadPolicyLabel(config_.policy) + " 策略", {}, {}, false);
            outcome.toasts.push_back(makeToast(core::LogLevel::Error, "加载策略不可用", outcome.error->summary));
            return outcome;
        }
        live_->start(config_.devServerUrl);
        PackageLoadOutcome outcome;
        outcome.pending = true;
        return outcome;
    }

    PackageLoadOutcome PackageRuntimeSource::reload() {
        if (live_) {
            PackageLoadOutcome outcome;
            outcome.pending = true;
            live_->requestReload();
            return outcome;
        }
        auto outcome = loadDistPackage();
        outcome.intentKind = PackageLoadOutcome::IntentKind::Reload;
        outcome.toasts.insert(outcome.toasts.begin(), makeToast(core::LogLevel::Info, "已请求重新加载", "正在重新加载 Canton 应用包"));
        return outcome;
    }

    void PackageRuntimeSource::pumpLive(CantonRuntime& runtime, DiagnosticsState& diagnostics, const std::function<void(PackageLoadOutcome)>& apply) {
        if (live_) live_->pump(*this, runtime, diagnostics, apply);
    }

    PackageLoadOutcome PackageRuntimeSource::loadDistPackage() {
        auto outcome = fromLoadResult(runtimeLoader_.loadDist(resolver_.resolveDist(config_)), PackageSource::Dist);
        activeSource_ = outcome.loaded ? PackageSource::Dist : PackageSource::None;
        return outcome;
    }
}
