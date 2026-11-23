#include <d3d11/RendererImpl.h>

#include <core/Modifier.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace canton::d3d11 {
    namespace {
        using core::Affine;
        using core::BlendMode;
        using core::ColorFilter;
        using core::ColorFilterKind;
        using core::Rect;
        using core::TileMode;

        constexpr float kBlurSigmaScale = 0.57735f;
        constexpr int kMaxOffscreenDim = 8192;
        constexpr int kMaxBlurPad = 256;

        int bucketDim(int value) noexcept {
            value = std::clamp(value, 1, kMaxOffscreenDim);
            if (value <= 512) return ((value + 63) / 64) * 64;
            if (value <= 2048) return ((value + 127) / 128) * 128;
            return std::min(kMaxOffscreenDim, ((value + 255) / 256) * 256);
        }

        D3D11_TEXTURE_ADDRESS_MODE toAddressMode(TileMode mode) noexcept {
            switch (mode) {
                case TileMode::Clamp:
                    return D3D11_TEXTURE_ADDRESS_CLAMP;
                case TileMode::Repeated:
                    return D3D11_TEXTURE_ADDRESS_WRAP;
                case TileMode::Mirror:
                    return D3D11_TEXTURE_ADDRESS_MIRROR;
                case TileMode::Decal:
                    return D3D11_TEXTURE_ADDRESS_BORDER;
            }
            return D3D11_TEXTURE_ADDRESS_CLAMP;
        }

        bool axisAlignedTransform(const Affine& t) noexcept {
            return t.b == 0.0 && t.c == 0.0;
        }

        bool blendModeZeroPreservesDst(BlendMode mode) noexcept {
            switch (mode) {
                case BlendMode::Dst:
                case BlendMode::SrcOver:
                case BlendMode::DstOver:
                case BlendMode::DstOut:
                case BlendMode::SrcAtop:
                case BlendMode::Xor:
                case BlendMode::Plus:
                case BlendMode::Screen:
                    return true;
                default:
                    return false;
            }
        }

        PixelRect toPixelBounds(Rect rect) noexcept {
            const auto bound = [](float v) { return static_cast<int>(std::clamp(v, -1.0e7f, 1.0e7f)); };
            return {bound(std::floor(rect.x)), bound(std::floor(rect.y)), bound(std::ceil(rect.x + rect.width)), bound(std::ceil(rect.y + rect.height))};
        }

        void unpackPremulColor(float* out, std::uint32_t color) noexcept {
            const float a = static_cast<float>((color >> 24) & 0xffu) / 255.0f;
            out[0] = static_cast<float>((color >> 16) & 0xffu) / 255.0f * a;
            out[1] = static_cast<float>((color >> 8) & 0xffu) / 255.0f * a;
            out[2] = static_cast<float>(color & 0xffu) / 255.0f * a;
            out[3] = a;
        }

        void unpackStraightColor(float* out, std::uint32_t color) noexcept {
            out[0] = static_cast<float>((color >> 16) & 0xffu) / 255.0f;
            out[1] = static_cast<float>((color >> 8) & 0xffu) / 255.0f;
            out[2] = static_cast<float>(color & 0xffu) / 255.0f;
            out[3] = static_cast<float>((color >> 24) & 0xffu) / 255.0f;
        }

        void fillColorFilterConstants(CompositeParamsConstants& params, const ColorFilter& filter) noexcept {
            params.filterMeta[0] = static_cast<float>(static_cast<int>(filter.kind));
            params.filterMeta[1] = static_cast<float>(static_cast<int>(filter.blendMode));
            if (filter.kind == ColorFilterKind::Tint) {
                unpackPremulColor(params.filterColor, filter.color);
            } else if (filter.kind == ColorFilterKind::ColorMatrix) {
                for (int c = 0; c < 4; ++c) {
                    params.matrixRow0[c] = filter.matrix[static_cast<std::size_t>(c)];
                    params.matrixRow1[c] = filter.matrix[static_cast<std::size_t>(5 + c)];
                    params.matrixRow2[c] = filter.matrix[static_cast<std::size_t>(10 + c)];
                    params.matrixRow3[c] = filter.matrix[static_cast<std::size_t>(15 + c)];
                }
                params.matrixBias[0] = filter.matrix[4] / 255.0f;
                params.matrixBias[1] = filter.matrix[9] / 255.0f;
                params.matrixBias[2] = filter.matrix[14] / 255.0f;
                params.matrixBias[3] = filter.matrix[19] / 255.0f;
            } else if (filter.kind == ColorFilterKind::Lighting) {
                unpackStraightColor(params.filterColor, filter.multiply);
                unpackStraightColor(params.filterAdd, filter.add);
            }
        }

        void unbindPsSrvs(ID3D11DeviceContext* ctx) noexcept {
            ID3D11ShaderResourceView* empty[16]{};
            ctx->PSSetShaderResources(0, 16, empty);
        }

        template <typename T>
        void uploadCbuffer(ID3D11DeviceContext* ctx, ID3D11Buffer* buf, const T& data) {
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(ctx->Map(buf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "无法写入常量缓冲");
            std::memcpy(mapped.pData, &data, sizeof(T));
            ctx->Unmap(buf, 0);
        }
    }

    void Renderer::Impl::bindQuadPipeline(ID3D11BlendState* blendState) {
        unbindPsSrvs(context.Get());
        context->OMSetRenderTargets(1, &targetView, nullptr);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(targetWidth), static_cast<float>(targetHeight), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->RSSetState(rasterizer.Get());
        context->OMSetBlendState(blendState ? blendState : blend.Get(), nullptr, 0xffffffffu);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->IASetInputLayout(inputLayout.Get());
        const UINT stride = sizeof(QuadInstance), offset = 0;
        context->IASetVertexBuffers(0, 1, instanceBuffer.GetAddressOf(), &stride, &offset);
        context->VSSetShader(vertexShader.Get(), nullptr, 0);
        context->PSSetShader(pixelShader.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, constantBuffer.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, constantBuffer.GetAddressOf());
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
    }

    ID3D11SamplerState* Renderer::Impl::samplerFor(TileMode tileX, TileMode tileY, bool nearest) {
        const std::size_t idx = (static_cast<std::size_t>(tileX) & 3u) | ((static_cast<std::size_t>(tileY) & 3u) << 2) | (nearest ? 16u : 0u);
        if (!samplers[idx]) {
            D3D11_SAMPLER_DESC desc{};
            desc.Filter = nearest ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            desc.AddressU = toAddressMode(tileX);
            desc.AddressV = toAddressMode(tileY);
            desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            desc.MaxLOD = D3D11_FLOAT32_MAX;
            desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = desc.BorderColor[3] = 0.0f;
            check(device->CreateSamplerState(&desc, samplers[idx].ReleaseAndGetAddressOf()), "无法创建着色器采样器");
        }
        return samplers[idx].Get();
    }

    ID3D11PixelShader* Renderer::Impl::resolveUserShader(const core::RuntimeEffectProgramRef& program) {
        if (!program || program->dxbc.empty()) return nullptr;
        const UserShaderKey key{program->identity, program->revision};
        if (const auto it = userShaders.find(key); it != userShaders.end()) return it->second.Get();
        ComPtr<ID3D11PixelShader> ps;
        check(device->CreatePixelShader(program->dxbc.data(), program->dxbc.size(), nullptr, ps.GetAddressOf()), "无法从 DXBC 创建用户像素着色器");
        ++counters.userShadersCompiled;
        auto* raw = ps.Get();
        userShaders.emplace(key, std::move(ps));
        return raw;
    }

    void Renderer::Impl::ensureUserCbuffer(std::uint32_t slot, std::uint32_t byteSize) {
        if (slot >= userCbuffers.size()) return;
        const std::uint32_t aligned = std::max<std::uint32_t>(16u, (byteSize + 15u) & ~15u);
        if (userCbuffers[slot] && userCbufferSizes[slot] >= aligned) return;
        const std::uint32_t allocSize = std::max<std::uint32_t>(aligned, 256u);
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = allocSize;
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        check(device->CreateBuffer(&desc, nullptr, userCbuffers[slot].ReleaseAndGetAddressOf()), "无法创建用户着色器常量缓冲");
        userCbufferSizes[slot] = allocSize;
    }

    int Renderer::Impl::acquireOffscreen(int width, int height, bool exactSize) {
        width = std::clamp(width, 1, kMaxOffscreenDim);
        height = std::clamp(height, 1, kMaxOffscreenDim);
        const int targetW = exactSize ? width : bucketDim(width);
        const int targetH = exactSize ? height : bucketDim(height);

        int bestIdx = -1;
        for (std::size_t i = 0; i < offscreenPool.size(); ++i) {
            const auto& rt = offscreenPool[i];
            if (rt.inUse) continue;
            if (rt.width == targetW && rt.height == targetH) {
                bestIdx = static_cast<int>(i);
                break;
            }
        }

        if (bestIdx >= 0) {
            auto& rt = offscreenPool[static_cast<std::size_t>(bestIdx)];
            rt.inUse = true;
            rt.lastUsedFrame = frameNumber;
            ++counters.offscreenTargetReuses;
            return bestIdx;
        }

        OffscreenTarget created;
        created.width = targetW;
        created.height = targetH;
        created.inUse = true;
        created.lastUsedFrame = frameNumber;

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = static_cast<UINT>(targetW);
        desc.Height = static_cast<UINT>(targetH);
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(device->CreateTexture2D(&desc, nullptr, created.texture.GetAddressOf()), "无法创建离屏渲染纹理");
        check(device->CreateRenderTargetView(created.texture.Get(), nullptr, created.rtv.GetAddressOf()), "无法创建离屏渲染目标视图");
        check(device->CreateShaderResourceView(created.texture.Get(), nullptr, created.srv.GetAddressOf()), "无法创建离屏着色器资源视图");
        offscreenPool.push_back(std::move(created));
        ++counters.offscreenTargetsCreated;
        return static_cast<int>(offscreenPool.size() - 1);
    }

    void Renderer::Impl::releaseOffscreen(int index) noexcept {
        if (index < 0 || static_cast<std::size_t>(index) >= offscreenPool.size()) return;
        offscreenPool[static_cast<std::size_t>(index)].inUse = false;
        offscreenPool[static_cast<std::size_t>(index)].lastUsedFrame = frameNumber;
    }

    void Renderer::Impl::releaseFrameLayers() noexcept {
        for (const auto& [id, layer] : frameRecordedLayers) {
            releaseOffscreen(layer.rtIndex);
        }
        frameRecordedLayers.clear();
    }

    void Renderer::Impl::runUserShaderPass(const core::RuntimeShaderData& shader, Rect localQuad, const Affine& transform, int vpWidth, int vpHeight, Rect fragBox, ID3D11ShaderResourceView* extraTexture, std::string_view extraTextureName) {
        if (!shader.program) return;
        auto* ps = resolveUserShader(shader.program);
        if (!ps) return;

        UserVsConstants vsConst;
        vsConst.rect[0] = localQuad.x;
        vsConst.rect[1] = localQuad.y;
        vsConst.rect[2] = localQuad.width;
        vsConst.rect[3] = localQuad.height;
        vsConst.xform0[0] = static_cast<float>(transform.a);
        vsConst.xform0[1] = static_cast<float>(transform.b);
        vsConst.xform0[2] = static_cast<float>(transform.c);
        vsConst.xform0[3] = static_cast<float>(transform.d);
        vsConst.xform1[0] = static_cast<float>(transform.tx);
        vsConst.xform1[1] = static_cast<float>(transform.ty);
        vsConst.xform1[2] = static_cast<float>(std::max(vpWidth, 1));
        vsConst.xform1[3] = static_cast<float>(std::max(vpHeight, 1));
        const float boxW = std::max(fragBox.width, 1e-4f);
        const float boxH = std::max(fragBox.height, 1e-4f);
        vsConst.fragRect[0] = fragBox.x;
        vsConst.fragRect[1] = fragBox.y;
        vsConst.fragRect[2] = 1.0f / boxW;
        vsConst.fragRect[3] = 1.0f / boxH;
        uploadCbuffer(context.Get(), userVsBuffer.Get(), vsConst);

        CantonConstants cantonConst;
        cantonConst.resolution[0] = fragBox.width;
        cantonConst.resolution[1] = fragBox.height;
        cantonConst.invResolution[0] = 1.0f / boxW;
        cantonConst.invResolution[1] = 1.0f / boxH;
        uploadCbuffer(context.Get(), cantonCbuffer.Get(), cantonConst);

        std::array<ID3D11Buffer*, 13> psCbuffers{};
        for (const auto& packed : shader.packedBuffers) {
            if (packed.slot >= 12 || packed.bytes.empty()) continue;
            ensureUserCbuffer(packed.slot, static_cast<std::uint32_t>(packed.bytes.size()));
            D3D11_MAPPED_SUBRESOURCE mapped{};
            check(context->Map(userCbuffers[packed.slot].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "无法写入用户着色器常量缓冲");
            std::memcpy(mapped.pData, packed.bytes.data(), packed.bytes.size());
            context->Unmap(userCbuffers[packed.slot].Get(), 0);
            psCbuffers[packed.slot] = userCbuffers[packed.slot].Get();
        }
        psCbuffers[12] = cantonCbuffer.Get();

        std::array<ID3D11ShaderResourceView*, 12> srvs{};
        srvs.fill(whiteView.Get());
        std::array<ID3D11SamplerState*, 16> smps{};
        auto* defaultLinear = samplerFor(TileMode::Clamp, TileMode::Clamp, false);
        smps.fill(defaultLinear);
        smps[11] = samplerFor(TileMode::Clamp, TileMode::Clamp, true);
        smps[12] = defaultLinear;
        smps[13] = samplerFor(TileMode::Repeated, TileMode::Repeated, false);
        smps[14] = samplerFor(TileMode::Mirror, TileMode::Mirror, false);
        smps[15] = samplerFor(TileMode::Decal, TileMode::Decal, false);

        const auto& reflection = shader.program->reflection;
        const auto findTextureSlot = [&](std::string_view name) -> int {
            for (const auto& slot : reflection.textures) {
                if (slot.name == name && slot.bindPoint < 12) return static_cast<int>(slot.bindPoint);
            }
            return -1;
        };

        for (const auto& texInput : shader.textures) {
            const int slot = findTextureSlot(texInput.name);
            if (slot < 0 || !texInput.painter.content) continue;
            const ImageTexture* img = imageTexture(texInput.painter.content);
            if (!img) {
                if (const auto* vec = dynamic_cast<const core::VectorContent*>(texInput.painter.content.get())) {
                    const auto sz = vec->intrinsicSize.value_or(core::Size{64.0f, 64.0f});
                    const int vw = std::clamp(static_cast<int>(std::ceil(sz.width)), 1, 2048);
                    const int vh = std::clamp(static_cast<int>(std::ceil(sz.height)), 1, 2048);
                    img = vectorTexture(texInput.painter.content, {0, 0, sz.width, sz.height}, vw, vh);
                }
            }
            if (img && img->view) {
                srvs[static_cast<std::size_t>(slot)] = img->view.Get();
                if (slot < 11) smps[static_cast<std::size_t>(slot)] = samplerFor(texInput.tileModeX, texInput.tileModeY, texInput.nearest);
            }
        }

        if (extraTexture) {
            int slot = -1;
            if (!extraTextureName.empty()) {
                slot = findTextureSlot(extraTextureName);
            }
            if (slot < 0 && !reflection.textures.empty() && reflection.textures.front().bindPoint < 12) {
                slot = static_cast<int>(reflection.textures.front().bindPoint);
            }
            if (slot >= 0) {
                srvs[static_cast<std::size_t>(slot)] = extraTexture;
                if (slot < 11) smps[static_cast<std::size_t>(slot)] = defaultLinear;
            }
        }

        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(userVertexShader.Get(), nullptr, 0);
        ID3D11Buffer* vsCb = userVsBuffer.Get();
        context->VSSetConstantBuffers(13, 1, &vsCb);
        context->PSSetShader(ps, nullptr, 0);
        context->PSSetConstantBuffers(0, 13, psCbuffers.data());
        context->PSSetShaderResources(0, 12, srvs.data());
        context->PSSetSamplers(0, 16, smps.data());
        context->Draw(4, 0);
        ++counters.drawCalls;
        ++counters.instances;
        unbindPsSrvs(context.Get());
    }

    Renderer::Impl::EffectChainResult Renderer::Impl::runRenderEffectChain(int srcRt, bool ownsSrc, int contentW, int contentH, const core::RenderEffectRef& effect, bool clipped) {
        EffectChainResult result{srcRt, contentW, contentH, 0.0f, 0.0f, ownsSrc};
        if (!effect || effect->steps.empty() || srcRt < 0) return result;

        const auto swapCurrent = [&](int nextRt, int nextW, int nextH) {
            if (result.ownsRt) releaseOffscreen(result.rtIndex);
            result.rtIndex = nextRt;
            result.contentWidth = nextW;
            result.contentHeight = nextH;
            result.ownsRt = true;
        };

        const auto blitSubrect = [&](int fromRt, int fromW, int fromH, int toRt, int toW, int toH, float shiftX, float shiftY) {
            unbindPsSrvs(context.Get());
            auto* rtv = offscreenPool[static_cast<std::size_t>(toRt)].rtv.Get();
            const float zero[4]{0, 0, 0, 0};
            context->OMSetRenderTargets(1, &rtv, nullptr);
            context->ClearRenderTargetView(rtv, zero);
            const D3D11_VIEWPORT vp{0, 0, static_cast<float>(toW), static_cast<float>(toH), 0, 1};
            const D3D11_RECT sc{0, 0, toW, toH};
            context->RSSetViewports(1, &vp);
            context->RSSetScissorRects(1, &sc);
            context->RSSetState(rasterizer.Get());
            context->OMSetBlendState(replace.Get(), nullptr, 0xffffffffu);

            PostVsConstants vs{};
            vs.rect[0] = shiftX;
            vs.rect[1] = shiftY;
            vs.rect[2] = static_cast<float>(fromW);
            vs.rect[3] = static_cast<float>(fromH);
            vs.xform0[0] = 1.0f;
            vs.xform0[3] = 1.0f;
            vs.xform1[2] = static_cast<float>(toW);
            vs.xform1[3] = static_cast<float>(toH);
            const auto& srcTarget = offscreenPool[static_cast<std::size_t>(fromRt)];
            vs.uvRect[0] = 0.0f;
            vs.uvRect[1] = 0.0f;
            vs.uvRect[2] = static_cast<float>(fromW) / static_cast<float>(srcTarget.width);
            vs.uvRect[3] = static_cast<float>(fromH) / static_cast<float>(srcTarget.height);
            uploadCbuffer(context.Get(), postVsBuffer.Get(), vs);

            CompositeParamsConstants ps{};
            ps.blendAlpha[0] = 1.0f;
            uploadCbuffer(context.Get(), postPsBuffer.Get(), ps);

            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            context->VSSetShader(postVertexShader.Get(), nullptr, 0);
            ID3D11Buffer* vsCb = postVsBuffer.Get();
            context->VSSetConstantBuffers(0, 1, &vsCb);
            context->PSSetShader(colorFilterPixelShader.Get(), nullptr, 0);
            ID3D11Buffer* psCb = postPsBuffer.Get();
            context->PSSetConstantBuffers(1, 1, &psCb);
            ID3D11ShaderResourceView* srv = srcTarget.srv.Get();
            context->PSSetShaderResources(0, 1, &srv);
            ID3D11SamplerState* smp = sampler.Get();
            context->PSSetSamplers(0, 1, &smp);
            context->Draw(4, 0);
            ++counters.drawCalls;
            ++counters.instances;
            ++counters.offscreenPasses;
            unbindPsSrvs(context.Get());
        };

        const auto& steps = effect->steps;
        for (std::size_t idx = 0; idx < steps.size(); ++idx) {
            const auto& step = steps[idx];
            const bool nextIsShader = (idx + 1 < steps.size() && steps[idx + 1].kind == core::RenderEffectStepKind::Shader);

            if (step.kind == core::RenderEffectStepKind::Blur) {
                const float sigmaX = step.radiusX > 0.0f ? kBlurSigmaScale * step.radiusX + 0.5f : 0.0f;
                const float sigmaY = step.radiusY > 0.0f ? kBlurSigmaScale * step.radiusY + 0.5f : 0.0f;
                if (sigmaX <= 0.0f && sigmaY <= 0.0f) continue;

                const int padX = (step.edgeTreatment == TileMode::Decal && !clipped && sigmaX > 0.0f) ? std::min(kMaxBlurPad, static_cast<int>(std::ceil(sigmaX * 3.0f))) : 0;
                const int padY = (step.edgeTreatment == TileMode::Decal && !clipped && sigmaY > 0.0f) ? std::min(kMaxBlurPad, static_cast<int>(std::ceil(sigmaY * 3.0f))) : 0;

                const auto runBlurAxis = [&](float dirX, float dirY, float sigma, int outW, int outH, float shiftX, float shiftY, bool exactOut) {
                    const int dstRt = acquireOffscreen(outW, outH, exactOut);
                    unbindPsSrvs(context.Get());
                    auto* rtv = offscreenPool[static_cast<std::size_t>(dstRt)].rtv.Get();
                    context->OMSetRenderTargets(1, &rtv, nullptr);
                    const D3D11_VIEWPORT vp{0, 0, static_cast<float>(outW), static_cast<float>(outH), 0, 1};
                    const D3D11_RECT sc{0, 0, outW, outH};
                    context->RSSetViewports(1, &vp);
                    context->RSSetScissorRects(1, &sc);
                    context->RSSetState(rasterizer.Get());
                    context->OMSetBlendState(replace.Get(), nullptr, 0xffffffffu);

                    PostVsConstants vs{};
                    vs.rect[2] = static_cast<float>(outW);
                    vs.rect[3] = static_cast<float>(outH);
                    vs.xform0[0] = 1.0f;
                    vs.xform0[3] = 1.0f;
                    vs.xform1[2] = static_cast<float>(outW);
                    vs.xform1[3] = static_cast<float>(outH);
                    uploadCbuffer(context.Get(), postVsBuffer.Get(), vs);

                    const auto& srcTarget = offscreenPool[static_cast<std::size_t>(result.rtIndex)];
                    BlurParamsConstants bp{};
                    bp.sizeAndInv[0] = static_cast<float>(result.contentWidth);
                    bp.sizeAndInv[1] = static_cast<float>(result.contentHeight);
                    bp.sizeAndInv[2] = 1.0f / static_cast<float>(srcTarget.width);
                    bp.sizeAndInv[3] = 1.0f / static_cast<float>(srcTarget.height);
                    bp.dirSigma[0] = dirX;
                    bp.dirSigma[1] = dirY;
                    bp.dirSigma[2] = sigma;
                    bp.dirSigma[3] = static_cast<float>(std::clamp(static_cast<int>(std::ceil(sigma * 3.0f)), 1, 256));
                    bp.tileShift[0] = static_cast<float>(static_cast<int>(step.edgeTreatment));
                    bp.tileShift[1] = shiftX;
                    bp.tileShift[2] = shiftY;
                    uploadCbuffer(context.Get(), postPsBuffer.Get(), bp);

                    context->IASetInputLayout(nullptr);
                    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                    context->VSSetShader(postVertexShader.Get(), nullptr, 0);
                    ID3D11Buffer* vsCb = postVsBuffer.Get();
                    context->VSSetConstantBuffers(0, 1, &vsCb);
                    context->PSSetShader(blurPixelShader.Get(), nullptr, 0);
                    ID3D11Buffer* psCb = postPsBuffer.Get();
                    context->PSSetConstantBuffers(1, 1, &psCb);
                    ID3D11ShaderResourceView* srv = srcTarget.srv.Get();
                    context->PSSetShaderResources(0, 1, &srv);
                    ID3D11SamplerState* smp = sampler.Get();
                    context->PSSetSamplers(0, 1, &smp);
                    context->Draw(4, 0);
                    ++counters.drawCalls;
                    ++counters.instances;
                    ++counters.offscreenPasses;
                    unbindPsSrvs(context.Get());
                    swapCurrent(dstRt, outW, outH);
                };

                if (sigmaX > 0.0f) {
                    const int nextW = std::clamp(result.contentWidth + 2 * padX, 1, kMaxOffscreenDim);
                    runBlurAxis(1.0f, 0.0f, sigmaX, nextW, result.contentHeight, -static_cast<float>(padX), 0.0f, sigmaY <= 0.0f && nextIsShader);
                    result.offsetX -= static_cast<float>(padX);
                }
                if (sigmaY > 0.0f) {
                    const int nextH = std::clamp(result.contentHeight + 2 * padY, 1, kMaxOffscreenDim);
                    runBlurAxis(0.0f, 1.0f, sigmaY, result.contentWidth, nextH, 0.0f, -static_cast<float>(padY), nextIsShader);
                    result.offsetY -= static_cast<float>(padY);
                }
            } else if (step.kind == core::RenderEffectStepKind::Offset) {
                if (step.offsetX == 0.0f && step.offsetY == 0.0f) continue;
                if (!clipped && idx + 1 == steps.size()) {
                    result.offsetX += step.offsetX;
                    result.offsetY += step.offsetY;
                } else {
                    const int dstRt = acquireOffscreen(result.contentWidth, result.contentHeight, nextIsShader);
                    blitSubrect(result.rtIndex, result.contentWidth, result.contentHeight, dstRt, result.contentWidth, result.contentHeight, step.offsetX, step.offsetY);
                    swapCurrent(dstRt, result.contentWidth, result.contentHeight);
                }
            } else if (step.kind == core::RenderEffectStepKind::ColorFilter) {
                if (!step.colorFilter) continue;
                const int dstRt = acquireOffscreen(result.contentWidth, result.contentHeight, nextIsShader);
                unbindPsSrvs(context.Get());
                auto* rtv = offscreenPool[static_cast<std::size_t>(dstRt)].rtv.Get();
                context->OMSetRenderTargets(1, &rtv, nullptr);
                const D3D11_VIEWPORT vp{0, 0, static_cast<float>(result.contentWidth), static_cast<float>(result.contentHeight), 0, 1};
                const D3D11_RECT sc{0, 0, result.contentWidth, result.contentHeight};
                context->RSSetViewports(1, &vp);
                context->RSSetScissorRects(1, &sc);
                context->RSSetState(rasterizer.Get());
                context->OMSetBlendState(replace.Get(), nullptr, 0xffffffffu);

                const auto& srcTarget = offscreenPool[static_cast<std::size_t>(result.rtIndex)];
                PostVsConstants vs{};
                vs.rect[2] = static_cast<float>(result.contentWidth);
                vs.rect[3] = static_cast<float>(result.contentHeight);
                vs.xform0[0] = 1.0f;
                vs.xform0[3] = 1.0f;
                vs.xform1[2] = static_cast<float>(result.contentWidth);
                vs.xform1[3] = static_cast<float>(result.contentHeight);
                vs.uvRect[2] = static_cast<float>(result.contentWidth) / static_cast<float>(srcTarget.width);
                vs.uvRect[3] = static_cast<float>(result.contentHeight) / static_cast<float>(srcTarget.height);
                uploadCbuffer(context.Get(), postVsBuffer.Get(), vs);

                CompositeParamsConstants ps{};
                fillColorFilterConstants(ps, step.colorFilter);
                uploadCbuffer(context.Get(), postPsBuffer.Get(), ps);

                context->IASetInputLayout(nullptr);
                context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                context->VSSetShader(postVertexShader.Get(), nullptr, 0);
                ID3D11Buffer* vsCb = postVsBuffer.Get();
                context->VSSetConstantBuffers(0, 1, &vsCb);
                context->PSSetShader(colorFilterPixelShader.Get(), nullptr, 0);
                ID3D11Buffer* psCb = postPsBuffer.Get();
                context->PSSetConstantBuffers(1, 1, &psCb);
                ID3D11ShaderResourceView* srv = srcTarget.srv.Get();
                context->PSSetShaderResources(0, 1, &srv);
                ID3D11SamplerState* smp = sampler.Get();
                context->PSSetSamplers(0, 1, &smp);
                context->Draw(4, 0);
                ++counters.drawCalls;
                ++counters.instances;
                ++counters.offscreenPasses;
                unbindPsSrvs(context.Get());
                swapCurrent(dstRt, result.contentWidth, result.contentHeight);
            } else if (step.kind == core::RenderEffectStepKind::Shader) {
                if (!step.shader || !step.shader->program) continue;
                // 确保传入用户着色器的内容纹理尺寸与逻辑内容 1:1 匹配（避免分桶纹理边角填充串样）
                const auto& curTarget = offscreenPool[static_cast<std::size_t>(result.rtIndex)];
                if (curTarget.width != result.contentWidth || curTarget.height != result.contentHeight) {
                    const int exactSrc = acquireOffscreen(result.contentWidth, result.contentHeight, true);
                    blitSubrect(result.rtIndex, result.contentWidth, result.contentHeight, exactSrc, result.contentWidth, result.contentHeight, 0.0f, 0.0f);
                    swapCurrent(exactSrc, result.contentWidth, result.contentHeight);
                }

                const int dstRt = acquireOffscreen(result.contentWidth, result.contentHeight, nextIsShader);
                unbindPsSrvs(context.Get());
                auto* rtv = offscreenPool[static_cast<std::size_t>(dstRt)].rtv.Get();
                const float zero[4]{0, 0, 0, 0};
                context->OMSetRenderTargets(1, &rtv, nullptr);
                context->ClearRenderTargetView(rtv, zero);
                const D3D11_VIEWPORT vp{0, 0, static_cast<float>(result.contentWidth), static_cast<float>(result.contentHeight), 0, 1};
                const D3D11_RECT sc{0, 0, result.contentWidth, result.contentHeight};
                context->RSSetViewports(1, &vp);
                context->RSSetScissorRects(1, &sc);
                context->RSSetState(rasterizer.Get());
                context->OMSetBlendState(replace.Get(), nullptr, 0xffffffffu);

                const Rect full{0.0f, 0.0f, static_cast<float>(result.contentWidth), static_cast<float>(result.contentHeight)};
                auto* srcSrv = offscreenPool[static_cast<std::size_t>(result.rtIndex)].srv.Get();
                runUserShaderPass(*step.shader, full, Affine{}, result.contentWidth, result.contentHeight, full, srcSrv, step.uniformShaderName);
                ++counters.offscreenPasses;
                swapCurrent(dstRt, result.contentWidth, result.contentHeight);
            }
        }

        return result;
    }

    void Renderer::Impl::compositeOffscreen(int srcRt, int contentW, int contentH, Rect localRect, const Affine& transform, float alpha, BlendMode blendMode, const ColorFilter& colorFilter, ID3D11ShaderResourceView* coverageMask, int covOriginX, int covOriginY, bool useBoxAA) {
        if (srcRt < 0 || contentW <= 0 || contentH <= 0) return;
        flush();
        const auto& current = state();
        if (current.scissor.empty()) return;
        if (blendMode == BlendMode::Dst) return;

        const bool hasClipOrMask = current.clip.kind != 0 || current.mask >= 0 || coverageMask != nullptr || useBoxAA || !axisAlignedTransform(transform);
        const bool clearPunchOut = (blendMode == BlendMode::Clear);
        const bool needAdvancedBlend = !clearPunchOut && (!core::isHardwareBlendMode(blendMode) || (!blendModeZeroPreservesDst(blendMode) && hasClipOrMask));

        Rect quadRect = localRect;
        if (useBoxAA) {
            const float pad = static_cast<float>(1.0 / std::max(std::min(std::hypot(transform.a, transform.b), std::hypot(transform.c, transform.d)), 1e-3));
            quadRect = {localRect.x - pad, localRect.y - pad, localRect.width + 2.0f * pad, localRect.height + 2.0f * pad};
        }

        int backdropRt = -1;
        int copyX = 0, copyY = 0;
        if (needAdvancedBlend) {
            const auto projected = transform.project(quadRect);
            if (!projected) return;
            const auto copyRect = intersect(toPixelBounds(*projected), intersect(current.scissor, {0, 0, targetWidth, targetHeight}));
            if (copyRect.empty()) return;
            copyX = copyRect.left;
            copyY = copyRect.top;
            backdropRt = acquireOffscreen(copyRect.width(), copyRect.height(), false);
            ComPtr<ID3D11Resource> targetRes;
            targetView->GetResource(targetRes.GetAddressOf());
            const D3D11_BOX srcBox{
                static_cast<UINT>(copyRect.left),
                static_cast<UINT>(copyRect.top),
                0,
                static_cast<UINT>(copyRect.right),
                static_cast<UINT>(copyRect.bottom),
                1,
            };
            context->CopySubresourceRegion(offscreenPool[static_cast<std::size_t>(backdropRt)].texture.Get(), 0, 0, 0, 0, targetRes.Get(), 0, &srcBox);
        }

        const auto& srcTarget = offscreenPool[static_cast<std::size_t>(srcRt)];
        PostVsConstants vs{};
        vs.rect[0] = quadRect.x;
        vs.rect[1] = quadRect.y;
        vs.rect[2] = quadRect.width;
        vs.rect[3] = quadRect.height;
        vs.xform0[0] = static_cast<float>(transform.a);
        vs.xform0[1] = static_cast<float>(transform.b);
        vs.xform0[2] = static_cast<float>(transform.c);
        vs.xform0[3] = static_cast<float>(transform.d);
        vs.xform1[0] = static_cast<float>(transform.tx);
        vs.xform1[1] = static_cast<float>(transform.ty);
        vs.xform1[2] = static_cast<float>(targetWidth);
        vs.xform1[3] = static_cast<float>(targetHeight);
        const float maxU = static_cast<float>(contentW) / static_cast<float>(srcTarget.width);
        const float maxV = static_cast<float>(contentH) / static_cast<float>(srcTarget.height);
        if (useBoxAA && localRect.width > 0 && localRect.height > 0) {
            const float u0 = (quadRect.x - localRect.x) / localRect.width * maxU;
            const float v0 = (quadRect.y - localRect.y) / localRect.height * maxV;
            const float u1 = (quadRect.x + quadRect.width - localRect.x) / localRect.width * maxU;
            const float v1 = (quadRect.y + quadRect.height - localRect.y) / localRect.height * maxV;
            vs.uvRect[0] = u0;
            vs.uvRect[1] = v0;
            vs.uvRect[2] = u1;
            vs.uvRect[3] = v1;
        } else {
            vs.uvRect[0] = 0.0f;
            vs.uvRect[1] = 0.0f;
            vs.uvRect[2] = maxU;
            vs.uvRect[3] = maxV;
        }
        if (backdropRt >= 0) {
            vs.dstUvRect[0] = static_cast<float>(copyX);
            vs.dstUvRect[1] = static_cast<float>(copyY);
            vs.dstUvRect[2] = 0.0f;
            vs.dstUvRect[3] = 0.0f;
        }
        uploadCbuffer(context.Get(), postVsBuffer.Get(), vs);

        CompositeParamsConstants ps{};
        if (current.clip.kind != 0) {
            const auto& inv = current.clip.inverse;
            ps.clipRow0[0] = static_cast<float>(inv.a);
            ps.clipRow0[1] = static_cast<float>(inv.c);
            ps.clipRow0[2] = static_cast<float>(inv.tx);
            ps.clipRow1[0] = static_cast<float>(inv.b);
            ps.clipRow1[1] = static_cast<float>(inv.d);
            ps.clipRow1[2] = static_cast<float>(inv.ty);
            std::copy(std::begin(current.clip.shape), std::end(current.clip.shape), ps.clipShape);
            std::copy(std::begin(current.clip.radii), std::end(current.clip.radii), ps.clipRadii);
        }
        ps.clipFlags[0] = static_cast<float>(current.clip.kind);
        ps.clipFlags[1] = current.mask >= 0 ? 1.0f : 0.0f;
        ps.clipFlags[2] = coverageMask ? 1.0f : 0.0f;
        ps.clipFlags[3] = needAdvancedBlend ? 1.0f : 0.0f;
        ps.blendAlpha[0] = std::clamp(alpha, 0.0f, 1.0f);
        ps.blendAlpha[1] = static_cast<float>(static_cast<int>(blendMode));
        ps.blendAlpha[2] = clearPunchOut ? 1.0f : 0.0f;
        ps.blendAlpha[3] = useBoxAA ? 1.0f : 0.0f;
        ps.boxShape[0] = localRect.x + localRect.width * 0.5f;
        ps.boxShape[1] = localRect.y + localRect.height * 0.5f;
        ps.boxShape[2] = localRect.width * 0.5f;
        ps.boxShape[3] = localRect.height * 0.5f;
        fillColorFilterConstants(ps, colorFilter);
        ps.filterMeta[2] = static_cast<float>(covOriginX);
        ps.filterMeta[3] = static_cast<float>(covOriginY);
        uploadCbuffer(context.Get(), postPsBuffer.Get(), ps);

        unbindPsSrvs(context.Get());
        context->OMSetRenderTargets(1, &targetView, nullptr);
        const D3D11_VIEWPORT vp{0, 0, static_cast<float>(targetWidth), static_cast<float>(targetHeight), 0, 1};
        const D3D11_RECT sc{current.scissor.left, current.scissor.top, current.scissor.right, current.scissor.bottom};
        context->RSSetViewports(1, &vp);
        context->RSSetScissorRects(1, &sc);
        context->RSSetState(rasterizer.Get());

        ID3D11BlendState* activeBlend = replace.Get();
        if (clearPunchOut) {
            activeBlend = clearPunchOutBlend.Get();
        } else if (!needAdvancedBlend) {
            activeBlend = hardwareBlends[static_cast<std::size_t>(blendMode)].Get();
        }
        context->OMSetBlendState(activeBlend, nullptr, 0xffffffffu);

        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(postVertexShader.Get(), nullptr, 0);
        ID3D11Buffer* vsCb = postVsBuffer.Get();
        context->VSSetConstantBuffers(0, 1, &vsCb);
        context->PSSetShader(compositePixelShader.Get(), nullptr, 0);
        ID3D11Buffer* psCb = postPsBuffer.Get();
        context->PSSetConstantBuffers(1, 1, &psCb);
        ID3D11ShaderResourceView* srvs[4]{
            srcTarget.srv.Get(),
            coverageMask ? coverageMask : whiteView.Get(),
            current.mask >= 0 ? masks[static_cast<std::size_t>(current.mask)].view.Get() : whiteView.Get(),
            backdropRt >= 0 ? offscreenPool[static_cast<std::size_t>(backdropRt)].srv.Get() : whiteView.Get(),
        };
        context->PSSetShaderResources(0, 4, srvs);
        ID3D11SamplerState* smp = sampler.Get();
        context->PSSetSamplers(0, 1, &smp);
        context->Draw(4, 0);
        ++counters.drawCalls;
        ++counters.instances;
        unbindPsSrvs(context.Get());

        if (backdropRt >= 0) releaseOffscreen(backdropRt);
        bindQuadPipeline();
    }

    void Renderer::Impl::drawOpWithShaderBrush(const core::DrawOp& op, float alpha) {
        if (!op.shader || !op.shader->program) return;
        flush();

        const auto bounds = core::drawOpBounds(op);
        if (!bounds.known || bounds.empty) return;
        const auto& current = state();
        const Rect localExpand{bounds.rect.x - 1.0f, bounds.rect.y - 1.0f, bounds.rect.width + 2.0f, bounds.rect.height + 2.0f};
        PixelRect devRect = current.scissor;
        // 对 src=0 时仍影响 dst 的 Porter-Duff 模式（如 DstIn/SrcIn/SrcOut/Src/DstAtop/Modulate），需覆盖整个裁剪区以清掉图元外像素
        if (blendModeZeroPreservesDst(op.blendMode) || !core::isHardwareBlendMode(op.blendMode) || op.blendMode == BlendMode::Clear) {
            const auto projected = current.transform.project(localExpand);
            if (!projected) return;
            devRect = intersect(toPixelBounds(*projected), current.scissor);
        }
        if (devRect.empty()) return;

        const int ox = devRect.left;
        const int oy = devRect.top;
        const int rw = devRect.width();
        const int rh = devRect.height();

        Rect fragBox = op.rect;
        if (op.type == core::DrawOpType::DrawLine) {
            const float minX = std::min(op.rect.x, op.lineEnd.x);
            const float minY = std::min(op.rect.y, op.lineEnd.y);
            const float w = std::max(std::abs(op.lineEnd.x - op.rect.x), op.strokeWidth);
            const float h = std::max(std::abs(op.lineEnd.y - op.rect.y), op.strokeWidth);
            fragBox = {minX, minY, w, h};
        }

        const int shaderRt = acquireOffscreen(rw, rh, false);
        const Affine shiftedTransform = Affine::translation(-static_cast<double>(ox), -static_cast<double>(oy)).after(current.transform);

        // Pass 1：运行用户像素着色器到离屏临时 RT
        unbindPsSrvs(context.Get());
        auto* shaderRtv = offscreenPool[static_cast<std::size_t>(shaderRt)].rtv.Get();
        const float zero[4]{0, 0, 0, 0};
        context->OMSetRenderTargets(1, &shaderRtv, nullptr);
        context->ClearRenderTargetView(shaderRtv, zero);
        const D3D11_VIEWPORT vp{0, 0, static_cast<float>(rw), static_cast<float>(rh), 0, 1};
        const D3D11_RECT sc{0, 0, rw, rh};
        context->RSSetViewports(1, &vp);
        context->RSSetScissorRects(1, &sc);
        context->RSSetState(rasterizer.Get());
        context->OMSetBlendState(replace.Get(), nullptr, 0xffffffffu);
        runUserShaderPass(*op.shader, localExpand, shiftedTransform, rw, rh, fragBox);
        ++counters.offscreenPasses;

        // Pass 2：绘制形状抗锯齿覆盖率掩码（直角轴对齐实心矩形可直接走解析盒抗锯齿）
        const bool needsMask = !(op.type == core::DrawOpType::FillRect && op.shape == core::DrawShapeType::Rectangle && axisAlignedTransform(current.transform));
        int covRt = -1;
        if (needsMask) {
            covRt = acquireOffscreen(rw, rh, false);
            auto* savedView = targetView;
            const int savedW = targetWidth;
            const int savedH = targetHeight;
            auto savedStates = std::move(states);

            targetView = offscreenPool[static_cast<std::size_t>(covRt)].rtv.Get();
            targetWidth = rw;
            targetHeight = rh;
            states.assign(1, RenderState{shiftedTransform, {0, 0, rw, rh}, {}, -1, false});
            bindQuadPipeline(replace.Get());
            context->ClearRenderTargetView(targetView, zero);

            core::DrawOp maskOp = op;
            maskOp.color = 0xffffffffu;
            maskOp.gradient = {};
            maskOp.shader = {};
            maskOp.blendMode = BlendMode::SrcOver;
            maskOp.colorFilter = {};
            std::size_t dummyDepth = 1;
            replayOps(std::span<const core::DrawOp>(&maskOp, 1), 1.0f, dummyDepth);
            flush();

            targetView = savedView;
            targetWidth = savedW;
            targetHeight = savedH;
            states = std::move(savedStates);
        }

        // Pass 3：合成到主目标（携带覆盖率掩码、外层裁剪、Alpha、ColorFilter 与 BlendMode）
        const float opAlpha = static_cast<float>((op.color >> 24) & 0xffu) / 255.0f * alpha;
        if (needsMask) {
            compositeOffscreen(
                shaderRt, rw, rh,
                {static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(rw), static_cast<float>(rh)},
                Affine{}, opAlpha, op.blendMode, op.colorFilter,
                offscreenPool[static_cast<std::size_t>(covRt)].srv.Get(), ox, oy, false);
            releaseOffscreen(covRt);
        } else {
            compositeOffscreen(
                shaderRt, rw, rh,
                {static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(rw), static_cast<float>(rh)},
                Affine{}, opAlpha, op.blendMode, op.colorFilter,
                nullptr, 0, 0, false);
        }
        releaseOffscreen(shaderRt);
    }

    void Renderer::Impl::drawOpIsolated(const core::DrawOp& op, float alpha) {
        flush();
        const auto bounds = core::drawOpBounds(op);
        if (!bounds.known || bounds.empty) return;
        const auto& current = state();
        const Rect localExpand{bounds.rect.x - 1.0f, bounds.rect.y - 1.0f, bounds.rect.width + 2.0f, bounds.rect.height + 2.0f};
        PixelRect devRect = current.scissor;
        // 对 src=0 时仍影响 dst 的 Porter-Duff 模式（如 DstIn/SrcIn/SrcOut/Src/DstAtop/Modulate），需覆盖整个裁剪区以清掉图元外像素
        if (blendModeZeroPreservesDst(op.blendMode) || !core::isHardwareBlendMode(op.blendMode) || op.blendMode == BlendMode::Clear) {
            const auto projected = current.transform.project(localExpand);
            if (!projected) return;
            devRect = intersect(toPixelBounds(*projected), current.scissor);
        }
        if (devRect.empty()) return;

        const int ox = devRect.left;
        const int oy = devRect.top;
        const int rw = devRect.width();
        const int rh = devRect.height();

        const int tempRt = acquireOffscreen(rw, rh, false);
        const Affine shiftedTransform = Affine::translation(-static_cast<double>(ox), -static_cast<double>(oy)).after(current.transform);

        auto* savedView = targetView;
        const int savedW = targetWidth;
        const int savedH = targetHeight;
        auto savedStates = std::move(states);

        targetView = offscreenPool[static_cast<std::size_t>(tempRt)].rtv.Get();
        targetWidth = rw;
        targetHeight = rh;
        states.assign(1, RenderState{shiftedTransform, {0, 0, rw, rh}, {}, -1, false});
        bindQuadPipeline();
        const float zero[4]{0, 0, 0, 0};
        context->ClearRenderTargetView(targetView, zero);

        core::DrawOp plain = op;
        plain.blendMode = BlendMode::SrcOver;
        plain.colorFilter = {};
        std::size_t dummyDepth = 1;
        replayOps(std::span<const core::DrawOp>(&plain, 1), 1.0f, dummyDepth);
        flush();
        ++counters.offscreenPasses;

        targetView = savedView;
        targetWidth = savedW;
        targetHeight = savedH;
        states = std::move(savedStates);

        compositeOffscreen(
            tempRt, rw, rh,
            {static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(rw), static_cast<float>(rh)},
            Affine{}, alpha, op.blendMode, op.colorFilter);
        releaseOffscreen(tempRt);
    }

    void Renderer::Impl::replayOffscreenGroup(const core::PaintFragment& fragment, float alpha) {
        const auto& layer = *fragment.layer;
        const float groupAlpha = std::clamp(alpha * layer.groupAlpha, 0.0f, 1.0f);
        if (groupAlpha <= 0.0f && blendModeZeroPreservesDst(layer.groupBlendMode)) return;

        flush();
        auto depth = states.size();
        // 先在主目标压入本层的 Transform 与 Clip，保证后续合成自动复用层的变换与形状裁剪
        replayOps(layer.before, 1.0f, depth);

        const auto& current = state();
        if (current.scissor.empty()) {
            while (states.size() > depth) popState();
            return;
        }

        const auto* transformSem = std::get_if<core::TransformModifierSemantics>(&layer.value);
        const bool isClipped = transformSem && transformSem->clip;
        const bool hasRenderEffect = static_cast<bool>(layer.groupRenderEffect) && !layer.groupRenderEffect->steps.empty();

        // 计算未经过本层 Transform 之前的局部内容包围盒
        Rect localRect = layer.bounds;
        bool hasLocalRect = localRect.width > 0.0f && localRect.height > 0.0f;
        if (!isClipped) {
            core::PaintBounds innerBounds;
            const auto merge = [&](const core::PaintBounds& b) {
                innerBounds.known = innerBounds.known && b.known;
                if (b.empty) return;
                if (innerBounds.empty) {
                    innerBounds.rect = b.rect;
                    innerBounds.empty = false;
                    return;
                }
                const float r = std::max(innerBounds.rect.x + innerBounds.rect.width, b.rect.x + b.rect.width);
                const float bt = std::max(innerBounds.rect.y + innerBounds.rect.height, b.rect.y + b.rect.height);
                innerBounds.rect.x = std::min(innerBounds.rect.x, b.rect.x);
                innerBounds.rect.y = std::min(innerBounds.rect.y, b.rect.y);
                innerBounds.rect.width = r - innerBounds.rect.x;
                innerBounds.rect.height = bt - innerBounds.rect.y;
            };
            if (fragment.content) {
                for (const auto& op : *fragment.content) merge(core::drawOpBounds(op));
            }
            for (const auto& child : fragment.children) {
                if (!child.fragment) continue;
                auto cb = child.fragment->bounds;
                if (cb.known && !cb.empty) {
                    cb.rect.x += child.offset.x;
                    cb.rect.y += child.offset.y;
                }
                merge(cb);
            }
            if (innerBounds.known && !innerBounds.empty) {
                if (!hasLocalRect) {
                    localRect = innerBounds.rect;
                    hasLocalRect = true;
                } else {
                    const float r = std::max(localRect.x + localRect.width, innerBounds.rect.x + innerBounds.rect.width);
                    const float bt = std::max(localRect.y + localRect.height, innerBounds.rect.y + innerBounds.rect.height);
                    localRect.x = std::min(localRect.x, innerBounds.rect.x);
                    localRect.y = std::min(localRect.y, innerBounds.rect.y);
                    localRect.width = r - localRect.x;
                    localRect.height = bt - localRect.y;
                }
            }
        }

        if (!hasRenderEffect) {
            // 无 RenderEffect 的离屏组：直接在设备空间紧包围盒里渲染，避免额外重采样
            PixelRect devRect = current.scissor;
            if (hasLocalRect) {
                if (const auto projected = current.transform.project({localRect.x - 1.0f, localRect.y - 1.0f, localRect.width + 2.0f, localRect.height + 2.0f})) {
                    devRect = intersect(toPixelBounds(*projected), current.scissor);
                }
            }
            if (!devRect.empty()) {
                const int ox = devRect.left;
                const int oy = devRect.top;
                const int rw = devRect.width();
                const int rh = devRect.height();
                const int groupRt = acquireOffscreen(rw, rh, false);
                const Affine shifted = Affine::translation(-static_cast<double>(ox), -static_cast<double>(oy)).after(current.transform);

                auto* savedView = targetView;
                const int savedW = targetWidth;
                const int savedH = targetHeight;
                auto savedStates = std::move(states);

                targetView = offscreenPool[static_cast<std::size_t>(groupRt)].rtv.Get();
                targetWidth = rw;
                targetHeight = rh;
                states.assign(1, RenderState{shifted, {0, 0, rw, rh}, {}, -1, false});
                bindQuadPipeline();
                const float zero[4]{0, 0, 0, 0};
                context->ClearRenderTargetView(targetView, zero);

                std::size_t innerDepth = 1;
                if (fragment.content) replayOps(*fragment.content, 1.0f, innerDepth);
                for (const auto& child : fragment.children) replayFragment(child, 1.0f);
                while (states.size() > innerDepth) popState();
                flush();
                ++counters.offscreenPasses;

                targetView = savedView;
                targetWidth = savedW;
                targetHeight = savedH;
                states = std::move(savedStates);

                compositeOffscreen(
                    groupRt, rw, rh,
                    {static_cast<float>(ox), static_cast<float>(oy), static_cast<float>(rw), static_cast<float>(rh)},
                    Affine{}, groupAlpha, layer.groupBlendMode, layer.groupColorFilter);
                releaseOffscreen(groupRt);
            }
        } else {
            // 有 RenderEffect：在层的局部坐标系（按变换缩放因子采样）渲染后执行效果链，再携带层变换与形状裁剪回贴
            if (!hasLocalRect || localRect.width <= 0.0f || localRect.height <= 0.0f) {
                while (states.size() > depth) popState();
                return;
            }
            const float sx = std::clamp(static_cast<float>(std::hypot(current.transform.a, current.transform.b)), 0.1f, 8.0f);
            const float sy = std::clamp(static_cast<float>(std::hypot(current.transform.c, current.transform.d)), 0.1f, 8.0f);
            const int rw = std::clamp(static_cast<int>(std::ceil(localRect.width * sx)), 1, 4096);
            const int rh = std::clamp(static_cast<int>(std::ceil(localRect.height * sy)), 1, 4096);
            const bool firstIsShader = layer.groupRenderEffect->steps.front().kind == core::RenderEffectStepKind::Shader;
            const int baseRt = acquireOffscreen(rw, rh, firstIsShader);

            const Affine localToRt = Affine{static_cast<double>(rw) / localRect.width, 0.0, 0.0, static_cast<double>(rh) / localRect.height, 0.0, 0.0}
                                         .after(Affine::translation(-localRect.x, -localRect.y));

            auto* savedView = targetView;
            const int savedW = targetWidth;
            const int savedH = targetHeight;
            auto savedStates = std::move(states);

            targetView = offscreenPool[static_cast<std::size_t>(baseRt)].rtv.Get();
            targetWidth = rw;
            targetHeight = rh;
            states.assign(1, RenderState{localToRt, {0, 0, rw, rh}, {}, -1, false});
            bindQuadPipeline();
            const float zero[4]{0, 0, 0, 0};
            context->ClearRenderTargetView(targetView, zero);

            std::size_t innerDepth = 1;
            if (fragment.content) replayOps(*fragment.content, 1.0f, innerDepth);
            for (const auto& child : fragment.children) replayFragment(child, 1.0f);
            while (states.size() > innerDepth) popState();
            flush();
            ++counters.offscreenPasses;

            auto chainOut = runRenderEffectChain(baseRt, true, rw, rh, layer.groupRenderEffect, isClipped);

            targetView = savedView;
            targetWidth = savedW;
            targetHeight = savedH;
            states = std::move(savedStates);

            const float invSx = localRect.width / static_cast<float>(rw);
            const float invSy = localRect.height / static_cast<float>(rh);
            const Rect outLocalRect{
                localRect.x + chainOut.offsetX * invSx,
                localRect.y + chainOut.offsetY * invSy,
                static_cast<float>(chainOut.contentWidth) * invSx,
                static_cast<float>(chainOut.contentHeight) * invSy,
            };
            compositeOffscreen(
                chainOut.rtIndex, chainOut.contentWidth, chainOut.contentHeight,
                outLocalRect, state().transform, groupAlpha, layer.groupBlendMode, layer.groupColorFilter,
                nullptr, 0, 0, !axisAlignedTransform(state().transform));
            if (chainOut.ownsRt) releaseOffscreen(chainOut.rtIndex);
        }

        replayOps(layer.after, 1.0f, depth);
        while (states.size() > depth) popState();
    }

    void Renderer::Impl::beginRecordLayer(const core::DrawOp& op, std::size_t& depth) {
        const auto layerId = op.layerHandle.identity;
        if (std::find(activeRecordingLayers.begin(), activeRecordingLayers.end(), layerId) != activeRecordingLayers.end()) {
            throw std::runtime_error("检测到 RenderLayer 自引用环：不能在录制自身时嵌套录制或绘制同一层");
        }
        flush();

        const int rw = std::clamp(static_cast<int>(std::ceil(std::max(op.rect.width, 1.0f))), 1, kMaxOffscreenDim);
        const int rh = std::clamp(static_cast<int>(std::ceil(std::max(op.rect.height, 1.0f))), 1, kMaxOffscreenDim);
        const int rtIndex = acquireOffscreen(rw, rh, false);

        SavedTargetState saved;
        saved.targetView = targetView;
        saved.targetWidth = targetWidth;
        saved.targetHeight = targetHeight;
        saved.states = std::move(states);
        saved.savedDepth = depth;
        saved.layerId = layerId;
        saved.rtIndex = rtIndex;
        saved.contentWidth = rw;
        saved.contentHeight = rh;
        layerRecordStack.push_back(std::move(saved));
        activeRecordingLayers.push_back(layerId);

        targetView = offscreenPool[static_cast<std::size_t>(rtIndex)].rtv.Get();
        targetWidth = rw;
        targetHeight = rh;
        states.assign(1, RenderState{{}, {0, 0, rw, rh}, {}, -1, false});
        depth = 1;
        bindQuadPipeline();
        const float zero[4]{0, 0, 0, 0};
        context->ClearRenderTargetView(targetView, zero);
    }

    void Renderer::Impl::endRecordLayer(std::size_t& depth) {
        if (layerRecordStack.empty()) return;
        while (states.size() > 1) popState();
        flush();
        ++counters.offscreenPasses;

        auto saved = std::move(layerRecordStack.back());
        layerRecordStack.pop_back();
        if (!activeRecordingLayers.empty()) activeRecordingLayers.pop_back();

        if (const auto prev = frameRecordedLayers.find(saved.layerId); prev != frameRecordedLayers.end()) {
            releaseOffscreen(prev->second.rtIndex);
        }
        frameRecordedLayers[saved.layerId] = RecordedLayer{saved.rtIndex, saved.contentWidth, saved.contentHeight};

        targetView = saved.targetView;
        targetWidth = saved.targetWidth;
        targetHeight = saved.targetHeight;
        states = std::move(saved.states);
        depth = saved.savedDepth;
        bindQuadPipeline();
    }

    void Renderer::Impl::drawRenderLayer(const core::DrawOp& op, float alpha) {
        if (!op.layerSnapshot) return;
        const auto& snap = *op.layerSnapshot;
        const auto layerId = snap.handle.identity;
        if (std::find(activeRecordingLayers.begin(), activeRecordingLayers.end(), layerId) != activeRecordingLayers.end()) {
            throw std::runtime_error("检测到 RenderLayer 自引用环：不能在录制 RenderLayer 时绘制该层自身");
        }
        const auto found = frameRecordedLayers.find(layerId);
        if (found == frameRecordedLayers.end()) return;

        const float finalAlpha = std::clamp(alpha * snap.alpha, 0.0f, 1.0f);
        if (finalAlpha <= 0.0f && blendModeZeroPreservesDst(snap.blendMode)) return;

        const auto recorded = found->second;
        const Rect layerRect{0.0f, 0.0f, static_cast<float>(recorded.contentWidth), static_cast<float>(recorded.contentHeight)};

        pushState();
        auto& cur = state();
        cur.transform = cur.transform.after(Affine::translation(snap.topLeftX, snap.topLeftY))
                            .after(Affine::layer(layerRect, snap.originX, snap.originY, snap.scaleX, snap.scaleY, snap.rotationZ, snap.translationX, snap.translationY));

        bool pushedClip = false;
        if (snap.clip) {
            core::DrawOp clipOp;
            clipOp.type = core::DrawOpType::PushClip;
            clipOp.rect = layerRect;
            clipOp.shape = snap.shape.type == 1 ? core::DrawShapeType::Rounded : snap.shape.type == 2 ? core::DrawShapeType::Circle : core::DrawShapeType::Rectangle;
            clipOp.corners = snap.shape.corners;
            pushClip(clipOp);
            pushedClip = true;
        }

        if (!state().scissor.empty()) {
            EffectChainResult chainOut{recorded.rtIndex, recorded.contentWidth, recorded.contentHeight, 0.0f, 0.0f, false};
            if (snap.renderEffect && !snap.renderEffect->steps.empty()) {
                flush();
                chainOut = runRenderEffectChain(recorded.rtIndex, false, recorded.contentWidth, recorded.contentHeight, snap.renderEffect, snap.clip);
            }
            const Rect outRect{chainOut.offsetX, chainOut.offsetY, static_cast<float>(chainOut.contentWidth), static_cast<float>(chainOut.contentHeight)};
            compositeOffscreen(
                chainOut.rtIndex, chainOut.contentWidth, chainOut.contentHeight,
                outRect, state().transform, finalAlpha, snap.blendMode, snap.colorFilter,
                nullptr, 0, 0, !axisAlignedTransform(state().transform));
            if (chainOut.ownsRt) releaseOffscreen(chainOut.rtIndex);
        }

        if (pushedClip) popState();
        popState();
    }
}
