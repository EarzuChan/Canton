#pragma once

#include <core/EventSlot.h>
#include <core/Font.h>
#include <core/InputEvent.h>
#include <core/Geometry.h>
#include <core/InputIntent.h>
#include <core/MutationTransaction.h>
#include <core/NativeScene.h>
#include <core/LayoutNode.h>
#include <core/Paint.h>
#include <core/SceneFramePipeline.h>
#include <core/Scroll.h>
#include <runtime/RearrangeHost.h>
#include <runtime/FramePlanner.h>
#include <runtime/ScenePipelineState.h>

#include <quickjs/QuickJsScriptHost.h>

#include <functional>
#include <chrono>
#include <mutex>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace canton::runtime {

    struct RuntimeStepResult {
        bool changed = false;
        bool ok = true;
        std::string error;
    };

    struct RuntimePipelineRunResult {
        bool changed = false;
        std::optional<std::string> error;
    };

    enum class RuntimeFrameErrorPhase {
        None,
        Event,
        Animation,
        Pipeline,
    };

    struct RuntimeFramePumpResult {
        bool changed = false;
        bool pipelineRan = false;
        bool ok = true;
        RuntimeFrameErrorPhase errorPhase = RuntimeFrameErrorPhase::None;
        std::string error;
    };

    class CantonRuntime final {
       public:
        explicit CantonRuntime(canton::core::SceneFramePipeline pipeline = canton::core::SceneFramePipeline());

        ~CantonRuntime();
        void reset();
        RuntimeStepResult semanticCheckpoint(double nowMillis);

        // 调度器可从资源线程收到唤醒。回调须只投递 Owner 任务且不能抛出异常，由 Owner 调用 semanticCheckpoint。
        void setWorkAvailable(std::function<void()> callback);

        void requestReload() noexcept {
            reloadRequested_ = true;
        }

        [[nodiscard]] bool consumeReloadRequest() noexcept;

        void suspend() noexcept {
            suspended_ = true;
        }

        void setScriptHost(std::unique_ptr<canton::quickjs::QuickJsScriptHost> host) noexcept;
        [[nodiscard]] canton::quickjs::QuickJsScriptHost* scriptHost() noexcept {
            return rearrangeHost_.scriptHost();
        }
        void notifyWorkAvailable();

        void setInteractionPreparation(std::function<void(canton::core::LayoutTree&)> prepare) {
            interactionPreparation_ = std::move(prepare);
        }

        void setEventSlotValidator(std::function<bool(const canton::core::EventSlotId&)> validator) {
            eventSlotValidator_ = std::move(validator);
        }

        // 多窗口宿主由 AppSession 自行提取窗口变更与按窗事务，禁止单视口管道在事件派发时吞取
        void setExternalSurfaceHost(bool external) noexcept {
            externalSurfaceHost_ = external;
        }

        [[nodiscard]] bool checkFontPreloadHold() {
            return holdsFirstFrameForFonts();
        }

        void requestFramePipelineRun() noexcept;
        [[nodiscard]] bool framePipelineRunRequested() const noexcept;

        void enqueue(canton::core::MutationTransaction transaction);
        void enqueueIntent(canton::core::InputIntent intent);
        [[nodiscard]] bool hasPendingIntents() const noexcept;
        [[nodiscard]] bool hasPendingTransactions() const noexcept;
        void clearPendingTransactions() noexcept;

        void enqueueEvent(const canton::core::EventSlotId& slot);
        void enqueueScrollSnapshotEvent(const canton::core::EventSlotId& slot, const canton::core::ScrollResult& result);
        void enqueueFocusEvent(const canton::core::EventSlotId& slot, bool focused);
        void enqueueFocusEvent(const canton::core::EventSlotId& slot, bool focused, bool hasFocus);
        std::vector<canton::core::FocusCommand> takeFocusCommands();
        void enqueueStringEvent(const canton::core::EventSlotId& slot, std::string value);
        void enqueueInputEvent(const canton::core::EventSlotId& slot, canton::core::InputEventPayload payload);
        // 先清空排队事件保持顺序，再同步调用并返回回调是否消费
        [[nodiscard]] bool dispatchInputEventNow(const canton::core::EventSlotId& slot, const canton::core::InputEventPayload& payload, double nowMillis);

        [[nodiscard]] bool hasPendingEvents() const noexcept;

        [[nodiscard]] bool hasPendingVisualWork() const noexcept;
        // 还没发布过帧，且有声明预加载的字体没到齐
        [[nodiscard]] bool awaitingFontPreload() const noexcept;
        [[nodiscard]] bool hasPendingFrameWork() const noexcept;

        [[nodiscard]] RuntimeFramePumpResult pumpFrame(canton::core::NodeId root, canton::core::Constraints constraints, double nowMillis, const canton::core::FrameFinalizer& finalize = {});
        [[nodiscard]] canton::core::NodeId effectiveRootNodeId(canton::core::NodeId fallback = 1) const noexcept {
            return boundWindowId_ ? boundRootNodeId_ : fallback;
        }
        [[nodiscard]] std::optional<canton::core::WindowId> boundWindowId() const noexcept {
            return boundWindowId_;
        }

        [[nodiscard]] canton::core::NativeScene& scene() noexcept {
            return pipelineState_.scene();
        }

        [[nodiscard]] const canton::core::NativeScene& scene() const noexcept {
            return pipelineState_.scene();
        }

        [[nodiscard]] const canton::core::PublishedFrame& publishedFrame() const noexcept {
            return pipelineState_.publishedFrame();
        }

        const canton::core::FrameExecutionCounters& frameCounters() const noexcept {
            return pipelineState_.counters();
        }

        [[nodiscard]] const canton::core::FrameTimeline& frameTimeline() const noexcept {
            return pipelineState_.timeline();
        }

        [[nodiscard]] canton::core::FrameTimeline& frameTimeline() noexcept {
            return pipelineState_.timeline();
        }

        bool publishRetained(const canton::core::FrameFinalizer& finalize) {
            return pipelineState_.publishRetained(finalize);
        }

        [[nodiscard]] std::vector<canton::quickjs::QuickJsToastRequest> takeDiagnosticToasts();
        [[nodiscard]] std::vector<canton::quickjs::QuickJsDiagnosticAction> takeDiagnosticActions();

       private:
        enum class QueuedEventKind {
            Invoke,
            InvokeString,
            InvokeFocus,
            InvokeScrollSnapshot,
            InvokeInput,
        };

        struct QueuedEvent {
            std::uint64_t sequence = 0;
            std::uint64_t ownerGeneration = 0;
            QueuedEventKind kind = QueuedEventKind::Invoke;
            canton::core::EventSlotId slot;
            std::string value;
            bool focused = false;
            bool hasFocus = false;
            canton::core::ScrollResult scroll;
            canton::core::InputEventPayload input;
        };

        [[nodiscard]] FrameWorkState frameWorkState() const noexcept;
        [[nodiscard]] bool captureRearrangeTransactions();
        [[nodiscard]] RuntimeStepResult dispatchQueuedEvents(double nowMillis);
        [[nodiscard]] RuntimeStepResult prepareVisualFrame(double nowMillis);
        [[nodiscard]] RuntimePipelineRunResult runPipeline(canton::core::NodeId root, canton::core::Constraints constraints, double nowMillis, const canton::core::FrameFinalizer& finalize);

        struct OwnerWake {
            std::mutex mutex;
            std::function<void()> notify;
            void request();
        };

        void postEvent(QueuedEvent event);
        std::shared_ptr<OwnerWake> wake_ = std::make_shared<OwnerWake>();
        std::uint64_t ownerGeneration_ = 1;
        std::uint64_t nextSequence_ = 1;
        // 声明预加载的字体未到齐时，首帧最多等待这段时间
        bool holdsFirstFrameForFonts();
        std::shared_ptr<core::FontRegistry> fonts_;
        std::uint64_t fontGeneration_ = 0;
        std::optional<std::chrono::steady_clock::time_point> fontPreloadSince_;
        bool inFrame_ = false;
        bool inSemantic_ = false;
        std::optional<std::string> semanticError_;
        bool suspended_ = false;
        bool reloadRequested_ = false;
        RearrangeHost rearrangeHost_;
        ScenePipelineState pipelineState_;
        FramePlanner frame_;
        std::deque<QueuedEvent> events_;
        std::function<void(canton::core::LayoutTree&)> interactionPreparation_;
        std::function<bool(const canton::core::EventSlotId&)> eventSlotValidator_;
        bool externalSurfaceHost_ = false;
        std::optional<canton::core::WindowId> boundWindowId_;
        canton::core::NodeId boundRootNodeId_ = 1;
    };

}
