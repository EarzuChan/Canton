#pragma once

#include <core/Paint.h>
#include <core/SceneFramePipeline.h>
#include <runtime/DiagnosticsTypes.h>
#include <runtime/ErrorScreenModel.h>
#include <runtime/LoadPolicy.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace canton::runtime {

    class CantonRuntime;

    // 诊断浮层（角标、气泡、帧率徽标）的可选插件；Dist 宿主不装配，相关代码与文案不进产物
    class DiagnosticsHud {
       public:
        virtual ~DiagnosticsHud() = default;
        virtual bool showToast(core::LogLevel level, std::string_view tag, std::string title, std::string message, bool coalesce) = 0;
        [[nodiscard]] virtual bool tick(CantonRuntime& runtime, double nowMillis) = 0;
        [[nodiscard]] virtual bool hasActiveToasts() const noexcept = 0;
        virtual void setFpsEnabled(bool enabled, CantonRuntime& runtime) = 0;
        [[nodiscard]] virtual bool fpsEnabled() const noexcept = 0;
        virtual void recordFrame(bool changed, bool ready, double nowMillis, CantonRuntime& runtime) = 0;
        virtual void prepare(core::Rect bounds, bool hasError, PackageSource activeSource, bool liveUnavailable, const core::TextLayoutService& textLayout, core::PublishedFrameContent& content) = 0;
    };

    class DiagnosticsState final {
       public:
        void setHud(std::unique_ptr<DiagnosticsHud> hud) noexcept;
        [[nodiscard]] DiagnosticsHud* hud() noexcept { return hud_.get(); }
        [[nodiscard]] const DiagnosticsHud* hud() const noexcept { return hud_.get(); }

        bool hasError() const noexcept;
        const ErrorScreenModel* error() const noexcept;
        bool errorRetryAvailable() const noexcept;
        void setError(ErrorScreenModel error);
        void clearError() noexcept;

        // 装了浮层就显示气泡并记日志；没装浮层只写日志
        bool notify(core::LogLevel level, std::string_view tag, std::string title, std::string message = {}, bool coalesce = true);
        [[nodiscard]] bool tick(CantonRuntime& runtime, double nowMillis);
        [[nodiscard]] bool hasActiveToasts() const noexcept;
        [[nodiscard]] bool fpsEnabled() const noexcept;
        void recordFrame(bool changed, bool ready, double nowMillis, CantonRuntime& runtime);

        void invalidatePreparedFrame() noexcept;
        void prepareFrame(core::Rect bounds, bool detailedErrorScreen, PackageSource activeSource, bool liveUnavailable, const canton::core::TextLayoutService& textLayout, core::PublishedFrameContent& content);

       private:
        std::unique_ptr<DiagnosticsHud> hud_;
        std::optional<ErrorScreenModel> error_;
        std::vector<canton::core::DrawOp> preparedErrorOps_;
        std::string preparedSignature_;
    };

}
