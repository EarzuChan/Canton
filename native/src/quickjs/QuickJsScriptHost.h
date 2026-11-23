#pragma once

#include <memory>
#include <optional>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <core/EventSlot.h>
#include <core/InputEvent.h>
#include <core/NativeScene.h>
#include <core/MutationTransaction.h>
#include <core/Scroll.h>
#include <core/Painter.h>
#include <core/UiPackage.h>
#include <core/FocusTypes.h>
#include <core/Font.h>
#include <core/GraphicsEffects.h>
#include <core/ModifierGeometry.h>
#include <core/NativePainter.h>
#include <core/Log.h>
#include <core/WindowTypes.h>
#include "ScriptHost.h"
#include "LiveModule.h"

namespace canton::quickjs {

    struct QuickJsToastRequest {
        core::LogLevel level = core::LogLevel::Info;
        std::string tag;
        std::string title;
        std::string content;
        bool coalesce = true;
    };

    enum class QuickJsDiagnosticActionKind {
        RequestReload,
        TriggerFakeError,
        SetToastsEnabled,
    };

    struct QuickJsDiagnosticAction {
        QuickJsDiagnosticActionKind kind = QuickJsDiagnosticActionKind::RequestReload;
        std::string message;
        std::string path;
        double timestamp = 0.0;
        bool enabled = false;
    };

    enum class WindowMutationKind : std::uint8_t {
        Create,
        Update,
        Destroy,
    };

    struct WindowMutation {
        WindowMutationKind kind = WindowMutationKind::Create;
        core::WindowId windowId = 0;
        core::NodeId rootNodeId = 0;
        core::WindowConfigSpec config;
    };

    struct WindowCommand {
        core::WindowId windowId = 0;
        core::WindowCommandKind kind = core::WindowCommandKind::Activate;
    };

    struct ScopedFocusCommand {
        core::WindowId windowId = 0;
        core::FocusCommand command;
    };

    struct ScriptMemoryStats {
        std::uint64_t allocations = 0;
        std::uint64_t allocatedBytes = 0;
        std::size_t liveBytes = 0;
        std::size_t peakBytes = 0;
        // 实际向系统堆申请的次数（新大块与池外大对象）
        std::uint64_t systemAllocations = 0;
    };

    // 超时是终止整个脚本任务的保护边界，不是帧率目标或可恢复时间片
    struct ScriptExecutionLimits {
        int semanticMillis = 100;
        int visualMillis = 1000;
        int moduleMillis = 1000;
    };

    class QuickJsScriptHost final : public ScriptHost {
       public:
        explicit QuickJsScriptHost(ScriptExecutionLimits limits = {});
        ~QuickJsScriptHost() override;
        void setPainterLoader(canton::core::PainterLoader loader);
        [[nodiscard]] const canton::core::PainterLoader& painterLoader() const noexcept;
        void setAssetLoader(core::AssetLoader loader);
        // 注册表须在加载脚本前设置。同一应用的多个脚本上下文可共享注册表。
        void setFontRegistry(std::shared_ptr<core::FontRegistry> fonts);
        [[nodiscard]] const std::shared_ptr<core::FontRegistry>& fontRegistry() const noexcept;
        void setNativePainterRegistry(std::shared_ptr<const core::NativePainterRegistry> painters);
        void setShaderCompiler(std::shared_ptr<const core::ShaderCompiler> compiler);

        ScriptExecutionResult executeModule(const std::filesystem::path& modulePath, std::string_view source);
        ScriptExecutionResult executePackage(std::shared_ptr<const core::UiPackage> package) override;
        ScriptExecutionResult executeLiveModules(const LiveModuleSnapshot& snapshot);
        [[nodiscard]] bool hasMountedApp() const noexcept;
        CallbackInvokeResult applyHotUpdate(const LiveModuleSnapshot& snapshot, const HotMessage& message);
        std::vector<HotMessage> takeHotMessages();
        std::vector<canton::core::FocusCommand> takeFocusCommands();
        std::vector<ScopedFocusCommand> takeScopedFocusCommands();
        bool hasPendingFocusCommands() const noexcept;

