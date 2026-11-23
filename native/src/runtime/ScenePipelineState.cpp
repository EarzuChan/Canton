#include <runtime/ScenePipelineState.h>

#include <algorithm>
#include <utility>

namespace canton::runtime {
    ScenePipelineState::ScenePipelineState(canton::core::SceneFramePipeline pipeline) : pipeline_(std::move(pipeline)) {}

    void ScenePipelineState::reset() {
        discard();
        scene_.reset();
        frameTimeMillis_ = 0;
        pendingIntents_.clear();
        pendingTransactions_.clear();
        publishedFrame_ = {};
    }

    void ScenePipelineState::enqueue(canton::core::MutationTransaction transaction) {
        enqueueIntent(canton::core::InputIntent::jsCommit(std::move(transaction)));
    }

    void ScenePipelineState::enqueueIntent(canton::core::InputIntent intent) {
        pendingIntents_.push(std::move(intent));
    }

    bool ScenePipelineState::hasPendingIntents() const noexcept {
        return pendingIntents_.size() > 0;
    }

    bool ScenePipelineState::hasPendingTransactions() const noexcept {
        return pendingTransactions_.hasPending();
    }

    void ScenePipelineState::clearPendingTransactions() noexcept {
        pendingTransactions_.clear();
    }

    canton::core::SceneFramePipelineResult ScenePipelineState::run(canton::core::NodeId root, canton::core::Constraints constraints, bool framePipelineRequested, const canton::core::FrameFinalizer& finalize, const canton::core::FramePreparation& preparation) {
        auto result = prepare(root, constraints, framePipelineRequested, finalize, preparation);
        commit();
        return result;
    }

    canton::core::SceneFramePipelineResult ScenePipelineState::prepare(canton::core::NodeId root, canton::core::Constraints constraints, bool framePipelineRequested, const canton::core::FrameFinalizer& finalize, const canton::core::FramePreparation& preparation) {
        for (auto& intent : pendingIntents_.take()) {
            if (intent.transaction && (!intent.transaction->rearrange || !intent.transaction->rearrange->cancelled)) pendingTransactions_.push(std::move(*intent.transaction));
            auto& tree = scene_.tree();
            if (intent.kind == canton::core::InputIntentKind::DiagnosticsEvent)
                tree.invalidateScene(canton::core::DirtyFlag::Diagnostics);
            else if (intent.kind == canton::core::InputIntentKind::Resize)
                tree.invalidateViewport();
            else if (intent.kind == canton::core::InputIntentKind::ResourceReady)
                tree.invalidateScene(canton::core::DirtyFlag::Resource);
            else if (intent.kind == canton::core::InputIntentKind::ResourceFailed) {
                tree.invalidateScene(canton::core::DirtyFlag::Resource);
                tree.invalidateScene(canton::core::DirtyFlag::Diagnostics);
            }
            if (intent.needsFullFallback) tree.requestFullFallback();
        }

        const auto hasPending = pendingTransactions_.hasPending();
        if (!hasPending && !framePipelineRequested && scene_.invalidationSnapshot().empty()) return {};

        auto transaction = hasPending ? pendingTransactions_.take() : std::optional<canton::core::MutationTransaction>{};
        if (transaction && transaction->rearrange && transaction->rearrange->cancelled) transaction.reset();
        return pipeline_.prepare(scene_, root, constraints, transaction ? &*transaction : nullptr, framePipelineRequested, publishedFrame_, finalize, frameTimeMillis_, preparation);
    }

}
