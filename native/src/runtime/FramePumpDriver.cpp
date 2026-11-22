#include <runtime/FramePumpDriver.h>

#include <runtime/CantonRuntime.h>
#include <runtime/DiagnosticsState.h>
#include <runtime/RuntimeSessionState.h>

#include <stdexcept>
#include <string>

namespace canton::runtime {
    bool FramePumpDriver::pumpFrame(CantonRuntime& runtime, RuntimeSessionState& session, DiagnosticsState& diagnostics, canton::core::NodeId root, const std::filesystem::path& frameErrorPath, core::Rect diagnosticsBounds, bool detailedErrorScreen, PackageSource activeSource, bool liveUnavailable, const canton::core::TextLayoutService& textLayout, double nowMillis, const core::FrameFinalizer& finalizeContent) const {
        (void)diagnostics.tick(runtime, nowMillis);
        const auto finalize = [&](const canton::core::NativeScene& scene, canton::core::PublishedFrame& frame) {
            // 首帧为等预加载字体而按住时流水线还没跑过，这时树是空的属正常
            if (session.loaded() && !diagnostics.hasError() && !scene.contains(root) && !runtime.awaitingFontPreload()) {
                throw std::runtime_error("Canton layout tree is empty after loading UI package.");
            }
            diagnostics.prepareFrame(diagnosticsBounds, detailedErrorScreen, activeSource, liveUnavailable, textLayout, frame.content);
            frame.content.errorFrame = diagnostics.hasError() ? std::optional<std::string>(diagnostics.error()->summary) : std::nullopt;
            if (finalizeContent) finalizeContent(scene, frame);
        };
        const auto revision = runtime.publishedFrame().revision;
        auto frame = runtime.pumpFrame(root, session.constraints(), nowMillis, finalize);
        if (frame.ok && !frame.pipelineRan) {
            try {
                (void)runtime.publishRetained(finalize);
            } catch (const std::exception& error) {
                frame.ok = false;
                frame.errorPhase = RuntimeFrameErrorPhase::Pipeline;
                frame.error = error.what();
            }
        }
        if (!frame.ok) {
            // JS 副作用无法回滚，保留上一场景并停止当前上下文，加载新上下文后恢复
            // 原生调用方仍可使用核心层重试
            runtime.suspend();
            (void)session.applyFrameError(frame, diagnostics, runtime, frameErrorPath);
            (void)runtime.publishRetained([&](const auto&, auto& retained) {
                retained.content.errorFrame = frame.error;
                diagnostics.prepareFrame(diagnosticsBounds, detailedErrorScreen, activeSource, liveUnavailable, textLayout, retained.content);
            });
        }
        return runtime.publishedFrame().revision != revision;
    }
}
