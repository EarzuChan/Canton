#pragma once

#include <runtime/AppResolver.h>
#include <runtime/RuntimePackageLoader.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace canton::runtime {

    class CantonRuntime;
    class DiagnosticsState;
    class PackageRuntimeSource;

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

    // Live 与热更新的运行期驱动；实现放在 canton_runtime_live，Dist 宿主不链接
    class LiveSession {
       public:
        virtual ~LiveSession() = default;
        virtual void start(const std::string& devServerUrl) = 0;
        virtual void requestReload() = 0;
        virtual void pump(PackageRuntimeSource& source, CantonRuntime& runtime, DiagnosticsState& diagnostics, const std::function<void(PackageLoadOutcome)>& apply) = 0;
        [[nodiscard]] virtual bool unavailable() const noexcept = 0;
    };

    // 按加载策略决定应用从哪里来。Live 会话由宿主装配时提供，没有它就只有 dist 一种策略可用
    class PackageRuntimeSource final {
       public:
        using LiveSessionFactory = std::function<std::unique_ptr<LiveSession>()>;

        PackageRuntimeSource(AppResolver resolver, RuntimePackageLoader loader, LiveSessionFactory live = {}) : resolver_(std::move(resolver)), runtimeLoader_(std::move(loader)), liveFactory_(std::move(live)) {}

        ~PackageRuntimeSource();

        PackageLoadOutcome configure(AppSourceConfig config);
        PackageLoadOutcome reload();
        void pumpLive(CantonRuntime& runtime, DiagnosticsState& diagnostics, const std::function<void(PackageLoadOutcome)>& apply);

        [[nodiscard]] bool usesLive() const noexcept {
            return static_cast<bool>(live_);
        }

        const AppSourceConfig& config() const noexcept {
            return config_;
        }

        const AppResolver& resolver() const noexcept {
            return resolver_;
        }

        const RuntimePackageLoader& loader() const noexcept {
            return runtimeLoader_;
        }

        PackageSource activeSource() const noexcept {
            return activeSource_;
        }

        void setActiveSource(PackageSource source) noexcept {
            activeSource_ = source;
        }

        // Live 策略下最近一次连接 DevServer 失败或已断开
        bool liveUnavailable() const noexcept {
            return live_ && live_->unavailable();
        }

        PackageLoadOutcome loadDistPackage();
        static PackageLoadOutcome fromLoadResult(RuntimePackageLoadResult loaded, PackageSource source);

       private:
        AppSourceConfig config_;
        AppResolver resolver_;
        RuntimePackageLoader runtimeLoader_;
        LiveSessionFactory liveFactory_;
        std::unique_ptr<LiveSession> live_;
        PackageSource activeSource_ = PackageSource::None;
    };

}
