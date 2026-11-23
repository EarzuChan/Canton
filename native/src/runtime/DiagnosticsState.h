#pragma once

#include <core/Paint.h>
#include <core/SceneFramePipeline.h>
#include <runtime/DiagnosticsTypes.h>
#include <runtime/ErrorScreenModel.h>
#include <runtime/LoadPolicy.h>

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace canton::runtime {

    class CantonRuntime;

    // 窗口级开发期诊断插件（HUD：角标/气泡/帧率 + Overlay：帧时间/脏区闪红/布局框框）；
    // Dist 宿主不装配，相关代码与文案不进 Dist 产物
    class DiagnosticsHud {
       public:
        virtual ~DiagnosticsHud() = default;
        virtual bool showToast(core::LogLevel level, std::string_view tag, std::string title, std::string message, bool coalesce) = 0;
        [[nodiscard]] virtual bool tickToasts(double nowMillis) = 0;
        [[nodiscard]] virtual bool tick(CantonRuntime& runtime, double nowMillis) = 0;
        [[nodiscard]] virtual bool hasActiveToasts() const noexcept = 0;
        [[nodiscard]] virtual bool setFpsEnabled(bool enabled) = 0;
        [[nodiscard]] virtual bool fpsEnabled() const noexcept = 0;
        [[nodiscard]] virtual bool recordFrame(bool appChanged, bool ready, double nowMillis) = 0;
        virtual void prepare(core::Rect bounds, bool hasError, PackageSource activeSource, bool liveUnavailable, const core::TextLayoutService& textLayout, core::PublishedFrameContent& content, float dpiScale = 1.0f) = 0;

        virtual void setOverlayFrameTimeEnabled(bool enabled) noexcept = 0;
        [[nodiscard]] virtual bool overlayFrameTimeEnabled() const noexcept = 0;
        virtual void setOverlayDirtyFlashEnabled(bool enabled) noexcept = 0;
        [[nodiscard]] virtual bool overlayDirtyFlashEnabled() const noexcept = 0;
        virtual void setOverlayBoundsEnabled(bool enabled) noexcept = 0;
        [[nodiscard]] virtual bool overlayBoundsEnabled() const noexcept = 0;
        [[nodiscard]] virtual bool overlayNeedsTimeline() const noexcept = 0;
        virtual void setOverlayPointer(std::optional<core::Point> point) noexcept = 0;
        [[nodiscard]] virtual bool updateOverlay(const core::FrameTimeline& timeline, const core::PublishedFrame& frame, const core::LayoutTree& tree, core::NodeId root, const core::TextLayoutService& textLayout, double nowMillis, float dpiScale = 1.0f) = 0;
        [[nodiscard]] virtual std::span<const core::DrawOp> overlayOps() const noexcept = 0;
        [[nodiscard]] virtual bool overlayAnimating() const noexcept = 0;
    };

    class DiagnosticsState final {
       public:
        using NotifySink = std::function<void(core::LogLevel, std::string_view, const std::string&, const std::string&, bool)>;

        void setHud(std::unique_ptr<DiagnosticsHud> hud) noexcept;
        void setNotifySink(NotifySink sink) noexcept;
        [[nodiscard]] DiagnosticsHud* hud() noexcept { return hud_.get(); }
        [[nodiscard]] const DiagnosticsHud* hud() const noexcept { return hud_.get(); }

        bool hasError() const noexcept;
        const ErrorScreenModel* error() const noexcept;
        bool errorRetryAvailable() const noexcept;
        void setError(ErrorScreenModel error);
        void clearError() noexcept;

        // 装了浮层就显示气泡并记日志；没装浮层只写日志
        bool notify(core::LogLevel level, std::string_view tag, std::string title, std::string message = {}, bool coalesce = true);
        [[nodiscard]] bool tickToasts(double nowMillis);
        [[nodiscard]] bool tick(CantonRuntime& runtime, double nowMillis);
        [[nodiscard]] bool hasActiveToasts() const noexcept;
        [[nodiscard]] bool fpsEnabled() const noexcept;
        [[nodiscard]] bool recordWindowFrame(bool appChanged, bool ready, double nowMillis);
        void recordFrame(bool changed, bool ready, double nowMillis, CantonRuntime& runtime);

        void invalidatePreparedFrame() noexcept;
        void prepareFrame(core::Rect bounds, bool detailedErrorScreen, PackageSource activeSource, bool liveUnavailable, const canton::core::TextLayoutService& textLayout, core::PublishedFrameContent& content, float dpiScale = 1.0f, bool primaryErrorWindow = true, const ErrorScreenModel* errorOverride = nullptr);

       private:
        std::unique_ptr<DiagnosticsHud> hud_;
        NotifySink notifySink_;
        std::optional<ErrorScreenModel> error_;
        std::vector<canton::core::DrawOp> preparedErrorOps_;
        std::string preparedSignature_;
    };

}
