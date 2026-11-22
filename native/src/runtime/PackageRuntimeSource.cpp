#include <runtime/PackageRuntimeSource.h>

#include <iterator>
#include <utility>

namespace canton::runtime {
    namespace {
        RuntimeLoadToast makeToast(core::LogLevel level, std::string title, std::string message, bool coalesce = true) {
            return RuntimeLoadToast{level, std::move(title), std::move(message), coalesce};
        }

        PackageLoadOutcome packageOutcomeFromRuntimeLoad(RuntimePackageLoadResult loaded, PackageSource source) {
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

        void prependToast(PackageLoadOutcome& outcome, RuntimeLoadToast toast) {
            outcome.toasts.insert(outcome.toasts.begin(), std::move(toast));
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

    PackageRuntimeSource::~PackageRuntimeSource() {
        stopDevServerClient();
    }

    PackageLoadOutcome PackageRuntimeSource::configure(AppSourceConfig config) {
        config_ = std::move(config);
        stopDevServerClient();
        liveUnavailable_ = false;
        activeSource_ = PackageSource::None;

        if (config_.policy == LoadPolicy::OnlyDist) {
            auto outcome = loadDistPackage();
            outcome.intentKind = PackageLoadOutcome::IntentKind::PackageLoad;
            return outcome;
        }
        if (liveClient_) devServerClient_ = liveClient_();
        if (!devServerClient_) {
            PackageLoadOutcome outcome;
            outcome.error = makeErrorScreenModel(ErrorSource::AppPackage, std::string("这个宿主不含 Live 客户端，不能使用 ") + loadPolicyLabel(config_.policy) + " 策略", {}, {}, false);
            outcome.toasts.push_back(makeToast(core::LogLevel::Error, "加载策略不可用", outcome.error->summary));
            return outcome;
        }
        // 客户端连上后自行取首个模块快照，结果经 takeLivePackets 交回
        devServerClient_->start(config_.devServerUrl);
        PackageLoadOutcome outcome;
        outcome.pending = true;
        return outcome;
    }

    PackageLoadOutcome PackageRuntimeSource::reload() {
        if (devServerClient_) {
            PackageLoadOutcome outcome;
            outcome.pending = true;
            devServerClient_->requestReload();
            return outcome;
        }
        auto outcome = loadDistPackage();
        outcome.intentKind = PackageLoadOutcome::IntentKind::Reload;
        prependToast(outcome, makeToast(core::LogLevel::Info, "已请求重新加载", "正在重新加载 Canton 应用包"));
        return outcome;
    }

    std::vector<LiveModulePacket> PackageRuntimeSource::takeLivePackets() {
        return devServerClient_ ? devServerClient_->takePackets() : std::vector<LiveModulePacket>{};
    }

    void PackageRuntimeSource::sendHotMessages(std::vector<quickjs::HotMessage> messages) {
        if (devServerClient_)
            for (const auto& message : messages) devServerClient_->send(message);
    }

    PackageLoadOutcome PackageRuntimeSource::completeLiveLoad(LiveModulePacket packet) {
        if (packet.unavailable) {
            liveUnavailable_ = true;
            if (config_.policy == LoadPolicy::LiveThenDist && !resolver_.locateDist(config_).missing) {
                auto outcome = loadDistPackage();
                outcome.intentKind = PackageLoadOutcome::IntentKind::Reload;
                // 回退必须让开发者看得见，免得误以为跑的是 Live
                prependToast(outcome, makeToast(core::LogLevel::Warn, "Live 不可用，已回退到 Dist", packet.error + "；DevServer 恢复后自动切回 Live", false));
                return outcome;
            }
            PackageLoadOutcome outcome;
            outcome.intentKind = PackageLoadOutcome::IntentKind::Reload;
            activeSource_ = PackageSource::None;
            const auto detail = config_.policy == LoadPolicy::LiveThenDist ? "也没有可回退的 Dist 包。启动 DevServer 后会自动连接" : "启动 DevServer 后会自动连接";
            outcome.error = makeErrorScreenModel(ErrorSource::AppPackage, "连不上 Live DevServer：" + config_.devServerUrl, packet.error + "\n" + detail, {}, true);
            outcome.toasts.push_back(makeToast(core::LogLevel::Error, "Live 不可用", packet.error));
            return outcome;
        }
        auto outcome = packageOutcomeFromRuntimeLoad(runtimeLoader_.loadLiveSnapshot(config_.devServerUrl, packet.snapshot, packet.error), PackageSource::Live);
        outcome.intentKind = PackageLoadOutcome::IntentKind::HmrReload;
        liveUnavailable_ = false;
        activeSource_ = outcome.loaded ? PackageSource::Live : PackageSource::None;
        return outcome;
    }

    PackageLoadOutcome PackageRuntimeSource::loadDistPackage() {
        auto outcome = packageOutcomeFromRuntimeLoad(runtimeLoader_.loadDist(resolver_.resolveDist(config_)), PackageSource::Dist);
        activeSource_ = outcome.loaded ? PackageSource::Dist : PackageSource::None;
        return outcome;
    }

    void PackageRuntimeSource::stopDevServerClient() {
        devServerClient_.reset();
    }
}
