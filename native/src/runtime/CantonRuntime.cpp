#include <runtime/CantonRuntime.h>

#include <runtime/ScriptEventDispatcher.h>

#include <utility>
#include <chrono>
#include <stdexcept>

namespace canton::runtime {
    namespace {
        struct ProcessingGuard {
            explicit ProcessingGuard(bool& value) : value(value) { value = true; }
            ~ProcessingGuard() { value = false; }
            bool& value;
        };
    }

    CantonRuntime::CantonRuntime(canton::core::SceneFramePipeline pipeline) : pipelineState_(std::move(pipeline)) {}

    CantonRuntime::~CantonRuntime() {
        setWorkAvailable({});
        rearrangeHost_.reset();
    }

    void CantonRuntime::OwnerWake::request() {
        std::function<void()> callback;
        {
            std::lock_guard lock(mutex);
            callback = notify;
        }
        if (callback) callback();
    }

    void CantonRuntime::setWorkAvailable(std::function<void()> callback) {
        std::lock_guard lock(wake_->mutex);
        wake_->notify = std::move(callback);
    }

    RuntimeStepResult CantonRuntime::semanticCheckpoint(double nowMillis) {
        if (inFrame_ || inSemantic_) return {false, false, "Owner 任务不能重入"};
        if (semanticError_) return {false, false, *semanticError_};
        const ProcessingGuard guard(inSemantic_);
        auto result = dispatchQueuedEvents(nowMillis);
        if (!result.ok) {
            semanticError_ = result.error;
            return result;
        }
        const auto completed = rearrangeHost_.semanticCheckpoint(nowMillis);
        if (!completed.ok) {
            semanticError_ = completed.error;
            return {true, false, completed.error};
        }
        return result;
    }

    bool CantonRuntime::dispatchInputEventNow(const canton::core::EventSlotId& slot, const canton::core::InputEventPayload& payload, double nowMillis) {
        if (!slot.valid()) return false;
        // 帧内或脚本已出错时不能同步调用，退回排队且视为未消费
        if (inFrame_ || inSemantic_ || semanticError_) {
            enqueueInputEvent(slot, payload);
            return false;
        }
        if (!semanticCheckpoint(nowMillis).ok || !pipelineState_.scene().hasEventSlot(slot)) return false;
        const ProcessingGuard guard(inSemantic_);
        const auto invoked = rearrangeHost_.invokeInput(slot, nowMillis, payload);
        if (!invoked.ok) {
            semanticError_ = invoked.error;
            return false;
        }
        if (invoked.invoked) (void)captureRearrangeTransactions();
        const auto completed = rearrangeHost_.semanticCheckpoint(nowMillis);
        if (!completed.ok) semanticError_ = completed.error;
        wake_->request();
        return invoked.consumed;
    }

    void CantonRuntime::postEvent(QueuedEvent event) {
        if (events_.size() >= 4096) {
            semanticError_ = "Owner 输入邮箱超过 4096 项，已停止接收并要求重新加载";
            wake_->request();
            return;
        }
        event.sequence = nextSequence_++;
        event.ownerGeneration = ownerGeneration_;
        events_.push_back(std::move(event));
        wake_->request();
    }

    bool CantonRuntime::consumeReloadRequest() noexcept {
        return std::exchange(reloadRequested_, false);
    }

    void CantonRuntime::reset() {
        ++ownerGeneration_;
        semanticError_.reset();
        suspended_ = false;
        reloadRequested_ = false;
        rearrangeHost_.reset();
        fonts_.reset();
        fontGeneration_ = 0;
        fontPreloadSince_.reset();
        pipelineState_.reset();
        frame_.reset();
        events_.clear();
    }

    quickjs::CallbackInvokeResult CantonRuntime::applyHotUpdate(const quickjs::LiveModuleSnapshot& snapshot, const quickjs::HotMessage& message) {
        const auto result = rearrangeHost_.applyHotUpdate(snapshot, message);
        wake_->request();
        return result;
    }

    std::vector<quickjs::HotMessage> CantonRuntime::takeHotMessages() {
        return rearrangeHost_.takeHotMessages();
    }

    void CantonRuntime::setScriptHost(std::unique_ptr<canton::quickjs::QuickJsScriptHost> host) noexcept {
        fonts_ = host ? host->fontRegistry() : nullptr;
        fontGeneration_ = fonts_ ? fonts_->generation() : 0;
        fontPreloadSince_.reset();
        rearrangeHost_.setScriptHost(std::move(host));
        const auto weak = std::weak_ptr<OwnerWake>(wake_);
        rearrangeHost_.setOwnerWake([weak] {
            if (const auto state = weak.lock()) {
                state->request();
            }
        });
        wake_->request();
    }

