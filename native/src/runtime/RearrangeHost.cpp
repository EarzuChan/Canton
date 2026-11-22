#include <runtime/RearrangeHost.h>

namespace canton::runtime {
    namespace {
        RearrangeInvokeResult fromScriptEventResult(ScriptEventInvokeResult result) {
            return {result.invoked, result.ok, std::move(result.error), result.consumed};
        }
    }

    void RearrangeHost::reset() noexcept {
        scriptHost_.reset();
    }

    void RearrangeHost::setScriptHost(std::unique_ptr<canton::quickjs::QuickJsScriptHost> host) noexcept {
        scriptHost_ = std::move(host);
    }

    bool RearrangeHost::hasScriptHost() const noexcept {
        return static_cast<bool>(scriptHost_);
    }

    bool RearrangeHost::hasPendingDiagnostics() const noexcept {
        return scriptHost_ && scriptHost_->hasPendingDiagnostics();
    }

    bool RearrangeHost::hasPendingVisualWork() const noexcept {
        return scriptHost_ && (scriptHost_->hasPendingVisualWork() || scriptHost_->hasPendingFocusCommands());
    }

    std::optional<canton::core::MutationTransaction> RearrangeHost::takePendingTransaction() noexcept {
        if (!scriptHost_) return std::nullopt;
        return scriptHost_->takePendingTransaction();
    }

    void RearrangeHost::setOwnerWake(std::function<void()> wake) {
        if (scriptHost_) scriptHost_->setOwnerWake(std::move(wake));
    }

    RearrangeInvokeResult RearrangeHost::semanticCheckpoint(double nowMillis) {
        if (scriptHost_) {
            const auto result = scriptHost_->semanticCheckpoint(nowMillis);
            return {true, result.ok, result.error};
        }
        return {false, true, {}};
    }

    RearrangeInvokeResult RearrangeHost::prepareVisualFrame(double nowMillis) {
        if (!scriptHost_) return {};
        const auto pumped = scriptHost_->prepareVisualFrame(nowMillis);
        if (!pumped.ok) return {true, false, pumped.error};
        return {true, true, {}};
    }

    RearrangeInvokeResult RearrangeHost::completeVisualFrame(bool success) {
        if (scriptHost_) {
            const auto result = scriptHost_->completeVisualFrame(success);
            return {true, result.ok, result.error};
        }
        return {false, true, {}};
    }

    RearrangeInvokeResult RearrangeHost::prepareDrawModifiers(canton::core::NativeScene& candidate) {
        if (scriptHost_) {
            const auto result = scriptHost_->prepareDrawModifiers(candidate);
            return {true, result.ok, result.error};
        }
        return {false, true, {}};
    }

    RearrangeInvokeResult RearrangeHost::materializeLayout(canton::core::NodeId id, const std::vector<int>& indices) {
        if (scriptHost_) {
            const auto result = scriptHost_->materializeLayout(id, indices);
            return {true, result.ok, result.error};
        }
        return {false, false, "Layout 缺少脚本材料化驱动"};
    }

    std::vector<canton::core::FocusCommand> RearrangeHost::takeFocusCommands() {
        if (scriptHost_) return scriptHost_->takeFocusCommands();
        return {};
    }

    RearrangeInvokeResult RearrangeHost::invokeFocus(const canton::core::EventSlotId& slot, double nowMillis, bool focused, bool hasFocus) {
        canton::quickjs::CallbackInvokeOptions options;
        options.hasFocusArgument = true;
        options.focused = focused;
        options.hasFocus = hasFocus;
        return fromScriptEventResult(eventDispatcher_.invoke(scriptHost_.get(), slot, nowMillis, options));
    }

    RearrangeInvokeResult RearrangeHost::invoke(const canton::core::EventSlotId& slot, double nowMillis) {
        return fromScriptEventResult(eventDispatcher_.invoke(scriptHost_.get(), slot, nowMillis));
    }

    RearrangeInvokeResult RearrangeHost::invokeString(const canton::core::EventSlotId& slot, double nowMillis, const std::string& value) {
        return fromScriptEventResult(eventDispatcher_.invokeString(scriptHost_.get(), slot, nowMillis, value));
    }

    RearrangeInvokeResult RearrangeHost::invokeScrollSnapshot(const canton::core::EventSlotId& slot, double nowMillis, const canton::core::ScrollResult& result) {
        return fromScriptEventResult(eventDispatcher_.invokeScroll(scriptHost_.get(), slot, nowMillis, result));
    }

    RearrangeInvokeResult RearrangeHost::invokeInput(const canton::core::EventSlotId& slot, double nowMillis, const canton::core::InputEventPayload& payload) {
        return fromScriptEventResult(eventDispatcher_.invokeInput(scriptHost_.get(), slot, nowMillis, payload));
    }

    RearrangeInvokeResult RearrangeHost::completeRearrange(const std::shared_ptr<canton::core::RearrangeSubmission>& submission, const std::string& error) {
        if (scriptHost_) {
            const auto result = scriptHost_->completeRearrange(submission, error);
            return {true, result.ok, result.error};
        }
        return {false, true, {}};
    }

    void RearrangeHost::publishScene(const canton::core::NativeScene& scene) {
        if (scriptHost_) scriptHost_->publishScene(scene);
    }

    std::vector<canton::quickjs::QuickJsToastRequest> RearrangeHost::takeDiagnosticToasts() {
        return scriptHost_ ? scriptHost_->takeDiagnosticToasts() : std::vector<canton::quickjs::QuickJsToastRequest>{};
    }

    std::vector<canton::quickjs::QuickJsDiagnosticAction> RearrangeHost::takeDiagnosticActions() {
        return scriptHost_ ? scriptHost_->takeDiagnosticActions() : std::vector<canton::quickjs::QuickJsDiagnosticAction>{};
    }
}

