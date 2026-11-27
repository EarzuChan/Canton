#include <d3d11/RendererImpl.h>

#include "BlurPs.h"
#include "ColorFilterPs.h"
#include "CompositePs.h"
#include "PostVs.h"
#include "QuadPs.h"
#include "QuadVs.h"
#include "UserVs.h"

#include <algorithm>
#include <cstring>

namespace canton::d3d11 {
    namespace {
        constexpr D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        constexpr UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

        bool softwareAdapter(const DXGI_ADAPTER_DESC1& description) {
            // Microsoft Basic Render Driver：没有显卡驱动时系统给出的软件适配器
            return (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || (description.VendorId == 0x1414 && description.DeviceId == 0x8c);
        }

        HRESULT create(IDXGIAdapter* adapter, D3D_DRIVER_TYPE type, ComPtr<ID3D11Device>& device, ComPtr<ID3D11DeviceContext>& context) {
            return D3D11CreateDevice(adapter, type, nullptr, deviceFlags, &featureLevel, 1, D3D11_SDK_VERSION, device.ReleaseAndGetAddressOf(), nullptr, context.ReleaseAndGetAddressOf());
        }
    }

    Renderer::Impl::Impl(RendererOptions input) : options(input) {
        createDevice();
        createDeviceResources();
    }

