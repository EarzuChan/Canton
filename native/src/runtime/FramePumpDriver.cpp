#include <runtime/FramePumpDriver.h>

#include <runtime/CantonRuntime.h>
#include <runtime/DiagnosticsState.h>
#include <runtime/RuntimeSessionState.h>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace canton::runtime {
    namespace {
        void replacePreparedOps(std::vector<canton::core::DrawOp>& target, std::vector<canton::core::DrawOp> source) {
            for (std::size_t index = 0; index < std::min(target.size(), source.size()); ++index) {
                auto& next = source[index];
                const auto& previous = target[index];
                if (next.type == canton::core::DrawOpType::DrawText && previous.type == canton::core::DrawOpType::DrawText && !next.textLayout && next.text == previous.text && next.textStyle == previous.textStyle && next.rect.width == previous.rect.width && next.maxLines == previous.maxLines && next.overflow == previous.overflow) next.textLayout = previous.textLayout;
            }
            target = std::move(source);
        }
    }

    bool FramePumpDriver::pumpFrame(CantonRuntime& runtime, RuntimeSessionState& session, DiagnosticsState& diagnostics, canton::core::NodeId root, const std::filesystem::path& frameErrorPath, core::Rect diagnosticsBounds, bool detailedErrorScreen, const DiagnosticsBadgeModel& badgeModel, const canton::core::TextLayoutService& textLayout, double nowMillis, const core::FrameFinalizer& finalizeContent) const {
        (void)tickDiagnostics(diagnostics, runtime, nowMillis);
        const auto finalize = [&](const canton::core::NativeScene& scene, canton::core::PublishedFrame& frame) {
            if (session.loaded() && !diagnostics.hasError() && !scene.contains(root)) {
                throw std::runtime_error("Canton layout tree is empty after loading UI package.");
            }
            (void)diagnostics.prepareFrame(diagnosticsBounds, detailedErrorScreen, badgeModel, textLayout);
            replacePreparedOps(frame.content.diagnosticsErrorDrawOps, diagnostics.errorOpsSnapshot());
            replacePreparedOps(frame.content.diagnosticsBadgeDrawOps, diagnostics.badgeOpsSnapshot());
            frame.content.diagnosticsFpsDrawOps = diagnostics.fpsOpsSnapshot();
            replacePreparedOps(frame.content.diagnosticsToastDrawOps, diagnostics.toastOpsSnapshot());
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
            (void)diagnostics.prepareFrame(diagnosticsBounds, detailedErrorScreen, badgeModel, textLayout);
            (void)runtime.publishRetained([&](const auto&, auto& retained) {
                retained.content.errorFrame = frame.error;
                replacePreparedOps(retained.content.diagnosticsErrorDrawOps, diagnostics.errorOpsSnapshot());
                replacePreparedOps(retained.content.diagnosticsBadgeDrawOps, diagnostics.badgeOpsSnapshot());
                retained.content.diagnosticsFpsDrawOps = diagnostics.fpsOpsSnapshot();
                replacePreparedOps(retained.content.diagnosticsToastDrawOps, diagnostics.toastOpsSnapshot());
            });
        }
        return runtime.publishedFrame().revision != revision;
    }

    bool FramePumpDriver::tickDiagnostics(DiagnosticsState& diagnostics, CantonRuntime& runtime, double nowMillis) {
        auto changed = false;
        for (auto toast : runtime.takeDiagnosticToasts()) {
            changed = DiagnosticsToast::show(diagnostics, toast.level, toast.tag, std::move(toast.title), std::move(toast.content), toast.coalesce) || changed;
        }
        for (const auto& action : runtime.takeDiagnosticActions()) {
            switch (action.kind) {
                case canton::quickjs::QuickJsDiagnosticActionKind::RequestReload: {
                    DiagnosticsToast::i(diagnostics, TAG, "脚本请求重新加载", action.path);
                    runtime.requestReload();
                    break;
                }
                case canton::quickjs::QuickJsDiagnosticActionKind::TriggerFakeError: {
                    DiagnosticsToast::e(diagnostics, TAG, action.message.empty() ? std::string("脚本请求诊断错误") : action.message, "脚本诊断已触发错误屏路径");
                    break;
                }
                case canton::quickjs::QuickJsDiagnosticActionKind::SetToastsEnabled: {
                    diagnostics.setToastsEnabled(action.enabled);
                    core::Log::i(TAG, "诊断提示显示状态已更新", action.enabled ? "已启用" : "已关闭");
                    break;
                }
            }
        }
        changed = diagnostics.tick(nowMillis) || changed;
        if (!changed) return false;
        runtime.enqueueIntent(canton::core::InputIntent::diagnostics());
        return true;
    }

}