        // App 与多窗口控制通道
        std::optional<core::AppConfigSpec> takeAppConfig();
        bool consumeAppExitRequest() noexcept;
        std::vector<WindowMutation> takeWindowMutations();
        std::vector<WindowCommand> takeWindowCommands();
        [[nodiscard]] bool hasPendingWindowWork() const noexcept;
        [[nodiscard]] std::vector<core::WindowId> pendingTransactionWindows() const;
        [[nodiscard]] bool hasWindowTransaction(core::WindowId windowId) const noexcept;
        std::optional<core::MutationTransaction> takeWindowTransaction(core::WindowId windowId) noexcept;
        void clearWindowTransactions() noexcept;
        [[nodiscard]] std::shared_ptr<core::RearrangeSubmission> pendingRearrangeSubmission() const noexcept;

        CallbackInvokeResult invokeWindowCloseRequest(core::WindowId windowId);
        CallbackInvokeResult invokeWindowGeometryChanged(core::WindowId windowId, const core::WindowGeometryEvent& event);
        CallbackInvokeResult invokeWindowFocusChanged(core::WindowId windowId, bool focused);
        CallbackInvokeResult invokeWindowDpiChanged(core::WindowId windowId, double dpiScale);

        CallbackInvokeResult invokeEventSlot(const canton::core::EventSlotId& slot, const CallbackInvokeOptions& options = {});
        CallbackInvokeResult invokeEventSlot(const canton::core::EventSlotId& slot, const canton::core::ScrollResult& scroll);
        CallbackInvokeResult invokeEventSlot(const canton::core::EventSlotId& slot, const canton::core::InputEventPayload& payload);

        bool hasPendingTransactions() const noexcept {
            return pendingTransactions_.hasPending();
        }

        const std::optional<canton::core::MutationTransaction>& pendingTransactions() const noexcept {
            return pendingTransactions_.pending();
        }

        std::optional<canton::core::MutationTransaction> takePendingTransaction() noexcept;
        void clearPendingTransactions() noexcept;
        void setFrameTimeMillis(double nowMillis) noexcept;
        void setOwnerWake(std::function<void()> wake);
        CallbackInvokeResult semanticCheckpoint(double nowMillis);
        bool hasPendingSemanticWork() const noexcept;
        bool hasPendingVisualWork() const noexcept;
        CallbackInvokeResult prepareVisualFrame(double nowMillis);
        CallbackInvokeResult dispatchPositioned(core::WindowId windowId, const core::NativeScene& candidateScene, const std::vector<core::PositionedUpdate>& updates);
        CallbackInvokeResult prepareDrawModifiers(canton::core::NativeScene& candidateScene);
        CallbackInvokeResult materializeLayout(canton::core::NodeId id, const std::vector<int>& indices);
        CallbackInvokeResult completeVisualFrame(bool success);
        bool hasPendingDiagnostics() const noexcept;
        std::vector<QuickJsToastRequest> takeDiagnosticToasts();
        std::vector<QuickJsDiagnosticAction> takeDiagnosticActions();
        std::size_t eventSlotCount() const noexcept;
        std::size_t bindingCount() const noexcept;
        std::size_t modifierInstanceCount() const noexcept;
        std::uint64_t rejectedBindingUpdates() const noexcept;
        ScriptMemoryStats memoryStats() const noexcept;
        void publishScene(const canton::core::NativeScene& scene);
        void publishWindowScene(core::WindowId windowId, const canton::core::NativeScene& scene);
        void unpublishWindowScene(core::WindowId windowId);
        CallbackInvokeResult completeRearrange(const std::shared_ptr<canton::core::RearrangeSubmission>& submission, const std::string& error = {});

       private:
        ScriptExecutionResult runModuleTask(const char* name, bool resetFirst, const std::function<bool(struct QuickJsRuntimeContext&, std::string&)>& step);
        struct QuickJsRuntimeContext* runtimeContext() noexcept;

        canton::core::MutationTransactionQueue pendingTransactions_;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

}
