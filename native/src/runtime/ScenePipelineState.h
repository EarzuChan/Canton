#pragma once

#include <core/Geometry.h>
#include <core/InputIntent.h>
#include <core/MutationTransaction.h>
#include <core/NativeScene.h>
#include <core/Paint.h>
#include <core/SceneFramePipeline.h>

#include <optional>
#include <vector>

namespace canton::runtime {
    class ScenePipelineState {
       public:
        explicit ScenePipelineState(canton::core::SceneFramePipeline pipeline = canton::core::SceneFramePipeline());

        void reset();
        void enqueue(canton::core::MutationTransaction transaction);
        void enqueueIntent(canton::core::InputIntent intent);
        [[nodiscard]] bool hasPendingIntents() const noexcept;
        [[nodiscard]] bool hasPendingTransactions() const noexcept;
        void clearPendingTransactions() noexcept;

        [[nodiscard]] canton::core::SceneFramePipelineResult run(canton::core::NodeId root, canton::core::Constraints constraints, bool framePipelineRequested, const canton::core::FrameFinalizer& finalize = {}, const canton::core::FramePreparation& preparation = {});

        void setFrameTime(double timeMillis) noexcept {
            frameTimeMillis_ = timeMillis;
        }

        [[nodiscard]] canton::core::NativeScene& scene() noexcept {
            return scene_;
        }

        [[nodiscard]] const canton::core::NativeScene& scene() const noexcept {
            return scene_;
        }

        [[nodiscard]] const canton::core::PublishedFrame& publishedFrame() const noexcept {
            return publishedFrame_;
        }

        const canton::core::FrameExecutionCounters& counters() const noexcept {
            return pipeline_.counters();
        }

        [[nodiscard]] const canton::core::FrameTimeline& timeline() const noexcept {
            return pipeline_.timeline();
        }

        [[nodiscard]] canton::core::FrameTimeline& timeline() noexcept {
            return pipeline_.timeline();
        }

        bool publishRetained(const canton::core::FrameFinalizer& finalize) {
            return pipeline_.publishRetained(scene_, publishedFrame_, finalize);
        }

       private:
        double frameTimeMillis_ = 0;
        canton::core::NativeScene scene_;
        canton::core::SceneFramePipeline pipeline_;
        canton::core::InputIntentQueue pendingIntents_;
        canton::core::MutationTransactionQueue pendingTransactions_;
        canton::core::PublishedFrame publishedFrame_;
    };
}