    void Renderer::Impl::createDevice() {
        device.Reset();
        context.Reset();
        factory.Reset();
        HRESULT result = E_FAIL;
        if (options.adapter == AdapterPreference::Hardware) {
            // 逐个适配器找第一块真显卡
            ComPtr<IDXGIFactory1> enumerator;
            check(CreateDXGIFactory1(IID_PPV_ARGS(enumerator.GetAddressOf())), "无法创建 DXGI 工厂");
            ComPtr<IDXGIAdapter1> adapter;
            for (UINT index = 0; enumerator->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++index) {
                DXGI_ADAPTER_DESC1 description{};
                if (FAILED(adapter->GetDesc1(&description)) || softwareAdapter(description)) continue;
                if (SUCCEEDED(result = create(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, device, context))) break;
            }
            if (FAILED(result)) throw AdapterUnavailable("这台机器没有可用的硬件显卡（需要 D3D11 Feature Level 11_0）");
        } else {
            if (options.adapter == AdapterPreference::Auto) result = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, device, context);
            if (FAILED(result)) {
                if (options.adapter == AdapterPreference::Auto) core::Log::w(TAG, "硬件显卡不可用，改用 WARP");
                check(create(nullptr, D3D_DRIVER_TYPE_WARP, device, context), "无法创建 D3D11 设备");
            }
        }
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        check(device.As(&dxgiDevice), "无法取得 DXGI 设备");
        check(dxgiDevice->GetAdapter(adapter.GetAddressOf()), "无法取得显示适配器");
        check(adapter->GetParent(IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())), "无法取得 DXGI 工厂");
        adapterInfo = {};
        ComPtr<IDXGIAdapter1> adapter1;
        DXGI_ADAPTER_DESC1 description{};
        if (SUCCEEDED(adapter.As(&adapter1)) && SUCCEEDED(adapter1->GetDesc1(&description))) {
            adapterInfo.description = narrow(description.Description);
            adapterInfo.software = softwareAdapter(description);
        }
        ++deviceGeneration;
    }

    ComPtr<ID3D11ShaderResourceView> Renderer::Impl::createTexture(int width, int height, DXGI_FORMAT format, const void* pixels, UINT pitch, ComPtr<ID3D11Texture2D>* texture) {
        D3D11_TEXTURE2D_DESC description{};
        description.Width = static_cast<UINT>(width);
        description.Height = static_cast<UINT>(height);
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = format;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{pixels, pitch, 0};
        ComPtr<ID3D11Texture2D> created;
        ComPtr<ID3D11ShaderResourceView> view;
        check(device->CreateTexture2D(&description, pixels ? &data : nullptr, created.GetAddressOf()), "无法创建纹理");
        check(device->CreateShaderResourceView(created.Get(), nullptr, view.GetAddressOf()), "无法创建纹理视图");
        if (texture) *texture = created;
        return view;
    }

    void Renderer::Impl::createDeviceResources() {
        check(device->CreateVertexShader(kQuadVs, sizeof(kQuadVs), nullptr, vertexShader.ReleaseAndGetAddressOf()), "无法创建顶点着色器");
        check(device->CreatePixelShader(kQuadPs, sizeof(kQuadPs), nullptr, pixelShader.ReleaseAndGetAddressOf()), "无法创建像素着色器");
        check(device->CreateVertexShader(kUserVs, sizeof(kUserVs), nullptr, userVertexShader.ReleaseAndGetAddressOf()), "无法创建用户着色器顶点管线");
        check(device->CreateVertexShader(kPostVs, sizeof(kPostVs), nullptr, postVertexShader.ReleaseAndGetAddressOf()), "无法创建后处理顶点着色器");
        check(device->CreatePixelShader(kBlurPs, sizeof(kBlurPs), nullptr, blurPixelShader.ReleaseAndGetAddressOf()), "无法创建模糊像素着色器");
        check(device->CreatePixelShader(kColorFilterPs, sizeof(kColorFilterPs), nullptr, colorFilterPixelShader.ReleaseAndGetAddressOf()), "无法创建色彩滤镜像素着色器");
        check(device->CreatePixelShader(kCompositePs, sizeof(kCompositePs), nullptr, compositePixelShader.ReleaseAndGetAddressOf()), "无法创建离屏合成像素着色器");
        std::array<D3D11_INPUT_ELEMENT_DESC, sizeof(QuadInstance) / (sizeof(float) * 4)> elements{};
        for (std::size_t i = 0; i < elements.size(); ++i) elements[i] = {"INSTANCE", static_cast<UINT>(i), DXGI_FORMAT_R32G32B32A32_FLOAT, 0, static_cast<UINT>(i * sizeof(float) * 4), D3D11_INPUT_PER_INSTANCE_DATA, 1};
        check(device->CreateInputLayout(elements.data(), static_cast<UINT>(elements.size()), kQuadVs, sizeof(kQuadVs), inputLayout.ReleaseAndGetAddressOf()), "无法创建输入布局");

        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = static_cast<UINT>(sizeof(QuadInstance) * instanceCapacity);
        buffer.Usage = D3D11_USAGE_DYNAMIC;
        buffer.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        buffer.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        check(device->CreateBuffer(&buffer, nullptr, instanceBuffer.ReleaseAndGetAddressOf()), "无法创建实例缓冲");
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        buffer.ByteWidth = sizeof(FrameConstants);
        check(device->CreateBuffer(&buffer, nullptr, constantBuffer.ReleaseAndGetAddressOf()), "无法创建常量缓冲");
        buffer.ByteWidth = sizeof(UserVsConstants);
        check(device->CreateBuffer(&buffer, nullptr, userVsBuffer.ReleaseAndGetAddressOf()), "无法创建用户顶点常量缓冲");
        buffer.ByteWidth = sizeof(CantonConstants);
        check(device->CreateBuffer(&buffer, nullptr, cantonCbuffer.ReleaseAndGetAddressOf()), "无法创建 Canton 常量缓冲");
        buffer.ByteWidth = sizeof(PostVsConstants);
        check(device->CreateBuffer(&buffer, nullptr, postVsBuffer.ReleaseAndGetAddressOf()), "无法创建后处理顶点常量缓冲");
        buffer.ByteWidth = static_cast<UINT>(std::max(sizeof(BlurParamsConstants), sizeof(CompositeParamsConstants)));
        check(device->CreateBuffer(&buffer, nullptr, postPsBuffer.ReleaseAndGetAddressOf()), "无法创建后处理像素常量缓冲");

        const auto makeBlend = [&](bool enable, D3D11_BLEND src, D3D11_BLEND dst, D3D11_BLEND srcA, D3D11_BLEND dstA, ComPtr<ID3D11BlendState>& out) {
            D3D11_BLEND_DESC desc{};
            desc.RenderTarget[0].BlendEnable = enable ? TRUE : FALSE;
            desc.RenderTarget[0].SrcBlend = src;
            desc.RenderTarget[0].DestBlend = dst;
            desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
            desc.RenderTarget[0].SrcBlendAlpha = srcA;
            desc.RenderTarget[0].DestBlendAlpha = dstA;
            desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
            desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
            check(device->CreateBlendState(&desc, out.ReleaseAndGetAddressOf()), "无法创建混合状态");
        };

        // 前 15 种硬件 BlendMode（预乘 RGBA）
        makeBlend(true, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, D3D11_BLEND_ZERO, hardwareBlends[0]);                             // Clear
        makeBlend(false, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, hardwareBlends[1]);                              // Src
        makeBlend(true, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_ONE, hardwareBlends[2]);                               // Dst
        makeBlend(true, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, hardwareBlends[3]);             // SrcOver
        makeBlend(true, D3D11_BLEND_INV_DEST_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_INV_DEST_ALPHA, D3D11_BLEND_ONE, hardwareBlends[4]);           // DstOver
        makeBlend(true, D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_ZERO, D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_ZERO, hardwareBlends[5]);                 // SrcIn
        makeBlend(true, D3D11_BLEND_ZERO, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_ZERO, D3D11_BLEND_SRC_ALPHA, hardwareBlends[6]);                   // DstIn
        makeBlend(true, D3D11_BLEND_INV_DEST_ALPHA, D3D11_BLEND_ZERO, D3D11_BLEND_INV_DEST_ALPHA, D3D11_BLEND_ZERO, hardwareBlends[7]);         // SrcOut
        makeBlend(true, D3D11_BLEND_ZERO, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ZERO, D3D11_BLEND_INV_SRC_ALPHA, hardwareBlends[8]);           // DstOut
        makeBlend(true, D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, hardwareBlends[9]); // SrcAtop
        makeBlend(true, D3D11_BLEND_INV_DEST_ALPHA, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_DEST_ALPHA, D3D11_BLEND_SRC_ALPHA, hardwareBlends[10]); // DstAtop
        makeBlend(true, D3D11_BLEND_INV_DEST_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_INV_DEST_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, hardwareBlends[11]); // Xor
        makeBlend(true, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, hardwareBlends[12]);                                // Plus
        makeBlend(true, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ZERO, D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_ZERO, hardwareBlends[13]);                // Modulate
        makeBlend(true, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_COLOR, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, hardwareBlends[14]);            // Screen

        blend = hardwareBlends[3];
        replace = hardwareBlends[1];
        // Clear 抗锯齿挖空：源输出 (0,0,0,cov) 时结果 = dst * (1 - cov)
        clearPunchOutBlend = hardwareBlends[8];

        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.ScissorEnable = TRUE;
        raster.DepthClipEnable = TRUE;
        check(device->CreateRasterizerState(&raster, rasterizer.ReleaseAndGetAddressOf()), "无法创建光栅化状态");

        D3D11_SAMPLER_DESC sampling{};
        sampling.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampling.AddressU = sampling.AddressV = sampling.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampling.MaxLOD = D3D11_FLOAT32_MAX;
        check(device->CreateSamplerState(&sampling, sampler.ReleaseAndGetAddressOf()), "无法创建采样器");

        atlasSize = adapterInfo.software ? warpInitialAtlasSize : maxAtlasSize;
        counters.atlasSize = atlasSize;
        atlasView = createTexture(atlasSize, atlasSize, DXGI_FORMAT_R8_UNORM, nullptr, 0, &atlasTexture);
        gradientView = createTexture(gradientWidth, gradientRows, DXGI_FORMAT_R8G8B8A8_UNORM, nullptr, 0, &gradientTexture);
        const std::uint32_t white = 0xffffffffu;
        whiteView = createTexture(1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, &white, 4);
        resetCaches();
    }

    void Renderer::Impl::releaseDeviceResources() {
        if (context) {
            context->ClearState();
            context->Flush();
        }
        resetCaches();
        frameRecordedLayers.clear();
        layerRecordStack.clear();
        activeRecordingLayers.clear();
        masks.clear();
        offscreenPool.clear();
        userShaders.clear();
        for (auto& cb : userCbuffers) cb.Reset();
        userCbufferSizes.fill(0);
        for (auto& smp : samplers) smp.Reset();
        for (auto& bs : hardwareBlends) bs.Reset();
        clearPunchOutBlend.Reset();
        maskWidth = maskHeight = 0;
        vertexShader.Reset();
        pixelShader.Reset();
        userVertexShader.Reset();
        postVertexShader.Reset();
        blurPixelShader.Reset();
        colorFilterPixelShader.Reset();
        compositePixelShader.Reset();
        inputLayout.Reset();
        instanceBuffer.Reset();
        constantBuffer.Reset();
        userVsBuffer.Reset();
        cantonCbuffer.Reset();
        postVsBuffer.Reset();
        postPsBuffer.Reset();
        blend.Reset();
        replace.Reset();
        rasterizer.Reset();
        sampler.Reset();
        atlasTexture.Reset();
        atlasView.Reset();
        gradientTexture.Reset();
        gradientView.Reset();
        whiteView.Reset();
    }

    void Renderer::Impl::rebuildDevice() {
        releaseDeviceResources();
        createDevice();
        createDeviceResources();
        ++counters.deviceRebuilds;
        core::Log::w(TAG, "已重建 D3D11 设备与显存资源", adapterInfo.description);
    }

    bool Renderer::Impl::deviceLost() const {
        return device && FAILED(device->GetDeviceRemovedReason());
    }

    void Renderer::Impl::ensureMasks() {
        if (maskWidth == targetWidth && maskHeight == targetHeight) return;
        masks.clear();
        maskWidth = targetWidth;
        maskHeight = targetHeight;
    }

    void TargetImpl::release() {
        view.Reset();
        texture.Reset();
        staging.Reset();
        visual.Reset();
        compositionTarget.Reset();
        composition.Reset();
        swapChain.Reset();
        bufferWidth = bufferHeight = 0;
    }

    void TargetImpl::ensure() {
        auto& renderer = *owner;
        const auto width = std::max(width_, 1), height = std::max(height_, 1);
        if (generation != renderer.deviceGeneration) {
            release();
            generation = renderer.deviceGeneration;
        }
        if (view && bufferWidth == width && bufferHeight == height) return;
        view.Reset();
        if (window) {
            if (!swapChain) {
                DXGI_SWAP_CHAIN_DESC1 description{};
                description.Width = static_cast<UINT>(width);
                description.Height = static_cast<UINT>(height);
                description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                description.SampleDesc.Count = 1;
                description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                description.BufferCount = 2;
                description.Scaling = DXGI_SCALING_STRETCH;
                description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
                description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
                check(renderer.factory->CreateSwapChainForComposition(renderer.device.Get(), &description, nullptr, swapChain.GetAddressOf()), "无法创建交换链");
                ComPtr<IDXGIDevice> dxgiDevice;
                check(renderer.device.As(&dxgiDevice), "无法取得 DXGI 设备");
                check(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(composition.GetAddressOf())), "无法创建 DirectComposition 设备");
                check(composition->CreateTargetForHwnd(window, TRUE, compositionTarget.GetAddressOf()), "无法绑定 DirectComposition 到窗口");
                check(composition->CreateVisual(visual.GetAddressOf()), "无法创建合成图层");
                check(visual->SetContent(swapChain.Get()), "无法设置合成内容");
                check(compositionTarget->SetRoot(visual.Get()), "无法设置合成根");
                check(composition->Commit(), "无法提交合成");
            } else
                check(swapChain->ResizeBuffers(0, static_cast<UINT>(width), static_cast<UINT>(height), DXGI_FORMAT_UNKNOWN, 0), "无法调整交换链尺寸");
            check(swapChain->GetBuffer(0, IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())), "无法取得后备缓冲");
        } else {
            D3D11_TEXTURE2D_DESC description{};
            description.Width = static_cast<UINT>(width);
            description.Height = static_cast<UINT>(height);
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags = D3D11_BIND_RENDER_TARGET;
            check(renderer.device->CreateTexture2D(&description, nullptr, texture.ReleaseAndGetAddressOf()), "无法创建离屏纹理");
            description.Usage = D3D11_USAGE_STAGING;
            description.BindFlags = 0;
            description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            check(renderer.device->CreateTexture2D(&description, nullptr, staging.ReleaseAndGetAddressOf()), "无法创建回读纹理");
        }
        check(renderer.device->CreateRenderTargetView(texture.Get(), nullptr, view.ReleaseAndGetAddressOf()), "无法创建渲染目标视图");
        // 交换链的后备缓冲只经视图持有，调整尺寸前才能全部放掉
        if (window) texture.Reset();
        bufferWidth = width;
        bufferHeight = height;
    }

    Renderer::Renderer(RendererOptions options) : impl_(std::make_unique<Impl>(options)) {}

    Renderer::~Renderer() = default;

    AdapterInfo Renderer::adapter() const {
        return impl_->adapterInfo;
    }

    std::uint64_t Renderer::deviceGeneration() const noexcept {
        return impl_->deviceGeneration;
    }

    const RenderCounters& Renderer::counters() const noexcept {
        return impl_->counters;
    }

    std::unique_ptr<RenderTarget> Renderer::createWindowTarget(void* window) {
        auto target = std::make_unique<TargetImpl>();
        target->owner = impl_.get();
        target->window = static_cast<HWND>(window);
        RECT client{};
        GetClientRect(target->window, &client);
        target->width_ = client.right - client.left;
        target->height_ = client.bottom - client.top;
        return target;
    }

    std::unique_ptr<RenderTarget> Renderer::createOffscreenTarget(int width, int height) {
        auto target = std::make_unique<TargetImpl>();
        target->owner = impl_.get();
        target->width_ = width;
        target->height_ = height;
        return target;
    }

    void Renderer::resize(RenderTarget& target, int width, int height) {
        auto& output = static_cast<TargetImpl&>(target);
        output.width_ = width;
        output.height_ = height;
    }

    void Renderer::recreateDevice() {
        impl_->rebuildDevice();
    }

    void Renderer::simulateDeviceLoss(long code, int count, bool onPresent) noexcept {
        static_assert(std::is_same_v<HRESULT, long>);
        impl_->injectedLossCode = code;
        impl_->injectedLossRemaining = count;
        impl_->injectedLossOnPresent = onPresent;
    }

    void Renderer::render(RenderTarget& target, const FrameInput& frame) {
        auto& output = static_cast<TargetImpl&>(target);
        if (!frame.content) throw std::invalid_argument("渲染缺少发布帧内容");
        if (output.width_ <= 0 || output.height_ <= 0) return;
        // 设备丢失可能在一帧里的任何调用上暴露：重建后整帧重画，连续失败两次才上报
        for (int attempt = 0;; ++attempt) {
            try {
                if (impl_->deviceLost()) impl_->rebuildDevice();
                output.ensure();
                impl_->drawFrame(output, frame);
                if (output.swapChain) {
                    if (impl_->injectedLossRemaining > 0 && impl_->injectedLossOnPresent) {
                        --impl_->injectedLossRemaining;
                        throw PlatformError("呈现失败", impl_->injectedLossCode);
                    }
                    const auto presented = output.swapChain->Present(1, 0);
                    if (FAILED(presented)) throw PlatformError("呈现失败", presented);
                }
                ++impl_->counters.frames;
                return;
            } catch (const PlatformError& error) {
                impl_->pending.clear();
                impl_->states.clear();
                impl_->batchImage = impl_->batchCoverage = nullptr;
                impl_->focused = {};
                impl_->focusedOps = {};
                impl_->releaseFrameLayers();
                impl_->layerRecordStack.clear();
                impl_->activeRecordingLayers.clear();
                for (auto& rt : impl_->offscreenPool) rt.inUse = false;
                for (auto& mask : impl_->masks) mask.used = false;
                if (attempt >= 2 || !(isDeviceLost(error.code()) || impl_->deviceLost())) throw;
                core::Log::w(Impl::TAG, "显示设备丢失，正在重建", error.what());
                impl_->rebuildDevice();
            } catch (...) {
                impl_->pending.clear();
                impl_->states.clear();
                impl_->batchImage = impl_->batchCoverage = nullptr;
                impl_->focused = {};
                impl_->focusedOps = {};
                impl_->releaseFrameLayers();
                impl_->layerRecordStack.clear();
                impl_->activeRecordingLayers.clear();
                for (auto& rt : impl_->offscreenPool) rt.inUse = false;
                for (auto& mask : impl_->masks) mask.used = false;
                throw;
            }
        }
    }

    std::vector<std::uint32_t> Renderer::readPixels(RenderTarget& target) {
        auto& output = static_cast<TargetImpl&>(target);
        if (output.window || !output.staging || output.generation != impl_->deviceGeneration) throw std::logic_error("只有画过的离屏目标可以读回像素");
        impl_->context->CopyResource(output.staging.Get(), output.texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(impl_->context->Map(output.staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "无法读回像素");
        const auto width = static_cast<std::size_t>(output.bufferWidth), height = static_cast<std::size_t>(output.bufferHeight);
        std::vector<std::uint32_t> pixels(width * height);
        for (std::size_t row = 0; row < height; ++row) std::memcpy(pixels.data() + row * width, static_cast<const std::uint8_t*>(mapped.pData) + row * mapped.RowPitch, width * 4);
        impl_->context->Unmap(output.staging.Get(), 0);
        return pixels;
    }
}
