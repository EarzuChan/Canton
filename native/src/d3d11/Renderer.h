#pragma once

#include <core/Paint.h>
#include <core/SceneFramePipeline.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace canton::d3d11 {
    enum class AdapterPreference {
        // 硬件优先，不可用时回退 WARP
        Auto,
        // 只用 WARP：各机器结果一致，无头回归用它出基线
        Warp,
        // 只用硬件显卡，没有就报 AdapterUnavailable
        Hardware,
    };

    struct RendererOptions {
        AdapterPreference adapter = AdapterPreference::Auto;
    };

    struct AdapterInfo {
        std::string description;
        bool software = false;
    };

    class AdapterUnavailable : public std::runtime_error {
       public:
        using std::runtime_error::runtime_error;
    };

    // 一帧的输入：core 的发布帧内容，加上不属于发布帧的诊断浮层指令
    struct FrameInput {
        const core::PublishedFrameContent* content = nullptr;
        std::span<const core::DrawOp> diagnosticsOverlay;
        // 0xAARRGGBB，非预乘
        std::uint32_t background = 0xff1f232au;
    };

    struct RenderCounters {
        std::uint64_t frames = 0;
        std::uint64_t drawCalls = 0;
        std::uint64_t instances = 0;
        std::uint64_t glyphsRasterized = 0;
        std::uint64_t pathsRasterized = 0;
        std::uint64_t atlasResets = 0;
        std::uint64_t deviceRebuilds = 0;
    };

    class RenderTarget {
       public:
        virtual ~RenderTarget() = default;
        [[nodiscard]] virtual int width() const noexcept = 0;
        [[nodiscard]] virtual int height() const noexcept = 0;
    };

    // D3D11 2D 渲染器。每帧整幅重绘；设备丢失时重建设备与全部显存资源后重画
    class Renderer {
       public:
        explicit Renderer(RendererOptions options = {});
        ~Renderer();
        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

        [[nodiscard]] AdapterInfo adapter() const;

        // window 是 HWND。呈现走 DXGI Flip Model 与 DirectComposition
        [[nodiscard]] std::unique_ptr<RenderTarget> createWindowTarget(void* window);
        [[nodiscard]] std::unique_ptr<RenderTarget> createOffscreenTarget(int width, int height);
        void resize(RenderTarget& target, int width, int height);

        void render(RenderTarget& target, const FrameInput& frame);
        // 离屏目标的像素，预乘 0xAARRGGBB，行优先
        [[nodiscard]] std::vector<std::uint32_t> readPixels(RenderTarget& target);

        // 丢弃设备与全部显存资源并重建；simulateDeviceLoss 供测试在绘制或呈现阶段注入设备丢失码（HRESULT）
        void recreateDevice();
        void simulateDeviceLoss(long code, int count = 1, bool onPresent = false) noexcept;

        [[nodiscard]] const RenderCounters& counters() const noexcept;

        // 实现细节，定义在渲染库内部
        struct Impl;

       private:
        std::unique_ptr<Impl> impl_;
    };

    // 垂直同步节拍：后台线程等显示器的垂直消隐，每拍调用一次回调。回调在后台线程上执行，由调用方转投到自己的线程
    class VBlankThread {
       public:
        VBlankThread();
        ~VBlankThread();
        VBlankThread(const VBlankThread&) = delete;
        VBlankThread& operator=(const VBlankThread&) = delete;

        void start(std::function<void()> tick);
        void stop() noexcept;

       private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
