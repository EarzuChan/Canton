#include <runtime/RuntimeSessionState.h>


#include <core/InputIntent.h>
#include <runtime/CantonRuntime.h>
#include <runtime/DiagnosticsState.h>
#include <runtime/ErrorScreenModel.h>

#include <string>
#include <utility>

namespace canton::runtime {
    namespace {
        [[nodiscard]] ErrorSource frameErrorSource(RuntimeFrameErrorPhase phase) noexcept {
            return phase == RuntimeFrameErrorPhase::Pipeline ? ErrorSource::NativeTransaction : ErrorSource::ScriptRuntime;
        }

        [[nodiscard]] const char* frameErrorTitle(RuntimeFrameErrorPhase phase) noexcept {
            switch (phase) {
                case RuntimeFrameErrorPhase::Pipeline:
                    return "帧管线执行失败";

                case RuntimeFrameErrorPhase::Event:
                    return "运行时事件处理失败";

                case RuntimeFrameErrorPhase::Animation:
                    return "动画帧执行失败";

                case RuntimeFrameErrorPhase::None:
                    break;
            }

            return "运行时帧执行失败";
        }

        void emitToast(DiagnosticsState& diagnostics, CantonRuntime& runtime, std::string_view tag, core::LogLevel level, std::string title, std::string message = {}, bool coalesce = true) {
            if (DiagnosticsToast::show(diagnostics, level, tag, std::move(title), std::move(message), coalesce)) runtime.enqueueIntent(canton::core::InputIntent::diagnostics());
        }
    }

    void RuntimeSessionState::reset(CantonRuntime& runtime, DiagnosticsState& diagnostics) {
        loaded_ = false;
        runtime.reset();
        diagnostics.clearError();
    }

    void RuntimeSessionState::resize(int width, int height, CantonRuntime& runtime) noexcept {
        if (width_ == width && height_ == height) return;
        width_ = width;
        height_ = height;
        runtime.enqueueIntent(canton::core::InputIntent::resize());
    }

    canton::core::Constraints RuntimeSessionState::constraints() const noexcept {
        return {0.0f, static_cast<float>(width_), 0.0f, static_cast<float>(height_)};
    }

    void RuntimeSessionState::markLoaded() noexcept {
        loaded_ = true;
    }

    void RuntimeSessionState::markUnloaded() noexcept {
        loaded_ = false;
    }

    void RuntimeSessionState::setLayoutTreeEmptyError(DiagnosticsState& diagnostics, CantonRuntime& runtime) {
        diagnostics.setError(makeErrorScreenModel(ErrorSource::NativeTransaction, "加载 UI 包后 Canton 布局树为空"));
        loaded_ = false;
        emitToast(diagnostics, runtime, TAG, core::LogLevel::Error, "Canton 布局树为空", "加载 UI 包后没有创建任何节点");
    }

    bool RuntimeSessionState::loaded() const noexcept {
        return loaded_;
    }

    bool RuntimeSessionState::interactive(const DiagnosticsState& diagnostics) const noexcept {
        return loaded_ && !diagnostics.hasError();
    }

    bool RuntimeSessionState::applyFrameError(const RuntimeFramePumpResult& frame, DiagnosticsState& diagnostics, CantonRuntime& runtime, const std::filesystem::path& relatedPath) {
        if (frame.ok) {
            return false;
        }

        diagnostics.setError(makeErrorScreenModel(frameErrorSource(frame.errorPhase), frame.error, {}, relatedPath));
        emitToast(diagnostics, runtime, TAG, core::LogLevel::Error, frameErrorTitle(frame.errorPhase), frame.error);
        loaded_ = false;
        return true;
    }
}

