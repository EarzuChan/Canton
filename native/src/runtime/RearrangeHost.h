#pragma once

#include <core/EventSlot.h>
#include <core/NativeScene.h>
#include <core/MutationTransaction.h>
#include <core/LayoutNode.h>
#include <core/Scroll.h>
#include <runtime/ScriptEventDispatcher.h>

#include <quickjs/QuickJsScriptHost.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace canton::runtime {

    struct RearrangeInvokeResult {
        bool invoked = false;
        bool ok = true;
        std::string error;
        bool consumed = false;
    };

    class RearrangeHost final {
       public:
        void reset() noexcept;

        void setScriptHost(std::unique_ptr<canton::quickjs::QuickJsScriptHost> host) noexcept;
        [[nodiscard]] canton::quickjs::QuickJsScriptHost* scriptHost() noexcept {
            return scriptHost_.get();
        }

        [[nodiscard]] bool hasScriptHost() const noexcept;
        [[nodiscard]] bool hasPendingVisualWork() const noexcept;
        [[nodiscard]] bool hasPendingDiagnostics() const noexcept;
        [[nodiscard]] std::optional<canton::core::MutationTransaction> takePendingTransaction() noexcept;
        [[nodiscard]] std::optional<canton::core::MutationTransaction> takeWindowTransaction(canton::core::WindowId windowId) noexcept;
        [[nodiscard]] std::vector<canton::quickjs::WindowMutation> takeWindowMutations();
        [[nodiscard]] std::shared_ptr<canton::core::RearrangeSubmission> pendingRearrangeSubmission() const noexcept;

        void setOwnerWake(std::function<void()> wake);
        RearrangeInvokeResult semanticCheckpoint(double nowMillis);
        [[nodiscard]] RearrangeInvokeResult prepareVisualFrame(double nowMillis);
        RearrangeInvokeResult completeVisualFrame(bool success);
        RearrangeInvokeResult prepareDrawModifiers(canton::core::NativeScene& candidate);
        RearrangeInvokeResult materializeLayout(canton::core::NodeId id, const std::vector<int>& indices);
        std::vector<canton::core::FocusCommand> takeFocusCommands();
        RearrangeInvokeResult invokeFocus(const canton::core::EventSlotId& slot, double nowMillis, bool focused, bool hasFocus);
        [[nodiscard]] RearrangeInvokeResult invoke(const canton::core::EventSlotId& slot, double nowMillis);
        [[nodiscard]] RearrangeInvokeResult invokeString(const canton::core::EventSlotId& slot, double nowMillis, const std::string& value);

        [[nodiscard]] RearrangeInvokeResult invokeScrollSnapshot(const canton::core::EventSlotId& slot, double nowMillis, const canton::core::ScrollResult& result);
        [[nodiscard]] RearrangeInvokeResult invokeInput(const canton::core::EventSlotId& slot, double nowMillis, const canton::core::InputEventPayload& payload);

        void publishScene(const canton::core::NativeScene& scene);
        void publishWindowScene(canton::core::WindowId windowId, const canton::core::NativeScene& scene);
        RearrangeInvokeResult completeRearrange(const std::shared_ptr<canton::core::RearrangeSubmission>& submission, const std::string& error = {});
        [[nodiscard]] std::vector<canton::quickjs::QuickJsToastRequest> takeDiagnosticToasts();
        [[nodiscard]] std::vector<canton::quickjs::QuickJsDiagnosticAction> takeDiagnosticActions();

       private:
        ScriptEventDispatcher eventDispatcher_;
        std::unique_ptr<canton::quickjs::QuickJsScriptHost> scriptHost_;
    };

}
