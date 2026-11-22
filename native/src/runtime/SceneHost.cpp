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

    SceneHost::SceneHost(const core::TextMeasurer& measurer, std::shared_ptr<core::FontRegistry> fonts, SceneHostOptions options) : diagnosticsFontFamily_(std::move(options.diagnosticsFontFamily)), detailedErrorScreen_(options.detailedErrorScreen), textLayout_(measurer, 256, 8 * 1024 * 1024, fonts.get()), runtime_(core::SceneFramePipeline(core::LayoutEngine(textLayout_))), source_(AppResolver(mounter(options)), RuntimePackageLoader(distResources(options), fonts, options.painters), std::move(options.live)) {
        diagnostics_.setHud(std::move(options.hud));
    }

    SceneHost::~SceneHost() {
        runtime_.setWorkAvailable({});
    }

    void SceneHost::setWorkAvailable(std::function<void()> callback) {
        runtime_.setWorkAvailable(std::move(callback));
    }

    void SceneHost::configure(AppSourceConfig config) {
        reloadRequested_ = false;
        apply(source_.configure(std::move(config)));
    }

    void SceneHost::resize(int width, int height) {
        bounds_ = {0, 0, static_cast<float>(width), static_cast<float>(height)};
        session_.resize(width, height, runtime_);
    }

    void SceneHost::apply(PackageLoadOutcome outcome) {
        binder_.apply(std::move(outcome), session_, runtime_, diagnostics_);
    }

    bool SceneHost::pumpFrame(double nowMillis) {
        source_.pumpLive(runtime_, diagnostics_, [this](PackageLoadOutcome outcome) { apply(std::move(outcome)); });
        if (runtime_.consumeReloadRequest() || std::exchange(reloadRequested_, false)) apply(source_.reload());
        // 诊断层与输入叠层的文字在发布前排好版；没指定字体族的用配置的诊断字体
        const auto prepare = [this](const core::NativeScene&, core::PublishedFrame& frame) {
            auto& content = frame.content;
            for (auto* ops : {&content.overlayDrawOps, &content.diagnosticsErrorDrawOps, &content.diagnosticsBadgeDrawOps, &content.diagnosticsFpsDrawOps, &content.diagnosticsToastDrawOps})
                for (auto& op : *ops) {
                    if (op.type != core::DrawOpType::DrawText) continue;
                    if (ops != &content.overlayDrawOps && op.textStyle.fontFamily.empty() && !diagnosticsFontFamily_.empty()) {
                        op.textStyle.fontFamily = diagnosticsFontFamily_;
                        op.textLayout.reset();
                    }
                    core::DrawOpsBuilder::prepareText(op, textLayout_);
                }
        };
        const auto related = source_.activeSource() == PackageSource::Dist ? source_.config().distOverride : std::filesystem::path();
        const auto changed = framePump_.pumpFrame(runtime_, session_, diagnostics_, rootNodeId, related, bounds_, detailedErrorScreen_, source_.activeSource(), source_.liveUnavailable(), textLayout_, nowMillis, prepare);
        diagnostics_.recordFrame(changed, ready(), nowMillis, runtime_);
        return changed;
    }

    bool SceneHost::wantsFrame() const {
        return reloadRequested_ || diagnostics_.hasActiveToasts() || runtime_.hasPendingFrameWork() || diagnostics_.fpsEnabled();
    }

    bool SceneHost::ready() const {
        return session_.interactive(diagnostics_) && runtime_.publishedFrame().revision > 0;
    }
}
