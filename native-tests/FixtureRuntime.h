#pragma once

#include "TextFixtures.h"

#include <core/SceneFramePipeline.h>
#include <d3d11/DWriteText.h>
#include <quickjs/QuickJsScriptHost.h>
#include <runtime/CantonRuntime.h>
#include <runtime/PainterResources.h>
#include <runtime/UiPackage.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace test_support {
    // 构建好的 .cta 夹具（Dist 目录包）经 QuickJS 进 CantonRuntime；时间由用例推进，分步操作经夹具里输入框的提交槽发指令
    class FixtureRuntime {
       public:
        FixtureRuntime(const std::filesystem::path& directory, canton::core::Constraints constraints) : text_(measurer_), runtime_(canton::core::SceneFramePipeline{canton::core::LayoutEngine{text_}}), constraints_(constraints) {
            auto script = std::make_unique<canton::quickjs::QuickJsScriptHost>();
            host_ = script.get();
            const auto package = canton::runtime::mountUiPackage(directory);
            script->setPainterLoader(canton::runtime::packagePainterLoader(package));
            script->setAssetLoader(canton::runtime::packageAssetLoader(package));
            const auto loaded = script->executePackage(package);
            if (!loaded.ok) throw std::runtime_error("夹具加载失败：" + loaded.error);
            auto initial = script->takePendingTransaction();
            runtime_.setScriptHost(std::move(script));
            if (initial) runtime_.enqueue(std::move(*initial));
        }

        void tick() {
            const auto result = runtime_.pumpFrame(1, constraints_, time_ += 16);
            if (!result.ok) throw std::runtime_error(step + "：" + result.error);
        }

        void settle(int limit = 32) {
            for (int count = 0; count < limit && runtime_.hasPendingFrameWork(); ++count) tick();
            if (runtime_.hasPendingFrameWork()) throw std::runtime_error(step + "：帧需求没有稳定");
        }

        void command(const std::string& value) {
            step = value;
            runtime_.enqueueStringEvent(submit(), value);
            settle();
        }

        [[nodiscard]] canton::core::EventSlotId submit() const {
            for (const auto id : runtime_.scene().tree().nodeIds()) {
                const auto slot = event(runtime_.scene().node(id), canton::core::EventSlotKind::InputSubmit);
                if (slot.valid()) return slot;
            }
            throw std::runtime_error("夹具里找不到接收指令的输入框");
        }

        [[nodiscard]] canton::core::NodeId findText(const std::string& value) const {
            for (const auto id : runtime_.scene().tree().nodeIds())
                if (textOf(runtime_.scene().node(id)) == value) return id;
            return 0;
        }

        void setConstraints(canton::core::Constraints constraints) noexcept {
            constraints_ = constraints;
        }

        [[nodiscard]] double now() const noexcept {
            return time_;
        }

        double advance(double millis) noexcept {
            return time_ += millis;
        }

        [[nodiscard]] canton::runtime::CantonRuntime& runtime() noexcept {
            return runtime_;
        }

        [[nodiscard]] const canton::core::NativeScene& scene() const noexcept {
            return runtime_.scene();
        }

        [[nodiscard]] canton::quickjs::QuickJsScriptHost& host() const noexcept {
            return *host_;
        }

        [[nodiscard]] const canton::core::TextLayoutService& text() const noexcept {
            return text_;
        }

        std::string step = "首帧";

       private:
        canton::d3d11::DWriteTextMeasurer measurer_;
        canton::core::TextLayoutService text_;
        canton::runtime::CantonRuntime runtime_;
        canton::quickjs::QuickJsScriptHost* host_ = nullptr;
        canton::core::Constraints constraints_;
        double time_ = 0;
    };
}
