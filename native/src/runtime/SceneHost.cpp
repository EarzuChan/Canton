#include <runtime/SceneHost.h>

#include <core/InputIntent.h>
#include <core/Layout.h>
#include <runtime/PainterResources.h>
#include <runtime/UiPackage.h>

#include <utility>

namespace canton::runtime {
    namespace {
        AppResolver::MountPackage mounter(const SceneHostOptions& options) {
            return options.mountPackage ? options.mountPackage : AppResolver::MountPackage(mountUiPackage);
        }

        RuntimePackageLoader::DistResources distResources(const SceneHostOptions& options) {
            if (options.distResources) return options.distResources;
            // 资源线程池整个视口共用一份，重载不重建
            return [workers = createResourceWorkers()](std::shared_ptr<const core::UiPackage> package) {
                return RuntimeLoadResources{packagePainterLoader(package, workers), packageAssetLoader(package, workers)};
            };
        }
    }

    SceneHost::SceneHost(const core::TextMeasurer& measurer, std::shared_ptr<core::FontRegistry> fonts, SceneHostOptions options) : options_(std::move(options)), textLayout_(measurer, 256, 8 * 1024 * 1024, fonts.get()), runtime_(core::SceneFramePipeline(core::LayoutEngine(textLayout_))), source_(AppResolver(mounter(options_)), RuntimePackageLoader(options_.liveResources, distResources(options_), fonts, options_.painters), options_.liveClient) {
        diagnostics_.configure(options_.diagnostics);
    }

    SceneHost::~SceneHost() {
        runtime_.setWorkAvailable({});
    }

    void SceneHost::setWorkAvailable(std::function<void()> callback) {
        runtime_.setWorkAvailable(std::move(callback));
    }

    void SceneHost::configure(AppSourceConfig config) {
        reloadRequested_ = false;
        hotUpdateFailed_ = false;
        apply(source_.configure(std::move(config)));
    }

    void SceneHost::resize(int width, int height) {
        bounds_ = {0, 0, static_cast<float>(width), static_cast<float>(height)};
        session_.resize(width, height, runtime_);
    }

    void SceneHost::apply(PackageLoadOutcome outcome) {
        if (!outcome.pending) hotUpdateFailed_ = false;
        binder_.apply(std::move(outcome), session_, runtime_, diagnostics_);
    }

    void SceneHost::failHotUpdate(const std::string& reason) {
        diagnostics_.setError(makeErrorScreenModel(ErrorSource::Hmr, reason, "改好后保存，画面会自动恢复", source_.config().devServerUrl));
        (void)DiagnosticsToast::e(diagnostics_, TAG, "Live 热更新失败", reason);
        hotUpdateFailed_ = true;
        runtime_.enqueueIntent(core::InputIntent::diagnostics());
    }

    void SceneHost::drainLivePackets() {
        for (auto& packet : source_.takeLivePackets()) {
            if (packet.reload) {
                apply(source_.completeLiveLoad(std::move(packet)));
                continue;
            }
            const auto live = source_.activeSource() == PackageSource::Live;
            if (packet.message.type == "update" && (!live || (diagnostics_.hasError() && !hotUpdateFailed_))) {
                // 当前不是 Live，或脚本已经出错停在错误屏：改了文件就整体重载
                apply(source_.reload());
                continue;
            }
            if (packet.error.empty() && live) {
                const auto result = runtime_.applyHotUpdate(packet.snapshot, packet.message);
                if (!result.ok)
                    packet.error = result.error;
                else if (hotUpdateFailed_ && packet.message.type == "update") {
                    hotUpdateFailed_ = false;
                    diagnostics_.clearError();
                    (void)DiagnosticsToast::i(diagnostics_, TAG, "热更新已恢复", source_.config().devServerUrl);
                    runtime_.enqueueIntent(core::InputIntent::diagnostics());
                }
            }
            if (!packet.error.empty()) failHotUpdate(packet.error);
        }
        source_.sendHotMessages(runtime_.takeHotMessages());
    }

    DiagnosticsBadgeModel SceneHost::badgeModel() const {
        switch (source_.activeSource()) {
            case PackageSource::Live:
                return {"LIVE", 0xff22c55eu};
            case PackageSource::Dist:
                // 策略本想用 Live 却落在 Dist 上，角标要看得出来
                return source_.liveUnavailable() ? DiagnosticsBadgeModel{"DIST FALLBACK", 0xfff59e0bu} : DiagnosticsBadgeModel{"DIST", 0xff3b82f6u};
            case PackageSource::None:
                break;
        }
        return {"NO APP", 0xffef4444u};
    }

    bool SceneHost::pumpFrame(double nowMillis) {
        drainLivePackets();
        if (runtime_.consumeReloadRequest() || std::exchange(reloadRequested_, false)) apply(source_.reload());
        // 诊断层与输入叠层的文字在发布前排好版；没指定字体族的用配置的诊断字体
        const auto prepare = [this](const core::NativeScene&, core::PublishedFrame& frame) {
            auto& content = frame.content;
            for (auto* ops : {&content.overlayDrawOps, &content.diagnosticsErrorDrawOps, &content.diagnosticsBadgeDrawOps, &content.diagnosticsFpsDrawOps, &content.diagnosticsToastDrawOps})
                for (auto& op : *ops) {
                    if (op.type != core::DrawOpType::DrawText) continue;
                    if (ops != &content.overlayDrawOps && op.textStyle.fontFamily.empty() && !options_.diagnosticsFontFamily.empty()) {
                        op.textStyle.fontFamily = options_.diagnosticsFontFamily;
                        op.textLayout.reset();
                    }
                    core::DrawOpsBuilder::prepareText(op, textLayout_);
                }
        };
        const auto related = source_.activeSource() == PackageSource::Dist ? source_.config().distOverride : std::filesystem::path();
        const auto changed = framePump_.pumpFrame(runtime_, session_, diagnostics_, rootNodeId, related, bounds_, options_.detailedErrorScreen, badgeModel(), textLayout_, nowMillis, prepare);
        if (fpsEnabled_) {
            // 每半秒统计一次发布帧数
            if (changed) ++fpsFrames_;
            if (nowMillis - fpsWindowStart_ >= 500.0) {
                diagnostics_.setFps(ready() ? static_cast<int>(fpsFrames_ * 1000.0 / (nowMillis - fpsWindowStart_) + 0.5) : 0);
                fpsWindowStart_ = nowMillis;
                fpsFrames_ = 0;
                runtime_.enqueueIntent(core::InputIntent::diagnostics());
            }
        }
        return changed;
    }

    bool SceneHost::wantsFrame() const {
        return reloadRequested_ || diagnostics_.hasActiveToasts() || runtime_.hasPendingFrameWork() || fpsEnabled_;
    }

    bool SceneHost::ready() const {
        return session_.interactive(diagnostics_) && runtime_.publishedFrame().revision > 0;
    }

    void SceneHost::setFpsBadgeEnabled(bool enabled) {
        if (fpsEnabled_ == enabled) return;
        fpsEnabled_ = enabled;
        fpsFrames_ = 0;
        fpsWindowStart_ = 0;
        diagnostics_.setFps(enabled ? std::optional<int>(0) : std::nullopt);
        runtime_.enqueueIntent(core::InputIntent::diagnostics());
    }
}