    void CantonRuntime::requestFramePipelineRun() noexcept {
        frame_.requestFramePipelineRun();
    }

    bool CantonRuntime::framePipelineRunRequested() const noexcept {
        return frame_.framePipelineRunRequested();
    }

    void CantonRuntime::enqueue(canton::core::MutationTransaction transaction) {
        enqueueIntent(canton::core::InputIntent::jsCommit(std::move(transaction)));
    }

    void CantonRuntime::enqueueIntent(canton::core::InputIntent intent) {
        pipelineState_.enqueueIntent(std::move(intent));
        requestFramePipelineRun();
    }

    bool CantonRuntime::hasPendingIntents() const noexcept {
        return pipelineState_.hasPendingIntents();
    }

    bool CantonRuntime::hasPendingTransactions() const noexcept {
        return pipelineState_.hasPendingTransactions();
    }

    void CantonRuntime::clearPendingTransactions() noexcept {
        pipelineState_.clearPendingTransactions();
    }

    void CantonRuntime::enqueueEvent(const canton::core::EventSlotId& slot) {
        if (!slot.valid()) return;
        QueuedEvent event;
        event.kind = QueuedEventKind::Invoke;
        event.slot = slot;
        postEvent(std::move(event));
    }

    void CantonRuntime::enqueueScrollSnapshotEvent(const canton::core::EventSlotId& slot, const canton::core::ScrollResult& result) {
        if (!slot.valid()) return;
        QueuedEvent event;
        event.kind = QueuedEventKind::InvokeScrollSnapshot;
        event.slot = slot;
        event.scroll = result;
        postEvent(std::move(event));
    }

    void CantonRuntime::enqueueStringEvent(const canton::core::EventSlotId& slot, std::string value) {
        if (!slot.valid()) return;
        QueuedEvent event;
        event.kind = QueuedEventKind::InvokeString;
        event.slot = slot;
        event.value = std::move(value);
        postEvent(std::move(event));
    }

    void CantonRuntime::enqueueInputEvent(const canton::core::EventSlotId& slot, canton::core::InputEventPayload payload) {
        if (!slot.valid()) return;
        if (const auto* delta = std::get_if<canton::core::DragDeltaPayload>(&payload); delta && !events_.empty() && events_.back().kind == QueuedEventKind::InvokeInput && events_.back().slot == slot && events_.back().ownerGeneration == ownerGeneration_)
            if (auto* pending = std::get_if<canton::core::DragDeltaPayload>(&events_.back().input); pending && pending->modifiers == delta->modifiers) {
                pending->deltaX += delta->deltaX;
                pending->deltaY += delta->deltaY;
                return;
            }
        // 连续指针移动只保留最新位置；按下、抬起、取消与进出不合并
        if (const auto* move = std::get_if<canton::core::PointerInputPayload>(&payload); move && move->type == canton::core::PointerInputType::Move && !events_.empty() && events_.back().kind == QueuedEventKind::InvokeInput && events_.back().slot == slot && events_.back().ownerGeneration == ownerGeneration_)
            if (auto* pending = std::get_if<canton::core::PointerInputPayload>(&events_.back().input); pending && pending->type == canton::core::PointerInputType::Move && pending->modifiers == move->modifiers) {
                *pending = *move;
                return;
            }
        QueuedEvent event;
        event.kind = QueuedEventKind::InvokeInput;
        event.slot = slot;
        event.input = std::move(payload);
        postEvent(std::move(event));
    }

    void CantonRuntime::enqueueFocusEvent(const canton::core::EventSlotId& slot, bool focused) {
        enqueueFocusEvent(slot, focused, focused);
    }

    void CantonRuntime::enqueueFocusEvent(const canton::core::EventSlotId& slot, bool focused, bool hasFocus) {
        if (!slot.valid()) return;
        QueuedEvent event;
        event.kind = QueuedEventKind::InvokeFocus;
        event.slot = slot;
        event.focused = focused;
        event.hasFocus = hasFocus;
        postEvent(std::move(event));
    }

    std::vector<canton::core::FocusCommand> CantonRuntime::takeFocusCommands() {
        return rearrangeHost_.takeFocusCommands();
    }

    bool CantonRuntime::hasPendingEvents() const noexcept {
        return !events_.empty();
    }

