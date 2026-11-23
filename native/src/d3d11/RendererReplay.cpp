#include <d3d11/RendererImpl.h>

#include <core/Modifier.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace canton::d3d11 {
    namespace {
        using core::Affine;
        using core::Rect;
        constexpr float pi = 3.14159265358979323846f;
        constexpr float unbounded = -1.0e9f;

        std::optional<Affine> inverted(const Affine& t) {
            const auto determinant = t.a * t.d - t.b * t.c;
            if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12) return std::nullopt;
            Affine result{t.d / determinant, -t.b / determinant, -t.c / determinant, t.a / determinant, 0, 0};
            result.tx = -(result.a * t.tx + result.c * t.ty);
            result.ty = -(result.b * t.tx + result.d * t.ty);
            return result;
        }

        bool axisAligned(const Affine& t) {
            return t.b == 0 && t.c == 0;
        }

        // 一个设备像素在局部坐标里有多长，用来给抗锯齿留边
        float pixelInLocal(const Affine& t) {
            const auto scale = std::min(std::hypot(t.a, t.b), std::hypot(t.c, t.d));
            return static_cast<float>(1.0 / std::max(scale, 1e-3));
        }

        float areaScale(const Affine& t) {
            return static_cast<float>(std::sqrt(std::abs(t.a * t.d - t.b * t.c)));
        }

        Rect expanded(Rect rect, float amount) {
            return {rect.x - amount, rect.y - amount, rect.width + 2 * amount, rect.height + 2 * amount};
        }

        bool drawable(Rect rect) {
            return std::isfinite(rect.x) && std::isfinite(rect.y) && rect.width > 0 && rect.height > 0 && std::isfinite(rect.width) && std::isfinite(rect.height);
        }

        void setRect(float* out, Rect rect) {
            out[0] = rect.x, out[1] = rect.y, out[2] = rect.width, out[3] = rect.height;
        }

        void setShape(float* out, Rect rect) {
            out[0] = rect.x + rect.width * 0.5f, out[1] = rect.y + rect.height * 0.5f, out[2] = std::max(rect.width, 0.0f) * 0.5f, out[3] = std::max(rect.height, 0.0f) * 0.5f;
        }

        void setRadii(float* out, const core::CornerRadii& corners, Rect rect) {
            const auto fitted = corners.fitted(rect.width, rect.height);
            out[0] = fitted.topStart, out[1] = fitted.topEnd, out[2] = fitted.bottomEnd, out[3] = fitted.bottomStart;
        }

        void setColor(float* out, std::uint32_t color, float alpha) {
            const auto a = static_cast<float>(color >> 24) / 255.0f * std::clamp(alpha, 0.0f, 1.0f);
            out[0] = static_cast<float>((color >> 16) & 0xff) / 255.0f * a;
            out[1] = static_cast<float>((color >> 8) & 0xff) / 255.0f * a;
            out[2] = static_cast<float>(color & 0xff) / 255.0f * a;
            out[3] = a;
        }

        PixelRect pixelBounds(Rect rect, bool nearest) {
            const auto bound = [](float value) {
                return static_cast<int>(std::clamp(value, -1.0e7f, 1.0e7f));
            };
            if (nearest) return {bound(std::round(rect.x)), bound(std::round(rect.y)), bound(std::round(rect.x + rect.width)), bound(std::round(rect.y + rect.height))};
            return {bound(std::floor(rect.x)), bound(std::floor(rect.y)), bound(std::ceil(rect.x + rect.width)), bound(std::ceil(rect.y + rect.height))};
        }

        bool sameClip(const AnalyticClip& a, const AnalyticClip& b) {
            if (a.kind != b.kind) return false;
            if (a.kind == 0) return true;
            const auto &m = a.inverse, &n = b.inverse;
            return m.a == n.a && m.b == n.b && m.c == n.c && m.d == n.d && m.tx == n.tx && m.ty == n.ty && std::equal(std::begin(a.shape), std::end(a.shape), std::begin(b.shape)) && std::equal(std::begin(a.radii), std::end(a.radii), std::begin(b.radii));
        }

        bool sameGpuState(const RenderState& a, const RenderState& b) {
            return a.scissor == b.scissor && a.mask == b.mask && sameClip(a.clip, b.clip);
        }
    }

    void Renderer::Impl::drawFrame(TargetImpl& output, const FrameInput& frame) {
        target = &output;
        targetView = output.view.Get();
        targetWidth = output.bufferWidth;
        targetHeight = output.bufferHeight;
        ++frameNumber;
        ensureMasks();
        releaseFrameLayers();
        layerRecordStack.clear();
        activeRecordingLayers.clear();

        float clear[4];
        setColor(clear, frame.background, 1.0f);
        bindQuadPipeline();
        context->ClearRenderTargetView(targetView, clear);

        states.assign(1, RenderState{{}, {0, 0, targetWidth, targetHeight}, {}, -1, false});
        pending.clear();
        batchImage = batchCoverage = nullptr;
        const auto& content = *frame.content;
        // 次序：场景（含原位重放的输入叠层）或错误屏，然后是诊断层
        if (content.errorFrame)
            replayScoped(content.diagnosticsErrorDrawOps, 1.0f);
        else {
            focused = content.focusedInputModifier;
            focusedOps = content.overlayDrawOps;
            replayFragment(content.scenePaint, 1.0f);
            focused = {};
            focusedOps = {};
        }
        replayScoped(frame.diagnosticsOverlay, 1.0f);
        replayScoped(content.diagnosticsFpsDrawOps, 1.0f);
        replayScoped(content.diagnosticsBadgeDrawOps, 1.0f);
        replayScoped(content.diagnosticsToastDrawOps, 1.0f);
        flush();
        states.clear();
        releaseFrameLayers();
        layerRecordStack.clear();
        activeRecordingLayers.clear();
        for (auto& mask : masks) mask.used = false;
        ID3D11ShaderResourceView* none[4]{};
        context->PSSetShaderResources(0, 4, none);
        trimCaches();
    }

    void Renderer::Impl::flush() {
        if (pending.empty()) return;
        const auto& current = state();
        if (!current.scissor.empty()) {
            FrameConstants constants;
            constants.viewport[0] = static_cast<float>(targetWidth), constants.viewport[1] = static_cast<float>(targetHeight);
            if (current.clip.kind != 0) {
                const auto& inverse = current.clip.inverse;
                constants.clipRow0[0] = static_cast<float>(inverse.a), constants.clipRow0[1] = static_cast<float>(inverse.c), constants.clipRow0[2] = static_cast<float>(inverse.tx);
                constants.clipRow1[0] = static_cast<float>(inverse.b), constants.clipRow1[1] = static_cast<float>(inverse.d), constants.clipRow1[2] = static_cast<float>(inverse.ty);
                std::copy(std::begin(current.clip.shape), std::end(current.clip.shape), constants.clipShape);
                std::copy(std::begin(current.clip.radii), std::end(current.clip.radii), constants.clipRadii);
            }
            constants.clipFlags[0] = static_cast<float>(current.clip.kind);
            constants.clipFlags[1] = current.mask >= 0 ? 1.0f : 0.0f;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (injectedLossRemaining > 0 && !injectedLossOnPresent) {
                --injectedLossRemaining;
                throw PlatformError("无法写入常量缓冲", injectedLossCode);
            }
            check(context->Map(constantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "无法写入常量缓冲");
            std::memcpy(mapped.pData, &constants, sizeof(constants));
            context->Unmap(constantBuffer.Get(), 0);

            const D3D11_RECT scissor{current.scissor.left, current.scissor.top, current.scissor.right, current.scissor.bottom};
            context->RSSetScissorRects(1, &scissor);
            ID3D11ShaderResourceView* views[4]{batchImage ? batchImage : whiteView.Get(), batchCoverage ? batchCoverage : atlasView.Get(), gradientView.Get(), current.mask >= 0 ? masks[static_cast<std::size_t>(current.mask)].view.Get() : whiteView.Get()};
            context->PSSetShaderResources(0, 4, views);
            for (std::size_t start = 0; start < pending.size(); start += instanceCapacity) {
                const auto count = std::min(instanceCapacity, pending.size() - start);
                check(context->Map(instanceBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "无法写入实例缓冲");
                std::memcpy(mapped.pData, pending.data() + start, count * sizeof(QuadInstance));
                context->Unmap(instanceBuffer.Get(), 0);
                context->DrawInstanced(4, static_cast<UINT>(count), 0, 0);
                ++counters.drawCalls;
            }
            counters.instances += pending.size();
        }
        pending.clear();
        batchImage = batchCoverage = nullptr;
    }

    void Renderer::Impl::add(const QuadInstance& instance, ID3D11ShaderResourceView* image, ID3D11ShaderResourceView* coverage) {
        // 一批只能绑一张图片和一张覆盖率纹理，换纹理就结束当前批
        if ((image && batchImage && image != batchImage) || (coverage && batchCoverage && coverage != batchCoverage)) flush();
        if (image) batchImage = image;
        if (coverage) batchCoverage = coverage;
        pending.push_back(instance);
    }

    void Renderer::Impl::pushState() {
        states.push_back(states.back());
        states.back().ownsMask = false;
    }

    void Renderer::Impl::popState() {
        if (states.size() < 2) return;
        // 裁剪没变就不必断批：变换写在实例里
        if (!sameGpuState(states.back(), states[states.size() - 2])) flush();
        if (states.back().ownsMask && states.back().mask >= 0) masks[static_cast<std::size_t>(states.back().mask)].used = false;
        states.pop_back();
    }

    bool Renderer::Impl::invisible(const core::PaintBounds& bounds) const {
        if (!bounds.known) return false;
        if (bounds.empty) return true;
        const auto projected = states.back().transform.project(expanded(bounds.rect, 1.0f));
        if (!projected) return false;
        return intersect(pixelBounds(*projected, false), states.back().scissor).empty();
    }

    QuadInstance Renderer::Impl::instanceFor(QuadKind kind, const core::Affine& t) const {
        QuadInstance instance;
        instance.xform0[0] = static_cast<float>(t.a), instance.xform0[1] = static_cast<float>(t.b), instance.xform0[2] = static_cast<float>(t.c), instance.xform0[3] = static_cast<float>(t.d);
        instance.xform1[0] = static_cast<float>(t.tx), instance.xform1[1] = static_cast<float>(t.ty), instance.xform1[2] = static_cast<float>(static_cast<int>(kind));
        return instance;
    }

    void Renderer::Impl::setBrush(QuadInstance& instance, std::uint32_t color, float alpha, const core::GradientRef& gradient, Rect box, const Affine* toLocal) {
        if (!gradient) {
            setColor(instance.color, color, alpha);
            return;
        }
        // 渐变存在时 color 只携带不透明度
        const auto opacity = static_cast<float>(color >> 24) / 255.0f * std::clamp(alpha, 0.0f, 1.0f);
        std::fill(std::begin(instance.color), std::end(instance.color), opacity);
        instance.brush[0] = static_cast<float>(static_cast<int>(gradient->kind) + 1);
        instance.brush[1] = (static_cast<float>(gradientRow(gradient)) + 0.5f) / static_cast<float>(gradientRows);
        // 渐变几何取外包盒的比例，换算成相对外包盒左上角的像素
        const auto w = box.width, h = box.height;
        auto* g = instance.brushGeometry;
        if (gradient->kind == core::GradientKind::Linear) {
            g[0] = gradient->start.x * w, g[1] = gradient->start.y * h, g[2] = gradient->end.x * w, g[3] = gradient->end.y * h;
            if (g[0] == g[2] && g[1] == g[3]) g[3] += 0.001f;
        } else {
            g[0] = gradient->center.x * w, g[1] = gradient->center.y * h;
            if (gradient->kind == core::GradientKind::Radial) {
                // 半径 1 表示到达离中心最远的角
                auto farthest = 0.0f;
                for (const auto x : {0.0f, w})
                    for (const auto y : {0.0f, h}) farthest = std::max(farthest, std::hypot(x - g[0], y - g[1]));
                g[2] = std::max(0.001f, farthest * gradient->radius);
            } else
                g[2] = gradient->startAngle * pi / 180.0f;
        }
        const Affine identity;
        const auto& local = toLocal ? *toLocal : identity;
        instance.brushRow0[0] = static_cast<float>(local.a), instance.brushRow0[1] = static_cast<float>(local.c), instance.brushRow0[2] = static_cast<float>(local.tx) - box.x;
        instance.brushRow1[0] = static_cast<float>(local.b), instance.brushRow1[1] = static_cast<float>(local.d), instance.brushRow1[2] = static_cast<float>(local.ty) - box.y;
    }

    void Renderer::Impl::pushTransform(const core::DrawOp& op) {
        pushState();
        auto& current = state();
        if (op.useMatrix) {
            current.transform = current.transform.after(op.matrix);
        } else {
            current.transform = current.transform.after(Affine::layer(op.rect, op.transformOriginX, op.transformOriginY, op.scaleX, op.scaleY, op.rotationZ, op.translationX, op.translationY));
        }
        // 压扁成一条线或一个点的层什么都画不出来
        if (!inverted(current.transform) && !current.scissor.empty()) {
            flush();
            current.scissor = {};
        }
    }

    void Renderer::Impl::clipToCoverage(const std::function<void()>& draw) {
        flush();
        const int needW = std::max(targetWidth, 1);
        const int needH = std::max(targetHeight, 1);
        std::size_t index = 0;
        while (index < masks.size() && (masks[index].used || masks[index].width < needW || masks[index].height < needH)) ++index;
        if (index == masks.size()) {
            MaskTarget mask;
            mask.width = std::max(needW, maskWidth);
            mask.height = std::max(needH, maskHeight);
            D3D11_TEXTURE2D_DESC description{};
            description.Width = static_cast<UINT>(mask.width);
            description.Height = static_cast<UINT>(mask.height);
            description.MipLevels = 1;
            description.ArraySize = 1;
            description.Format = DXGI_FORMAT_R8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            check(device->CreateTexture2D(&description, nullptr, mask.texture.GetAddressOf()), "无法创建裁剪掩码");
            check(device->CreateRenderTargetView(mask.texture.Get(), nullptr, mask.target.GetAddressOf()), "无法创建裁剪掩码视图");
            check(device->CreateShaderResourceView(mask.texture.Get(), nullptr, mask.view.GetAddressOf()), "无法创建裁剪掩码视图");
            masks.push_back(std::move(mask));
        }
        masks[index].used = true;
        // 形状以白色画进清零的掩码：结果 = 形状覆盖率 × 已有的掩码与解析裁剪
        const float zero[4]{};
        auto* maskTarget = masks[index].target.Get();
        context->OMSetRenderTargets(1, &maskTarget, nullptr);
        context->ClearRenderTargetView(maskTarget, zero);
        context->OMSetBlendState(replace.Get(), nullptr, 0xffffffffu);
        draw();
        flush();
        bindQuadPipeline();
        auto& current = state();
        current.clip = {};
        current.mask = static_cast<int>(index);
        current.ownsMask = true;
    }

    void Renderer::Impl::clipToRect(Rect rect) {
        auto& current = state();
        const auto projected = current.transform.project(rect);
        if (!projected || !inverted(current.transform)) {
            if (!current.scissor.empty()) flush();
            current.scissor = {};
            return;
        }
        // 没有旋转时矩形裁剪就是剪刀矩形，四边取整到最近的像素；否则先收紧到外包盒，再按形状裁
        const auto aligned = axisAligned(current.transform);
        const auto scissor = intersect(current.scissor, pixelBounds(*projected, aligned));
        if (scissor != current.scissor) {
            flush();
            current.scissor = scissor;
        }
        if (aligned) return;
        core::DrawOp shape;
        shape.type = core::DrawOpType::PushClip;
        shape.rect = rect;
        pushClip(shape);
        // pushClip 自己压了一层状态；把它的裁剪并回本层，保持调用方的出入栈配对
        auto merged = states.back();
        states.pop_back();
        merged.ownsMask = merged.ownsMask || states.back().ownsMask;
        states.back() = merged;
    }

    void Renderer::Impl::pushClip(const core::DrawOp& op) {
        pushState();
        if (op.shape == core::DrawShapeType::Rectangle && axisAligned(state().transform)) {
            clipToRect(op.rect);
            return;
        }
        auto& current = state();
        const auto inverse = inverted(current.transform);
        const auto projected = current.transform.project(op.shape == core::DrawShapeType::Path && op.path ? Rect{op.rect.x, op.rect.y, op.path->bounds.width, op.path->bounds.height} : op.rect);
        const auto scissor = inverse && projected ? intersect(current.scissor, pixelBounds(*projected, false)) : PixelRect{};
        if (scissor != current.scissor) {
            flush();
            current.scissor = scissor;
        }
        if (scissor.empty()) return;
        const auto white = [&](core::DrawOp shape) {
            shape.color = 0xffffffffu;
            shape.gradient = {};
            shape.strokeWidth = 0;
            return shape;
        };
        if (op.shape == core::DrawShapeType::Path) {
            if (!op.path) return;
            clipToCoverage([&] {
                auto fill = white(op);
                fill.type = core::DrawOpType::PushClip;
                drawPath(fill, 1.0f);
            });
            return;
        }
        if (current.clip.kind == 0) {
            // 第一层圆角或椭圆裁剪在像素着色器里解析求值，不必另开掩码
            AnalyticClip clip;
            clip.kind = op.shape == core::DrawShapeType::Circle ? 2 : 1;
            clip.inverse = *inverse;
            setShape(clip.shape, op.rect);
            if (op.shape == core::DrawShapeType::Rounded) setRadii(clip.radii, op.corners, op.rect);
            flush();
            current.clip = clip;
            return;
        }
        clipToCoverage([&] { drawShape(white(op), op.rect, op.shape, op.corners, unbounded, 0.0f, 1.0f); });
    }

    void Renderer::Impl::replayScoped(std::span<const core::DrawOp> ops, float alpha) {
        if (ops.empty()) return;
        pushState();
        auto depth = states.size();
        replayOps(ops, alpha, depth);
        while (states.size() > depth) popState();
        popState();
    }

    void Renderer::Impl::replayFragment(const core::PlacedPaintFragment& placed, float alpha) {
        if (!placed.fragment) return;
        pushState();
        state().transform = state().transform.after(Affine::translation(placed.offset.x, placed.offset.y));
        const auto& fragment = *placed.fragment;
        if (!invisible(fragment.bounds)) {
            auto depth = states.size();
            if (fragment.layer && fragment.layer->drawCommands) {
                // 自定义绘制层：子片段只在内容标记处重放；录制 RenderLayer 时以满不透明度捕获
                for (const auto& op : fragment.layer->before) {
                    if (op.type == core::DrawOpType::ContentMarker) {
                        const float childAlpha = layerRecordStack.empty() ? alpha : 1.0f;
                        for (const auto& child : fragment.children) replayFragment(child, childAlpha);
                    } else
                        replayOps(std::span<const core::DrawOp>(&op, 1), alpha, depth);
                }
            } else if (fragment.layer && fragment.layer->useOffscreenGroup) {
                replayOffscreenGroup(fragment, alpha);
            } else {
                if (const auto* effect = fragment.layer ? std::get_if<core::EffectModifier>(&fragment.layer->value) : nullptr) replayEffect(*effect, fragment.layer->bounds, alpha);
                if (fragment.layer) replayOps(fragment.layer->before, alpha, depth);
                const auto contentAlpha = alpha * (fragment.layer ? fragment.layer->contentAlpha : 1.0f);
                if (fragment.content) replayOps(*fragment.content, contentAlpha, depth);
                for (const auto& child : fragment.children) replayFragment(child, contentAlpha);
                if (fragment.layer) replayOps(fragment.layer->after, alpha, depth);
            }
            while (states.size() > depth) popState();
        }
        popState();
    }

    void Renderer::Impl::replayOps(std::span<const core::DrawOp> ops, float alpha, std::size_t& depth) {
        using core::BlendMode;
        using core::DrawOpType;
        const auto isCoverageSafeHardwareBlend = [](BlendMode mode) noexcept {
            return mode == BlendMode::DstOver || mode == BlendMode::DstOut || mode == BlendMode::SrcAtop || mode == BlendMode::Xor || mode == BlendMode::Plus || mode == BlendMode::Screen;
        };

        for (const auto& op : ops) {
            const auto stateOp = op.type == DrawOpType::PushClip || op.type == DrawOpType::PopClip || op.type == DrawOpType::PushTransform || op.type == DrawOpType::PopTransform || op.type == DrawOpType::BeginRecordLayer || op.type == DrawOpType::EndRecordLayer;
            if (!stateOp && (state().scissor.empty() || invisible(core::drawOpBounds(op)))) continue;

            const bool isDrawPrimitive = op.type == DrawOpType::FillRect || op.type == DrawOpType::StrokeRect || op.type == DrawOpType::DrawLine || op.type == DrawOpType::DrawPath || op.type == DrawOpType::DrawText || op.type == DrawOpType::DrawPainter;
            if (isDrawPrimitive) {
                if (op.blendMode == BlendMode::Dst) continue;
                if (op.shader) {
                    drawOpWithShaderBrush(op, alpha);
                    continue;
                }
                if (op.colorFilter || (op.blendMode != BlendMode::SrcOver && op.blendMode != BlendMode::Clear && !isCoverageSafeHardwareBlend(op.blendMode))) {
                    drawOpIsolated(op, alpha);
                    continue;
                }
            }

            const bool customHardwareBlend = isDrawPrimitive && (op.blendMode == BlendMode::Clear || isCoverageSafeHardwareBlend(op.blendMode));
            if (customHardwareBlend) {
                flush();
                auto* bs = op.blendMode == BlendMode::Clear ? clearPunchOutBlend.Get() : hardwareBlends[static_cast<std::size_t>(op.blendMode)].Get();
                context->OMSetBlendState(bs, nullptr, 0xffffffffu);
            }

            const core::DrawOp* activeOp = &op;
            core::DrawOp clearMaskOp;
            float activeAlpha = alpha;
            if (customHardwareBlend && op.blendMode == BlendMode::Clear) {
                clearMaskOp = op;
                clearMaskOp.color = 0xffffffffu;
                clearMaskOp.gradient = {};
                activeOp = &clearMaskOp;
                activeAlpha = 1.0f;
            }

            switch (activeOp->type) {
                case DrawOpType::ContentMarker:
                    break;
                case DrawOpType::FillRect:
                    drawShape(*activeOp, activeOp->rect, activeOp->shape, activeOp->corners, unbounded, 0.0f, activeAlpha);
                    break;
                case DrawOpType::StrokeRect: {
                    // 对齐 Compose：border 的描边落在轮廓内侧，Canvas 的形状描边以轮廓为中线。
                    // 直角矩形的中线描边按外扩半个线宽的矩形向内描，外角保持尖角（尖角线接）
                    const auto half = activeOp->strokeWidth * 0.5f;
                    const auto inward = [&](Rect rect) {
                        drawShape(*activeOp, rect, activeOp->shape, activeOp->corners, -std::min(activeOp->strokeWidth, std::min(rect.width, rect.height) * 0.5f), 0.0f, activeAlpha);
                    };
                    if (activeOp->strokeInside)
                        inward(activeOp->rect);
                    else if (activeOp->shape == core::DrawShapeType::Rectangle)
                        inward(expanded(activeOp->rect, half));
                    else
                        drawShape(*activeOp, activeOp->rect, activeOp->shape, activeOp->corners, -half, half, activeAlpha);
                    break;
                }
                case DrawOpType::DrawLine:
                    drawLine(*activeOp, activeAlpha);
                    break;
                case DrawOpType::DrawPath:
                    drawPath(*activeOp, activeAlpha);
                    break;
                case DrawOpType::DrawText:
                    if (activeOp->inputText && focused.valid() && activeOp->textField == focused) {
                        // 焦点输入框的文字、选区和光标由输入叠层在原位重放
                        pushState();
                        state().transform = state().transform.after(Affine::translation(activeOp->rect.x, activeOp->rect.y));
                        const auto saved = std::exchange(focused, {});
                        replayScoped(focusedOps, activeAlpha * activeOp->inputAlpha);
                        focused = saved;
                        popState();
                    } else
                        drawText(*activeOp, activeAlpha);
                    break;
                case DrawOpType::DrawPainter:
                    drawPainter(*activeOp, activeAlpha);
                    break;
                case DrawOpType::DrawNative:
                    // drawNative 的后端实现属后续里程碑：不绘制，每个名字记一条诊断
                    if (activeOp->native && reportedNatives.insert(activeOp->native->name).second) core::Log::w(TAG, "渲染器尚不支持 drawNative，已跳过", activeOp->native->name);
                    break;
                case DrawOpType::PushClip:
                    pushClip(*activeOp);
                    break;
                case DrawOpType::PushTransform:
                    pushTransform(*activeOp);
                    break;
                case DrawOpType::PopClip:
                case DrawOpType::PopTransform:
                    if (states.size() > depth) popState();
                    break;
                case DrawOpType::BeginRecordLayer:
                    beginRecordLayer(*activeOp, depth);
                    break;
                case DrawOpType::EndRecordLayer:
                    endRecordLayer(depth);
                    break;
                case DrawOpType::DrawLayer:
                    drawRenderLayer(*activeOp, alpha);
                    break;
            }

            if (customHardwareBlend) {
                flush();
                context->OMSetBlendState(blend.Get(), nullptr, 0xffffffffu);
            }
        }
    }

    void Renderer::Impl::replayEffect(const core::EffectModifier& effect, Rect rect, float alpha) {
        if ((effect.color >> 24) == 0 || !drawable(rect)) return;
        const auto& transform = state().transform;
        // 标准差过小时模糊积分不稳定，按近乎清晰处理
        const auto sigma = std::max(core::effectSigma(effect), 0.25f);
        const auto ellipse = effect.shape.shapeType == core::ShapeKind::Circle;
        const auto grown = [&](float grow, core::Point offset, float* shape, float* radii) {
            const Rect result{rect.x - grow + offset.x, rect.y - grow + offset.y, rect.width + 2 * grow, rect.height + 2 * grow};
            setShape(shape, result);
            if (effect.shape.shapeType == core::ShapeKind::Rounded && result.width > 0 && result.height > 0) {
                auto corners = effect.shape.corners;
                for (auto* corner : {&corners.topStart, &corners.topEnd, &corners.bottomEnd, &corners.bottomStart}) *corner = std::max(0.0f, *corner + grow);
                setRadii(radii, corners, result);
            }
            return result;
        };
        const auto margin = pixelInLocal(transform);
        if (effect.kind == core::EffectKind::DropShadow) {
            auto instance = instanceFor(QuadKind::DropShadow, transform);
            const auto shape = grown(effect.spread, effect.offset, instance.shape, instance.radii);
            if (!drawable(shape)) return;
            setRect(instance.rect, expanded(shape, std::ceil(sigma * 3.0f) + margin));
            instance.params[2] = sigma, instance.params[3] = ellipse ? 1.0f : 0.0f;
            setColor(instance.color, effect.color, alpha);
            add(instance);
            return;
        }
        // 内阴影：阴影色铺满外形，挖去按扩展收缩、按偏移移动后的形状
        auto instance = instanceFor(QuadKind::InnerShadow, transform);
        grown(0, {}, instance.shape, instance.radii);
        grown(-effect.spread, effect.offset, instance.shape2, instance.radii2);
        setRect(instance.rect, expanded(rect, margin));
        instance.params[2] = sigma, instance.params[3] = ellipse ? 1.0f : 0.0f;
        setColor(instance.color, effect.color, alpha);
        add(instance);
    }

    void Renderer::Impl::drawShape(const core::DrawOp& op, Rect rect, core::DrawShapeType shape, const core::CornerRadii& corners, float low, float high, float alpha) {
        if (!drawable(rect)) return;
        const auto& transform = state().transform;
        auto instance = instanceFor(shape == core::DrawShapeType::Circle ? QuadKind::Ellipse : QuadKind::RoundedRect, transform);
        setRect(instance.rect, expanded(rect, std::max(high, 0.0f) + pixelInLocal(transform)));
        setShape(instance.shape, rect);
        if (shape == core::DrawShapeType::Rounded) setRadii(instance.radii, corners, rect);
        instance.params[0] = low, instance.params[1] = high;
        // 渐变按图形自身的矩形取比例，中线描边外扩出去的部分沿用同一套坐标
        setBrush(instance, op.color, alpha, op.gradient, op.rect);
        add(instance);
    }

    void Renderer::Impl::drawLine(const core::DrawOp& op, float alpha) {
        const core::Point from{op.rect.x, op.rect.y}, to = op.lineEnd;
        const auto width = op.strokeWidth > 0 ? op.strokeWidth : 1.0f;
        const auto length = std::hypot(to.x - from.x, to.y - from.y);
        if (!std::isfinite(length) || (length == 0 && op.cap == core::StrokeCap::Butt)) return;
        // 线段在以起点为原点、沿线方向为 x 轴的坐标系里是一个矩形；圆头就是圆角
        const auto angle = length > 0 ? std::atan2(to.y - from.y, to.x - from.x) : 0.0f;
        const Affine frame{std::cos(angle), std::sin(angle), -std::sin(angle), std::cos(angle), from.x, from.y};
        const auto transform = state().transform.after(frame);
        const auto half = width * 0.5f, extend = op.cap == core::StrokeCap::Butt ? 0.0f : half;
        const Rect body{-extend, -half, length + 2 * extend, width};
        auto instance = instanceFor(QuadKind::RoundedRect, transform);
        setRect(instance.rect, expanded(body, pixelInLocal(transform)));
        setShape(instance.shape, body);
        if (op.cap == core::StrokeCap::Round) setRadii(instance.radii, core::CornerRadii::uniform(half), body);
        instance.params[0] = unbounded;
        // 线段的渐变参照两端点的外包盒
        setBrush(instance, op.color, alpha, op.gradient, {std::min(from.x, to.x), std::min(from.y, to.y), std::abs(to.x - from.x), std::abs(to.y - from.y)}, &frame);
        add(instance);
    }

    void Renderer::Impl::drawCoverage(const CoverageEntry& entry, int deviceX, int deviceY, QuadInstance instance) {
        if (entry.width <= 0 || entry.height <= 0) return;
        // 覆盖率已在设备空间栅格化，按整数像素一对一贴上去
        setRect(instance.rect, {static_cast<float>(deviceX + entry.originX), static_cast<float>(deviceY + entry.originY), static_cast<float>(entry.width), static_cast<float>(entry.height)});
        const auto w = static_cast<float>(entry.textureWidth), h = static_cast<float>(entry.textureHeight);
        instance.uv[0] = static_cast<float>(entry.x) / w, instance.uv[1] = static_cast<float>(entry.y) / h;
        instance.uv[2] = static_cast<float>(entry.x + entry.width) / w, instance.uv[3] = static_cast<float>(entry.y + entry.height) / h;
        add(instance, nullptr, entry.dedicated ? entry.dedicated.Get() : atlasView.Get());
    }

    void Renderer::Impl::drawPath(const core::DrawOp& op, float alpha) {
        if (!op.path) return;
        const auto transform = state().transform;
        const auto inverse = inverted(transform);
        if (!inverse) return;
        const auto originX = static_cast<int>(std::floor(transform.tx)), originY = static_cast<int>(std::floor(transform.ty));
        auto local = transform;
        local.tx -= originX, local.ty -= originY;
        const auto entry = pathCoverage(op, local, {-originX, -originY, targetWidth - originX, targetHeight - originY});
        if (!entry) return;
        auto instance = instanceFor(QuadKind::Coverage, Affine{});
        setBrush(instance, op.color, alpha, op.gradient, op.rect, &*inverse);
        drawCoverage(*entry, originX, originY, instance);
    }

    void Renderer::Impl::drawText(const core::DrawOp& op, float alpha) {
        if (!op.textLayout) return;
        const auto* resource = dynamic_cast<const DWriteTextResource*>(op.textLayout->resource.get());
        if (!resource) {
            if (reportedNatives.insert("\x01text").second) core::Log::w(TAG, "文字不是 DirectWrite 排版的结果，已跳过");
            return;
        }
        const auto clipped = op.inputText || (op.overflow != core::TextOverflow::Visible && core::textHasVisualOverflow(op));
        if (clipped) {
            pushState();
            clipToRect(op.rect);
        }
        const auto transform = state().transform;
        const auto scissor = state().scissor;
        auto instance = instanceFor(QuadKind::Coverage, Affine{});
        setColor(instance.color, op.color, alpha);
        auto linear = transform;
        linear.tx = linear.ty = 0;
        // 伪斜体：字形绕自己的基线原点错切
        const auto sheared = linear.after(Affine{1, 0, -core::syntheticItalicSkew, 1, 0, 0});
        for (const auto& run : resource->runs) {
            if (scissor.empty()) break;
            const auto lineWidth = run.line < op.textLayout->lines.size() ? op.textLayout->lines[run.line].width : 0.0f;
            const auto x = op.rect.x + core::textAlignShift(op.textAlign, op.rect.width, lineWidth), y = op.rect.y;
            const auto reach = static_cast<int>(run.emSize * std::max(areaScale(transform), 1.0f) * 2.0f) + 2;
            for (std::size_t i = 0; i < run.glyphs.size(); ++i) {
                const auto px = x + run.positions[i].x, py = y + run.positions[i].y;
                // 子像素位置量化到四分之一像素，同一字形最多栅格化十六种相位
                const auto deviceX = std::round((transform.a * px + transform.c * py + transform.tx) * 4.0) / 4.0;
                const auto deviceY = std::round((transform.b * px + transform.d * py + transform.ty) * 4.0) / 4.0;
                if (!std::isfinite(deviceX) || !std::isfinite(deviceY) || std::abs(deviceX) > 1.0e7 || std::abs(deviceY) > 1.0e7) continue;
                const auto wholeX = static_cast<int>(std::floor(deviceX)), wholeY = static_cast<int>(std::floor(deviceY));
                if (wholeX + reach < scissor.left || wholeX - reach > scissor.right || wholeY + reach < scissor.top || wholeY - reach > scissor.bottom) continue;
                const auto* entry = glyphCoverage(run, i, run.syntheticItalic ? sheared : linear, static_cast<float>(deviceX - wholeX), static_cast<float>(deviceY - wholeY));
                if (entry) drawCoverage(*entry, wholeX, wholeY, instance);
            }
        }
        if (clipped) popState();
    }

    void Renderer::Impl::drawImage(const ImageTexture& texture, Rect source, Rect destination, Rect visible, const core::DrawOp& op, float alpha, bool antialias) {
        if (!drawable(destination) || !drawable(visible) || !drawable(source) || texture.width <= 0 || texture.height <= 0) return;
        const auto& transform = state().transform;
        auto instance = instanceFor(QuadKind::Image, transform);
        const auto quad = antialias ? expanded(visible, pixelInLocal(transform)) : visible;
        setRect(instance.rect, quad);
        setShape(instance.shape, antialias ? visible : expanded(visible, 1.0e6f));
        const auto w = static_cast<float>(texture.width), h = static_cast<float>(texture.height);
        const auto u = [&](float x) {
            return (source.x + (x - destination.x) / destination.width * source.width) / w;
        };
        const auto v = [&](float y) {
            return (source.y + (y - destination.y) / destination.height * source.height) / h;
        };
        instance.uv[0] = u(quad.x), instance.uv[1] = v(quad.y), instance.uv[2] = u(quad.x + quad.width), instance.uv[3] = v(quad.y + quad.height);
        instance.radii2[0] = source.x / w, instance.radii2[1] = source.y / h, instance.radii2[2] = (source.x + source.width) / w, instance.radii2[3] = (source.y + source.height) / h;
        if (op.hasTint) {
            // 着色：图片只提供 Alpha，颜色取 tint
            instance.xform1[3] = 1.0f;
            setColor(instance.color, op.color, alpha);
        } else
            std::fill(std::begin(instance.color), std::end(instance.color), static_cast<float>(op.color >> 24) / 255.0f * std::clamp(alpha, 0.0f, 1.0f));
        add(instance, texture.view.Get());
    }

    void Renderer::Impl::drawPainter(const core::DrawOp& op, float alpha) {
        const auto rect = op.rect;
        const auto& content = op.painter.content;
        if (!content || !drawable(rect)) return;
        const auto* bitmap = imageTexture(content);
        const auto isVector = !bitmap && dynamic_cast<const core::VectorContent*>(content.get()) != nullptr;
        if (!bitmap && !isVector) {
            if (reportedNatives.insert("\x01painter").second) core::Log::w(TAG, "Painter 内容不是位图、像素缓冲或矢量图，已跳过");
            return;
        }
        // 逻辑尺寸取固有尺寸；filmstrip 只取当前帧
        const auto whole = content->intrinsicSize.value_or(bitmap ? core::Size{static_cast<float>(bitmap->width), static_cast<float>(bitmap->height)} : core::Size{});
        if (!(whole.width > 0) || !(whole.height > 0)) return;
        const auto frame = core::painterFrameRect(op.painter, whole);
        if (!drawable(frame)) return;
        const auto scale = std::max(areaScale(state().transform), 0.01f);
        // 位图取帧对应的像素区域；矢量图按落到屏幕上的像素尺寸栅格化当前帧
        const auto sourceOf = [&](float logicalWidth, float logicalHeight, const ImageTexture*& texture) -> Rect {
            if (bitmap) {
                texture = bitmap;
                const auto sx = static_cast<float>(bitmap->width) / whole.width, sy = static_cast<float>(bitmap->height) / whole.height;
                return {frame.x * sx, frame.y * sy, frame.width * sx, frame.height * sy};
            }
            const auto width = std::clamp(static_cast<int>(std::ceil(logicalWidth * scale)), 1, 4096), height = std::clamp(static_cast<int>(std::ceil(logicalHeight * scale)), 1, 4096);
            texture = vectorTexture(content, frame, width, height);
            return {0, 0, static_cast<float>(width), static_cast<float>(height)};
        };
        const ImageTexture* texture = nullptr;
        if (op.repeat == core::ImageRepeat::None) {
            auto factorX = rect.width / frame.width, factorY = rect.height / frame.height;
            switch (op.contentScale) {
                case core::ContentScale::FillBounds:
                    break;
                case core::ContentScale::Fit:
                    factorX = factorY = std::min(factorX, factorY);
                    break;
                case core::ContentScale::Crop:
                    factorX = factorY = std::max(factorX, factorY);
                    break;
                case core::ContentScale::Inside:
                    factorX = factorY = std::min(1.0f, std::min(factorX, factorY));
                    break;
                case core::ContentScale::Unscaled:
                    factorX = factorY = 1.0f;
                    break;
                case core::ContentScale::FillWidth:
                    factorY = factorX;
                    break;
                case core::ContentScale::FillHeight:
                    factorX = factorY;
                    break;
            }
            const auto width = frame.width * factorX, height = frame.height * factorY;
            const Rect destination{rect.x + (rect.width - width) * core::horizontalBias(op.alignment), rect.y + (rect.height - height) * core::verticalBias(op.alignment), width, height};
            const auto source = sourceOf(width, height, texture);
            if (texture) drawImage(*texture, source, destination, core::intersectRect(destination, rect), op, alpha, true);
            return;
        }
        // 平铺与九宫格以固有尺寸为逻辑单位
        const auto source = sourceOf(frame.width, frame.height, texture);
        if (!texture) return;
        if (op.repeat == core::ImageRepeat::Tile) {
            const auto& transform = state().transform;
            auto instance = instanceFor(QuadKind::Image, transform);
            setRect(instance.rect, expanded(rect, pixelInLocal(transform)));
            setShape(instance.shape, rect);
            const auto w = static_cast<float>(texture->width), h = static_cast<float>(texture->height);
            instance.radii2[0] = source.x / w, instance.radii2[1] = source.y / h, instance.radii2[2] = (source.x + source.width) / w, instance.radii2[3] = (source.y + source.height) / h;
            instance.shape2[0] = rect.x, instance.shape2[1] = rect.y, instance.shape2[2] = frame.width, instance.shape2[3] = frame.height;
            instance.xform1[3] = op.hasTint ? 3.0f : 2.0f;
            if (op.hasTint)
                setColor(instance.color, op.color, alpha);
            else
                std::fill(std::begin(instance.color), std::end(instance.color), static_cast<float>(op.color >> 24) / 255.0f * std::clamp(alpha, 0.0f, 1.0f));
            add(instance, texture->view.Get());
            return;
        }
        // 九宫格：四角保持逻辑尺寸，放不下时等比缩小；边与中心拉伸
        const auto& s = op.slices;
        const auto fit = std::min({1.0f, s.left + s.right > 0 ? rect.width / (s.left + s.right) : 1.0f, s.top + s.bottom > 0 ? rect.height / (s.top + s.bottom) : 1.0f});
        const auto unitX = source.width / frame.width, unitY = source.height / frame.height;
        const float sourceX[]{source.x, source.x + s.left * unitX, source.x + (frame.width - s.right) * unitX, source.x + source.width};
        const float sourceY[]{source.y, source.y + s.top * unitY, source.y + (frame.height - s.bottom) * unitY, source.y + source.height};
        const float targetX[]{rect.x, rect.x + s.left * fit, rect.x + rect.width - s.right * fit, rect.x + rect.width};
        const float targetY[]{rect.y, rect.y + s.top * fit, rect.y + rect.height - s.bottom * fit, rect.y + rect.height};
        for (int row = 0; row < 3; ++row)
            for (int column = 0; column < 3; ++column) {
                const Rect from{sourceX[column], sourceY[row], sourceX[column + 1] - sourceX[column], sourceY[row + 1] - sourceY[row]};
                const Rect to{targetX[column], targetY[row], targetX[column + 1] - targetX[column], targetY[row + 1] - targetY[row]};
                // 相邻宫格共边，不各自留抗锯齿边，免得接缝处重复混合
                drawImage(*texture, from, to, to, op, alpha, false);
            }
    }
}
