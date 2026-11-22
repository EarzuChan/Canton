#pragma once

#include <runtime/AppResolver.h>
#include <runtime/RuntimePackageLoader.h>
#include <runtime/LiveModuleClient.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace canton::runtime {

    enum class PackageSource {
        None,
        Live,
        Dist,
    };

    const char* packageSourceLabel(PackageSource source) noexcept;

    struct PackageLoadOutcome {
        enum class IntentKind {
            PackageLoad,
            Reload,
            HmrReload,
        };

        bool loaded = false;
        bool pending = false;
        bool serverUnavailable = false;
        IntentKind intentKind = IntentKind::PackageLoad;
        PackageSource activeSource = PackageSource::None;
        std::filesystem::path packageDir;
        std::optional<ErrorScreenModel> error;
        std::vector<RuntimeLoadLog> logs;
        std::vector<RuntimeLoadToast> toasts;
        std::optional<canton::core::MutationTransaction> initialTransaction;
        std::unique_ptr<canton::quickjs::QuickJsScriptHost> scriptHost;
    };

    class PackageRuntimeSource final {
       public:
        using LiveClientFactory = std::function<std::unique_ptr<LiveModuleClient>()>;

        PackageRuntimeSource(AppResolver resolver, RuntimePackageLoader loader, LiveClientFactory liveClient)
            : resolver_(std::move(resolver)), runtimeLoader_(std::move(loader)), liveClient_(std::move(liveClient)) {}
        ~PackageRuntimeSource();

        PackageLoadOutcome configure(PackageRuntimeConfig config);
        PackageLoadOutcome reload();
        PackageLoadOutcome reloadFromDevServer();
        PackageLoadOutcome manualReload(bool toggleLive);

        bool wantsReloadPolling() const;
        std::vector<LiveModulePacket> takeLivePackets();
        PackageLoadOutcome completeLiveLoad(LiveModulePacket packet);
        void sendHotMessages(std::vector<quickjs::HotMessage> messages);

        const PackageRuntimeConfig& config() const noexcept {
            return config_;
        }

        PackageSource activeSource() const noexcept {
            return activeSource_;
        }

        bool liveRuntimeEnabled() const noexcept {
            return liveRuntimeEnabled_;
        }

        bool lastLiveUnavailable() const noexcept {
            return lastLiveUnavailable_;
        }

       private:
        PackageLoadOutcome loadConfiguredPackage();
        PackageLoadOutcome loadLivePackage();
        PackageLoadOutcome loadDistPackage();
        PackageLoadOutcome noSourceOutcome() const;
        PackageLoadOutcome disabledLiveWithoutDistOutcome() const;

        void startDevServerClientIfNeeded();
        void stopDevServerClient();
        void prependToasts(PackageLoadOutcome& outcome, std::vector<RuntimeLoadToast> toasts) const;

        PackageRuntimeConfig config_;
        AppResolver resolver_;
        RuntimePackageLoader runtimeLoader_;
        LiveClientFactory liveClient_;
        std::unique_ptr<LiveModuleClient> devServerClient_;
        PackageSource activeSource_ = PackageSource::None;
        bool liveRuntimeEnabled_ = false;
        bool lastLiveUnavailable_ = false;
    };

}