    bool CantonRuntime::hasPendingVisualWork() const noexcept {
        return rearrangeHost_.hasPendingVisualWork() || pipelineState_.scene().tree().activeAnimationCount() > 0;
    }

    FrameWorkState CantonRuntime::frameWorkState() const noexcept {
        return {
            hasPendingEvents(),
            hasPendingVisualWork(),
            pipelineState_.hasPendingIntents(),
            pipelineState_.hasPendingTransactions(),
        };
    }

    bool CantonRuntime::hasPendingFrameWork() const noexcept {
        return reloadRequested_ || rearrangeHost_.hasPendingDiagnostics() || (!suspended_ && (frame_.hasPendingFrameWork(frameWorkState()) || (fonts_ && fontGeneration_ != fonts_->generation())));
    }

    bool CantonRuntime::holdsFirstFrameForFonts() {
        if (pipelineState_.publishedFrame().revision != 0 || !fonts_ || fonts_->pendingPreloads() == 0) {
            fontPreloadSince_.reset();
            return false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!fontPreloadSince_) fontPreloadSince_ = now;
        return now - *fontPreloadSince_ < std::chrono::seconds(3);
    }

    bool CantonRuntime::captureRearrangeTransactions() {
        if (!rearrangeHost_.hasScriptHost()) return false;
        auto transaction = rearrangeHost_.takePendingTransaction();
        if (!transaction) return false;
        enqueue(std::move(*transaction));
        return true;
    }

    RuntimeStepResult CantonRuntime::prepareVisualFrame(double nowMillis) {
        if (!rearrangeHost_.hasScriptHost()) return {};
        enqueueIntent(canton::core::InputIntent::animationFrame());
        const auto pumped = rearrangeHost_.prepareVisualFrame(nowMillis);
        if (!pumped.ok) return {true, false, pumped.error};
        return {captureRearrangeTransactions(), true, {}};
    }

    RuntimeStepResult CantonRuntime::dispatchQueuedEvents(double nowMillis) {
        RuntimeStepResult result;

        const auto cutoff = nextSequence_ - 1;
        const auto started = std::chrono::steady_clock::now();
        std::size_t processed = 0;
        while (!events_.empty() && events_.front().sequence <= cutoff) {
            if (processed++ >= 256 || std::chrono::steady_clock::now() - started > std::chrono::milliseconds(8)) {
                wake_->request();
                break;
            }
            auto event = std::move(events_.front());
            events_.pop_front();
            if (event.ownerGeneration != ownerGeneration_ || !pipelineState_.scene().hasEventSlot(event.slot)) continue;

            RearrangeInvokeResult invoked;
            switch (event.kind) {
                case QueuedEventKind::Invoke:
                    invoked = rearrangeHost_.invoke(event.slot, nowMillis);
                    break;
                case QueuedEventKind::InvokeFocus:
                    invoked = rearrangeHost_.invokeFocus(event.slot, nowMillis, event.focused, event.hasFocus);
                    break;
                case QueuedEventKind::InvokeString:
                    invoked = rearrangeHost_.invokeString(event.slot, nowMillis, event.value);
                    break;
                case QueuedEventKind::InvokeScrollSnapshot:
                    invoked = rearrangeHost_.invokeScrollSnapshot(event.slot, nowMillis, event.scroll);
                    break;
                case QueuedEventKind::InvokeInput:
                    invoked = rearrangeHost_.invokeInput(event.slot, nowMillis, event.input);
                    break;
            }

            if (!invoked.ok) return {true, false, invoked.error};
            if (invoked.invoked) {
                result.changed = true;
                result.changed = captureRearrangeTransactions() || result.changed;
            }
        }

        return result;
    }

