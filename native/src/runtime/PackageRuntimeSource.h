#pragma once

#include <runtime/AppResolver.h>
#include <runtime/LiveModuleClient.h>
#include <runtime/RuntimePackageLoader.h>

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
        IntentKind intentKind = IntentKind::PackageLoad;
        PackageSource activeSource = PackageSource::None;
        std::filesystem::path packageDir;
        std::optional<ErrorScreenModel> error;
        std::vector<RuntimeLoadLog> logs;
        std::vector<RuntimeLoadToast> toasts;
        std::optional<canton::core::MutationTransaction> initialTransaction;
        std::unique_ptr<canton::quickjs::QuickJsScriptHost> scriptHost;
    };

    // 按加载策略决定应用从哪里来。Live 客户端由宿主装配时提供，没有它就只有 dist 一种策略可用
    class PackageRuntimeSource final {
       public:
        using LiveClientFactory = std::function<std::unique_ptr<LiveModuleClient>()>;

        PackageRuntimeSource(AppResolver resolver, RuntimePackageLoader loader, LiveClientFactory liveClient = {}) : resolver_(std::move(resolver)), runtimeLoader_(std::move(loader)), liveClient_(std::move(liveClient)) {}

        ~PackageRuntimeSource();

        PackageLoadOutcome configure(AppSourceConfig config);
        PackageLoadOutcome reload();

        [[nodiscard]] bool usesLive() const noexcept {
            return static_cast<bool>(devServerClient_);
        }

        std::vector<LiveModulePacket> takeLivePackets();
        PackageLoadOutcome completeLiveLoad(LiveModulePacket packet);
        void sendHotMessages(std::vector<quickjs::HotMessage> messages);

        const AppSourceConfig& config() const noexcept {
            return config_;
        }

        PackageSource activeSource() const noexcept {
            return activeSource_;
        }

        // Live 策略下最近一次连接 DevServer 失败或已断开
        bool liveUnavailable() const noexcept {
            return liveUnavailable_;
        }

       private:
        PackageLoadOutcome loadDistPackage();
        void stopDevServerClient();

        AppSourceConfig config_;
        AppResolver resolver_;
        RuntimePackageLoader runtimeLoader_;
        LiveClientFactory liveClient_;
        std::unique_ptr<LiveModuleClient> devServerClient_;
        PackageSource activeSource_ = PackageSource::None;
        bool liveUnavailable_ = false;
    };

}
