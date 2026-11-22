#pragma once

#include "QuickJsEventRegistry.h"
#include "QuickJsModuleLoader.h"
#include "QuickJsPainterResources.h"
#include <quickjs/QuickJsScriptHost.h>
#include <core/Mutation.h>
#include <core/MutationTransaction.h>

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>
#include <string>

extern "C" {
#include <quickjs.h>
}

namespace canton::quickjs {
    struct QuickJsRuntimeContext {
        JSRuntime* runtime = nullptr;
        JSContext* context = nullptr;
        canton::core::NodeId rootNodeId = 0;
        std::function<void()> wakeOwner;
        bool frameRequested = false;
        bool framePrepared = false;
        JSValue prepareFrame = JS_UNDEFINED;
        JSValue completeFrame = JS_UNDEFINED;
        JSValue disposeApp = JS_UNDEFINED;
        JSValue materializeLayout = JS_UNDEFINED;
        bool materializingLayout = false;
        double frameTimeMillis = 0.0;
        static constexpr std::size_t maxJobsPerFrame = 10000;
        std::size_t remainingFrameJobs = maxJobsPerFrame;
        canton::core::MutationTransactionQueue* pendingTransactions = nullptr;
        QuickJsEventRegistry events;

        struct PreparedDrawCallback {
            JSValue callback;
            float width;
            float height;
        };

        std::vector<PreparedDrawCallback> preparedDrawCallbacks;
        std::vector<std::pair<JSValue, JSValue>> unhandledRejections;
        std::unordered_map<std::uint64_t, canton::core::RegisterBinding> bindings;

        struct PublishedModifier {
            canton::core::NodeHandle node;
            canton::core::ModifierHandle handle;
            canton::core::ModifierDescriptor descriptor;
            std::size_t position = 0;
        };

        std::unordered_map<std::uint64_t, PublishedModifier> publishedModifiers;
        // 已接收的类型化输入，提交期间包含候选；不创建 Modifier 实例或预测原生 handle
        std::unordered_map<canton::core::NodeId, std::vector<PublishedModifier>> modifierInputs;
        std::uint64_t rejectedBindingUpdates = 0;
        QuickJsModuleLoader moduleLoader;
        canton::core::PainterLoader painterLoader;
        std::shared_ptr<core::FontRegistry> fonts = std::make_shared<core::FontRegistry>();
        std::shared_ptr<const core::NativePainterRegistry> nativePainters;
        core::AssetLoader assetLoader;

        struct AssetRequest {
            std::future<std::string> bytes;
            JSValue resolve;
            JSValue reject;
            bool text;
        };

        std::vector<AssetRequest> assetRequests;
        bool pumpAssets();
        std::unique_ptr<QuickJsPainterResources> painters;
        std::unordered_map<canton::core::NodeId, canton::core::MeasurePolicy> measurePolicies;
        std::unordered_map<canton::core::NodeId, canton::core::NodeType> nodeTypes;
        std::unordered_map<canton::core::NodeId, std::uint64_t> nodeGenerations;
        std::unordered_map<canton::core::NodeId, std::unordered_map<canton::core::HostInput, canton::core::BindingHandle>> hostBindings;
        std::unordered_map<canton::core::NodeId, canton::core::BindingHandle> modifierBindings;
        std::unordered_map<canton::core::NodeId, std::vector<canton::core::NodeId>> childrenByNode;
        std::unordered_map<canton::core::NodeId, canton::core::NodeId> parentByNode;
        std::vector<QuickJsToastRequest> diagnosticToasts;
        std::vector<QuickJsDiagnosticAction> diagnosticActions;
        std::vector<HotMessage> hotMessages;
        std::vector<canton::core::FocusCommand> focusCommands;
        std::string hotSession;

        struct RearrangeCheckpoint {
            canton::core::NodeId root;
            std::optional<canton::core::MutationTransaction> pending;
            decltype(bindings) savedBindings;
            decltype(publishedModifiers) savedPublishedModifiers;
            decltype(modifierInputs) savedModifierInputs;
            decltype(nodeTypes) savedNodeTypes;
            decltype(measurePolicies) savedMeasurePolicies;
            decltype(nodeGenerations) savedNodeGenerations;
            decltype(hostBindings) savedHostBindings;
            decltype(modifierBindings) savedModifierBindings;
            decltype(childrenByNode) savedChildrenByNode;
            decltype(parentByNode) savedParentByNode;
        };

        std::optional<RearrangeCheckpoint> rearrangeCheckpoint;
        std::shared_ptr<canton::core::RearrangeSubmission> rearrangeSubmission;
        JSValue rearrangeCompletion = JS_UNDEFINED;
        void beginRearrange();
        void abortRearrange();
        void commitRearrange();

        canton::core::MutationTransaction* currentTransaction() noexcept;
        void push(canton::core::TreeMutation mutation);
        canton::core::BindingHandle registerBinding(canton::core::BindingTarget target);
        void updateBinding(canton::core::BindingHandle handle, canton::core::SlotValue value);
        void retireBinding(canton::core::BindingHandle handle);
        std::vector<const canton::core::ModifierDescriptor*> modifierInputDescriptors(canton::core::NodeId id) const;
        void setHostInput(canton::core::NodeId id, canton::core::HostInput input, canton::core::PropValue value);
        void setModifierChain(canton::core::NodeId id, canton::core::ModifierDescriptors value);
        void attachChild(canton::core::NodeId parent, canton::core::NodeId child, std::uint32_t index);
        void detachChild(canton::core::NodeId parent, canton::core::NodeId child);
        void retireSubtree(canton::core::NodeId id);
        void recordToast(QuickJsToastRequest toast);
        void recordDiagnosticAction(QuickJsDiagnosticAction action);
    };
}

