#pragma once

#include <core/TextLayoutService.h>
#include <runtime/CantonRuntime.h>
#include <runtime/DiagnosticsState.h>
#include <runtime/FramePumpDriver.h>
#include <runtime/PackageRuntimeSource.h>
#include <runtime/RuntimePackageBinder.h>
#include <runtime/RuntimeSessionState.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace canton::runtime {
    struct SceneHostOptions {
        std::unique_ptr<DiagnosticsHud> hud;
        // 错误屏是否带详细信息
        bool detailedErrorScreen = true;
        // 诊断层文字用的字体族；空表示平台默认字体
        std::vector<std::string> diagnosticsFontFamily;
        // 不给 Live 会话就只有 dist 一种策略可用
        PackageRuntimeSource::LiveSessionFactory live;
        RuntimePackageLoader::DistResources distResources;
        AppResolver::MountPackage mountPackage;
        std::shared_ptr<const core::NativePainterRegistry> painters;
        std::shared_ptr<const core::ShaderCompiler> shaderCompiler;
    };

    // 一个视口的全部编排：脚本运行时、帧泵、诊断与应用来源。不含窗口和渲染，宿主与无头回归共用。
    // 所有方法都在所有者线程上调用
    class SceneHost final {
       public:
        static constexpr core::NodeId rootNodeId = 1;

        // 测量器与字体注册表须与本对象同寿命或更长
        SceneHost(const core::TextMeasurer& measurer, std::shared_ptr<core::FontRegistry> fonts, SceneHostOptions options);
        ~SceneHost();
        SceneHost(const SceneHost&) = delete;
        SceneHost& operator=(const SceneHost&) = delete;

        // 后台线程（资源、Live）有新工作时调用；回调须只做转投
        void setWorkAvailable(std::function<void()> callback);
        void configure(AppSourceConfig config);

        void requestReload() noexcept {
            reloadRequested_ = true;
        }

        void resize(int width, int height);

        // 推进一帧；返回发布帧是否变了
        bool pumpFrame(double nowMillis);
        // 还有没有要在下一拍做的事
        [[nodiscard]] bool wantsFrame() const;
        // 应用已加载、没有错误屏，且至少发布过一帧
        [[nodiscard]] bool ready() const;

        void setFpsBadgeEnabled(bool enabled);

        [[nodiscard]] const core::PublishedFrame& publishedFrame() const noexcept {
            return runtime_.publishedFrame();
        }

        [[nodiscard]] CantonRuntime& runtime() noexcept {
            return runtime_;
        }

        [[nodiscard]] DiagnosticsState& diagnostics() noexcept {
            return diagnostics_;
        }

        [[nodiscard]] const PackageRuntimeSource& source() const noexcept {
            return source_;
        }

        [[nodiscard]] const core::TextLayoutService& textLayout() const noexcept {
            return textLayout_;
        }

       private:
        void apply(PackageLoadOutcome outcome);

        std::vector<std::string> diagnosticsFontFamily_;
        bool detailedErrorScreen_ = true;
        core::TextLayoutService textLayout_;
        CantonRuntime runtime_;
        RuntimeSessionState session_;
        DiagnosticsState diagnostics_;
        PackageRuntimeSource source_;
        RuntimePackageBinder binder_;
        FramePumpDriver framePump_;
        core::Rect bounds_{};
        bool reloadRequested_ = false;
    };
}