    RuntimePipelineRunResult CantonRuntime::runPipeline(canton::core::NodeId root, canton::core::Constraints constraints, double nowMillis, const canton::core::FrameFinalizer& finalize) {
        (void)captureRearrangeTransactions();
        const auto hasPending = pipelineState_.hasPendingTransactions() || pipelineState_.hasPendingIntents();
        if (!hasPending && !frame_.framePipelineRunRequested()) return {};

        const canton::core::FramePreparation preparation{
            [&](canton::core::NodeId id, const std::vector<int>& indices) {
                const auto materialized = rearrangeHost_.materializeLayout(id, indices);
                if (!materialized.ok) throw std::runtime_error(materialized.error);
                auto transaction = rearrangeHost_.takePendingTransaction();
                if (!transaction) throw std::runtime_error("Layout 材料化未提交原生候选");
                return std::move(*transaction);
            },
            [&](canton::core::NativeScene& candidate) {
                const auto prepared = rearrangeHost_.prepareDrawModifiers(candidate);
                if (!prepared.ok) throw std::runtime_error(prepared.error);
            },
            interactionPreparation_,
        };
        auto result = pipelineState_.run(root, constraints, frame_.framePipelineRunRequested(), finalize, preparation);
        frame_.clearFramePipelineRunRequest();
        if (result.error) {
            const auto completed = rearrangeHost_.completeRearrange(result.rearrange, *result.error);
            (void)rearrangeHost_.completeVisualFrame(false);
            (void)captureRearrangeTransactions();
            return {true, completed.ok ? *result.error : *result.error + "\n撤销回调失败：" + completed.error};
        }

        rearrangeHost_.publishScene(pipelineState_.scene());
        // 布局反馈属于本次成功发布，必须在后续输入及挂载回调前交付
        // 反馈引出的视觉提交留在队列中，不在当前帧重入流水线
        for (const auto& snapshot : result.scrollUpdates) {
            if (!snapshot.eventSlot.valid()) continue;
            const auto feedback = rearrangeHost_.invokeScrollSnapshot(snapshot.eventSlot, nowMillis, snapshot);
            if (!feedback.ok) {
                (void)rearrangeHost_.completeRearrange(result.rearrange, feedback.error);
                (void)rearrangeHost_.completeVisualFrame(false);
                return {true, feedback.error};
            }
        }
        const auto completed = rearrangeHost_.completeRearrange(result.rearrange);
        (void)captureRearrangeTransactions();
        const auto frameCompleted = rearrangeHost_.completeVisualFrame(completed.ok);
        if (!completed.ok) return {true, completed.error};
        if (!frameCompleted.ok) return {true, frameCompleted.error};
        return {true, std::nullopt};
    }

    RuntimeFramePumpResult CantonRuntime::pumpFrame(canton::core::NodeId root, canton::core::Constraints constraints, double nowMillis, const canton::core::FrameFinalizer& finalize) {
        RuntimeFramePumpResult result;
        if (suspended_) return result;
        if (inFrame_ || inSemantic_) return {false, false, false, RuntimeFrameErrorPhase::Event, "Owner 视觉帧不能重入"};
        const auto semantic = semanticCheckpoint(nowMillis);
        if (!semantic.ok) return {true, false, false, RuntimeFrameErrorPhase::Event, semantic.error};
        const ProcessingGuard frameGuard(inFrame_);
        pipelineState_.setFrameTime(nowMillis);
        if (holdsFirstFrameForFonts()) return result;
        // 字体环境代际推进（自带字体到达、族被替换等）后重跑流水线，由它让 Text、Input 与含文字的绘制片段重建
        if (const auto generation = fonts_ ? fonts_->generation() : 0; generation != fontGeneration_) {
            fontGeneration_ = generation;
            requestFramePipelineRun();
        }
        if (pipelineState_.scene().tree().activeAnimationCount() > 0) requestFramePipelineRun();
        auto plan = frame_.planTick(frameWorkState());

        if (plan.runAnimation || plan.runPipeline && rearrangeHost_.hasScriptHost()) {
            const auto animation = prepareVisualFrame(nowMillis);
            if (!animation.ok) {
                result.changed = true;
                result.ok = false;
                result.errorPhase = RuntimeFrameErrorPhase::Animation;
                result.error = animation.error;
                return result;
            }
            result.changed = result.changed || animation.changed;
            plan = frame_.planTick(frameWorkState());
        }

        if (!plan.runPipeline) {
            const auto completed = rearrangeHost_.completeVisualFrame(true);
            result.ok = completed.ok;
            result.error = completed.error;
            return result;
        }

        const auto pipeline = runPipeline(root, constraints, nowMillis, finalize);
        if (pipeline.error) {
            result.changed = true;
            result.pipelineRan = true;
            result.ok = false;
            result.errorPhase = RuntimeFrameErrorPhase::Pipeline;
            result.error = *pipeline.error;
            return result;
        }

        result.changed = result.changed || pipeline.changed;
        result.pipelineRan = pipeline.changed;
        return result;
    }

    std::vector<canton::quickjs::QuickJsToastRequest> CantonRuntime::takeDiagnosticToasts() {
        return rearrangeHost_.takeDiagnosticToasts();
    }

    std::vector<canton::quickjs::QuickJsDiagnosticAction> CantonRuntime::takeDiagnosticActions() {
        return rearrangeHost_.takeDiagnosticActions();
    }

}

