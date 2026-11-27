#include <d3d11/RendererImpl.h>

#include <core/Font.h>

#include <d2d1.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace canton::d3d11 {
    namespace {
        constexpr std::size_t dedicatedBudget = 64 * 1024 * 1024;
        constexpr PixelRect everywhere{-(1 << 24), -(1 << 24), 1 << 24, 1 << 24};

        // 接收 DirectWrite 字形轮廓，展平成折线
        class OutlineSink final : public ID2D1SimplifiedGeometrySink {
           public:
            explicit OutlineSink(float scale) : scale_(scale) {}

            std::vector<Polyline> lines;

            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** object) override {
                if (id == __uuidof(IUnknown) || id == __uuidof(ID2D1SimplifiedGeometrySink)) {
                    *object = this;
                    return S_OK;
                }
                *object = nullptr;
                return E_NOINTERFACE;
            }

            ULONG STDMETHODCALLTYPE AddRef() override {
                return 1;
            }

            ULONG STDMETHODCALLTYPE Release() override {
                return 1;
            }

            void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE) override {}

            void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT) override {}

            void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F start, D2D1_FIGURE_BEGIN) override {
                path_.verbs.push_back(core::PathVerb::MoveTo);
                path_.values.insert(path_.values.end(), {start.x, start.y});
            }

            void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F* points, UINT32 count) override {
                for (UINT32 i = 0; i < count; ++i) {
                    path_.verbs.push_back(core::PathVerb::LineTo);
                    path_.values.insert(path_.values.end(), {points[i].x, points[i].y});
                }
            }

            void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT* beziers, UINT32 count) override {
                for (UINT32 i = 0; i < count; ++i) {
                    path_.verbs.push_back(core::PathVerb::CubicTo);
                    path_.values.insert(path_.values.end(), {beziers[i].point1.x, beziers[i].point1.y, beziers[i].point2.x, beziers[i].point2.y, beziers[i].point3.x, beziers[i].point3.y});
                }
            }

            void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END) override {
                path_.verbs.push_back(core::PathVerb::Close);
            }

            HRESULT STDMETHODCALLTYPE Close() override {
                lines = flattenPath(path_, {}, scale_);
                return S_OK;
            }

           private:
            float scale_;
            core::PathData path_;
        };

        float linearScale(const core::Affine& t) {
            return static_cast<float>(std::sqrt(std::abs(t.a * t.d - t.b * t.c)));
        }

        // 两块覆盖率取并集：逐像素取较大者
        CoverageMask unite(const CoverageMask& a, const CoverageMask& b) {
            if (a.bounds.empty()) return b;
            if (b.bounds.empty()) return a;
            CoverageMask result;
            result.bounds = {std::min(a.bounds.left, b.bounds.left), std::min(a.bounds.top, b.bounds.top), std::max(a.bounds.right, b.bounds.right), std::max(a.bounds.bottom, b.bounds.bottom)};
            const auto width = static_cast<std::size_t>(result.bounds.width());
            result.pixels.assign(width * static_cast<std::size_t>(result.bounds.height()), 0);
            for (const auto* mask : {&a, &b})
                for (int y = 0; y < mask->bounds.height(); ++y)
                    for (int x = 0; x < mask->bounds.width(); ++x) {
                        auto& out = result.pixels[static_cast<std::size_t>(y + mask->bounds.top - result.bounds.top) * width + static_cast<std::size_t>(x + mask->bounds.left - result.bounds.left)];
                        out = std::max(out, mask->pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(mask->bounds.width()) + static_cast<std::size_t>(x)]);
                    }
            return result;
        }

        PixelRect boundsOf(const std::vector<Polyline>& polygons) {
            float minX = 0, minY = 0, maxX = 0, maxY = 0;
            bool any = false;
            for (const auto& polygon : polygons)
                for (const auto point : polygon.points) {
                    if (!std::isfinite(point.x) || !std::isfinite(point.y)) return {};
                    minX = any ? std::min(minX, point.x) : point.x;
                    minY = any ? std::min(minY, point.y) : point.y;
                    maxX = any ? std::max(maxX, point.x) : point.x;
                    maxY = any ? std::max(maxY, point.y) : point.y;
                    any = true;
                }
            if (!any) return {};
            const auto bound = [](float value) {
                return static_cast<int>(std::clamp(value, -1.0e7f, 1.0e7f));
            };
            return {bound(std::floor(minX)), bound(std::floor(minY)), bound(std::ceil(maxX)), bound(std::ceil(maxY))};
        }
    }

    void Renderer::Impl::resetCaches() {
        shelves.clear();
        atlasBottom = 0;
        dedicatedBytes = 0;
        glyphs.clear();
        paths.clear();
        gradients.clear();
        images.clear();
        vectors.clear();
        transient.clear();
    }

    void Renderer::Impl::resetAtlas() {
        // 已入批的实例还指着图集里的旧位置，先画掉
        flush();
        shelves.clear();
        atlasBottom = 0;
        dedicatedBytes = 0;
        glyphs.clear();
        paths.clear();
        ++counters.atlasResets;
    }

    std::optional<CoverageEntry> Renderer::Impl::storeCoverage(const CoverageMask& mask, int originX, int originY, bool cacheable) {
        CoverageEntry entry;
        entry.width = mask.bounds.width();
        entry.height = mask.bounds.height();
        entry.originX = originX;
        entry.originY = originY;
        if (mask.bounds.empty()) return entry;
        if (!cacheable || entry.width > atlasItemLimit || entry.height > atlasItemLimit || entry.width + 1 > maxAtlasSize || entry.height + 1 > maxAtlasSize) {
            entry.dedicated = createTexture(entry.width, entry.height, DXGI_FORMAT_R8_UNORM, mask.pixels.data(), static_cast<UINT>(entry.width));
            entry.textureWidth = entry.width;
            entry.textureHeight = entry.height;
            if (cacheable)
                dedicatedBytes += static_cast<std::size_t>(entry.width) * static_cast<std::size_t>(entry.height);
            else
                transient.push_back(entry.dedicated);
            return entry;
        }
        // 每块右下留一像素空隙，线性采样不会串到邻块
        const auto width = entry.width + 1, height = entry.height + 1;
        for (int attempt = 0; attempt < 4; ++attempt) {
            Shelf* chosen = nullptr;
            for (auto& shelf : shelves)
                if (shelf.height >= height && shelf.height <= height + height / 2 && shelf.used + width <= atlasSize && (!chosen || shelf.height < chosen->height)) chosen = &shelf;
            if (!chosen && atlasBottom + height <= atlasSize) {
                shelves.push_back({atlasBottom, height, 0});
                atlasBottom += height;
                chosen = &shelves.back();
            }
            if (!chosen) {
                if (atlasSize < maxAtlasSize) {
                    // WARP 无损内存瘦身：图集从 512×512 起步，空间不足时倍增扩容至上限（最大 2048×2048）
                    flush();
                    batchCoverage = nullptr;
                    atlasSize = std::min(maxAtlasSize, std::max(atlasSize * 2, std::max(width, height)));
                    // 向上取 2 的幂（不超过 maxAtlasSize）
                    int pow2 = warpInitialAtlasSize;
                    while (pow2 < atlasSize && pow2 < maxAtlasSize) pow2 *= 2;
                    atlasSize = std::min(maxAtlasSize, pow2);
                    atlasView = createTexture(atlasSize, atlasSize, DXGI_FORMAT_R8_UNORM, nullptr, 0, &atlasTexture);
                    counters.atlasSize = atlasSize;
                    ++counters.atlasGrowths;
                    shelves.clear();
                    atlasBottom = 0;
                    dedicatedBytes = 0;
                    glyphs.clear();
                    paths.clear();
                    continue;
                }
                // 图集已达上限且满：整体回收，之后用到的字形和路径按需重新栅格化
                resetAtlas();
                continue;
            }
            entry.x = chosen->used;
            entry.y = chosen->y;
            chosen->used += width;
            entry.textureWidth = entry.textureHeight = atlasSize;
            const D3D11_BOX box{static_cast<UINT>(entry.x), static_cast<UINT>(entry.y), 0, static_cast<UINT>(entry.x + entry.width), static_cast<UINT>(entry.y + entry.height), 1};
            context->UpdateSubresource(atlasTexture.Get(), 0, &box, mask.pixels.data(), static_cast<UINT>(entry.width), 0);
            return entry;
        }
        return std::nullopt;
    }

    const CoverageEntry* Renderer::Impl::glyphCoverage(const GlyphRun& run, std::size_t index, const core::Affine& linear, float fractionX, float fractionY) {
        GlyphKey key;
        key.face = run.face.Get();
        key.glyph = run.glyphs[index];
        key.subX = static_cast<std::uint8_t>(fractionX * 4.0f + 0.5f);
        key.subY = static_cast<std::uint8_t>(fractionY * 4.0f + 0.5f);
        key.bold = run.syntheticBold;
        key.emSize = run.emSize;
        key.linear[0] = static_cast<float>(linear.a), key.linear[1] = static_cast<float>(linear.b), key.linear[2] = static_cast<float>(linear.c), key.linear[3] = static_cast<float>(linear.d);
        if (const auto found = glyphs.find(key); found != glyphs.end()) return &found->second.second;

        CoverageMask mask;
        const auto offsetX = static_cast<float>(key.subX) * 0.25f, offsetY = static_cast<float>(key.subY) * 0.25f;
        if (run.syntheticBold) {
            // 伪粗体：字形轮廓填充后再沿轮廓描一圈，宽度取 core 的常量
            OutlineSink sink(linearScale(linear));
            const FLOAT advance = 0;
            const DWRITE_GLYPH_OFFSET offset{};
            // DirectWrite 不替调用方收尾，这里自己结束轮廓
            if (FAILED(run.face->GetGlyphRunOutline(run.emSize, &key.glyph, &advance, &offset, 1, FALSE, FALSE, &sink)) || FAILED(sink.Close())) return nullptr;
            auto placement = linear;
            placement.tx = offsetX, placement.ty = offsetY;
            auto fill = sink.lines;
            transformPolylines(fill, placement);
            auto stroke = strokePolylines(sink.lines, {core::syntheticBoldStroke * run.emSize, core::StrokeCap::Butt, core::StrokeJoin::Round}, linearScale(linear));
            transformPolylines(stroke, placement);
            mask = unite(rasterizePolygons(fill, false, everywhere), rasterizePolygons(stroke, false, everywhere));
        } else {
            const FLOAT advance = 0;
            const DWRITE_GLYPH_OFFSET offset{};
            const DWRITE_GLYPH_RUN glyph{run.face.Get(), run.emSize, 1, &key.glyph, &advance, &offset, FALSE, 0};
            const DWRITE_MATRIX matrix{key.linear[0], key.linear[1], key.linear[2], key.linear[3], offsetX, offsetY};
            ComPtr<IDWriteGlyphRunAnalysis> analysis;
            // 不做网格对齐：子像素位置由缓存键区分，硬件与 WARP 拿到同一份覆盖率
            if (FAILED(dwriteFactory()->CreateGlyphRunAnalysis(&glyph, &matrix, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, DWRITE_MEASURING_MODE_NATURAL, DWRITE_GRID_FIT_MODE_DISABLED, DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0, 0, analysis.GetAddressOf()))) return nullptr;
            RECT bounds{};
            if (FAILED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &bounds))) return nullptr;
            mask.bounds = {bounds.left, bounds.top, bounds.right, bounds.bottom};
            if (!mask.bounds.empty()) {
                mask.pixels.resize(static_cast<std::size_t>(mask.bounds.width()) * static_cast<std::size_t>(mask.bounds.height()));
                if (FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &bounds, mask.pixels.data(), static_cast<UINT32>(mask.pixels.size())))) return nullptr;
            }
        }
        ++counters.glyphsRasterized;
        const auto entry = storeCoverage(mask, mask.bounds.left, mask.bounds.top, true);
        if (!entry) return nullptr;
        return &glyphs.emplace(key, std::make_pair(run.face, *entry)).first->second.second;
    }

    std::optional<CoverageEntry> Renderer::Impl::pathCoverage(const core::DrawOp& op, const core::Affine& transform, const PixelRect& clip) {
        // transform 的平移已去掉整数部分；路径随整数像素移动（滚动）时复用同一块覆盖率
        const auto& data = *op.path;
        const auto dx = op.rect.x - data.bounds.x, dy = op.rect.y - data.bounds.y;
        const auto stroked = op.type == core::DrawOpType::DrawPath && op.strokeWidth > 0;
        PathKey key;
        key.path = op.path.value.get();
        const float values[8]{static_cast<float>(transform.a), static_cast<float>(transform.b), static_cast<float>(transform.c), static_cast<float>(transform.d), static_cast<float>(transform.tx), static_cast<float>(transform.ty), dx, dy};
        std::copy(std::begin(values), std::end(values), key.values);
        if (stroked) {
            key.strokeWidth = op.strokeWidth;
            key.cap = static_cast<std::uint8_t>(op.cap);
            key.join = static_cast<std::uint8_t>(op.join);
        }
        if (const auto found = paths.find(key); found != paths.end()) return found->second.second;

        const auto scale = linearScale(transform);
        auto polygons = flattenPath(data, {dx, dy}, scale);
        if (stroked) polygons = strokePolylines(polygons, {op.strokeWidth, op.cap, op.join}, scale);
        transformPolylines(polygons, transform);
        const auto full = boundsOf(polygons);
        const auto mask = rasterizePolygons(polygons, !stroked && data.evenOdd, clip);
        ++counters.pathsRasterized;
        // 被目标边界切掉一部分的覆盖率只对这一帧的位置成立，不进缓存
        const auto complete = mask.bounds == full;
        const auto entry = storeCoverage(mask, mask.bounds.left, mask.bounds.top, complete);
        if (entry && complete) paths.emplace(key, std::make_pair(op.path, *entry));
        return entry;
    }

    int Renderer::Impl::gradientRow(const core::GradientRef& gradient) {
        if (const auto found = gradients.find(gradient.value.get()); found != gradients.end()) return found->second.second;
        if (gradients.size() >= static_cast<std::size_t>(gradientRows)) {
            flush();
            gradients.clear();
        }
        const auto row = static_cast<int>(gradients.size());
        // 色标在非预乘空间插值，写入查找表前再预乘
        std::array<std::uint8_t, gradientWidth * 4> pixels{};
        const auto& stops = gradient->stops;
        for (int i = 0; i < gradientWidth && !stops.empty(); ++i) {
            const auto t = static_cast<float>(i) / static_cast<float>(gradientWidth - 1);
            std::size_t next = 0;
            while (next < stops.size() && stops[next].position <= t) ++next;
            float channel[4];
            const auto unpack = [](std::uint32_t color, float* out) {
                out[0] = static_cast<float>((color >> 16) & 0xff), out[1] = static_cast<float>((color >> 8) & 0xff), out[2] = static_cast<float>(color & 0xff), out[3] = static_cast<float>(color >> 24);
            };
            if (next == 0)
                unpack(stops.front().color, channel);
            else if (next == stops.size())
                unpack(stops.back().color, channel);
            else {
                float from[4], to[4];
                unpack(stops[next - 1].color, from);
                unpack(stops[next].color, to);
                const auto span = stops[next].position - stops[next - 1].position;
                const auto mix = span > 0 ? (t - stops[next - 1].position) / span : 1.0f;
                for (int c = 0; c < 4; ++c) channel[c] = from[c] + (to[c] - from[c]) * mix;
            }
            const auto alpha = channel[3] / 255.0f;
            for (int c = 0; c < 3; ++c) pixels[static_cast<std::size_t>(i * 4 + c)] = static_cast<std::uint8_t>(channel[c] * alpha + 0.5f);
            pixels[static_cast<std::size_t>(i * 4 + 3)] = static_cast<std::uint8_t>(channel[3] + 0.5f);
        }
        const D3D11_BOX box{0, static_cast<UINT>(row), 0, gradientWidth, static_cast<UINT>(row + 1), 1};
        context->UpdateSubresource(gradientTexture.Get(), 0, &box, pixels.data(), gradientWidth * 4, 0);
        gradients.emplace(gradient.value.get(), std::make_pair(gradient, row));
        return row;
    }

    const ImageTexture* Renderer::Impl::imageTexture(const std::shared_ptr<const core::PainterContent>& content) {
        if (const auto found = images.find(content.get()); found != images.end()) {
            // 地址可能被新内容复用，按所有权确认还是同一份
            if (found->second.owner.lock() == content) {
                found->second.lastUsedFrame = frameNumber;
                return &found->second;
            }
            flush();
            images.erase(found);
        }
        ImageTexture texture;
        texture.owner = content;
        texture.lastUsedFrame = frameNumber;
        if (const auto* bitmap = dynamic_cast<const core::BitmapContent*>(content.get())) {
            if (bitmap->width <= 0 || bitmap->height <= 0 || bitmap->pixels.size() < static_cast<std::size_t>(bitmap->width) * static_cast<std::size_t>(bitmap->height)) return nullptr;
            texture.width = bitmap->width, texture.height = bitmap->height;
            texture.view = createTexture(bitmap->width, bitmap->height, DXGI_FORMAT_B8G8R8A8_UNORM, bitmap->pixels.data(), static_cast<UINT>(bitmap->width * 4));
        } else if (const auto* buffer = dynamic_cast<const core::PixelBufferContent*>(content.get())) {
            if (buffer->width <= 0 || buffer->height <= 0 || buffer->pixels.size() < static_cast<std::size_t>(buffer->width) * static_cast<std::size_t>(buffer->height)) return nullptr;
            // 像素缓冲是非预乘 ARGB，上传前预乘；每次提交是新内容，同一版本只转换一次
            auto pixels = buffer->pixels;
            for (auto& pixel : pixels) {
                const auto alpha = pixel >> 24;
                const auto scale = [&](std::uint32_t channel) {
                    return (channel * alpha + 127) / 255;
                };
                pixel = (alpha << 24) | (scale((pixel >> 16) & 0xff) << 16) | (scale((pixel >> 8) & 0xff) << 8) | scale(pixel & 0xff);
            }
            texture.width = buffer->width, texture.height = buffer->height;
            texture.view = createTexture(buffer->width, buffer->height, DXGI_FORMAT_B8G8R8A8_UNORM, pixels.data(), static_cast<UINT>(buffer->width * 4));
        } else
            return nullptr;
        return &images.emplace(content.get(), std::move(texture)).first->second;
    }

    const ImageTexture* Renderer::Impl::vectorTexture(const std::shared_ptr<const core::PainterContent>& content, core::Rect source, int width, int height) {
        const auto* vector = dynamic_cast<const core::VectorContent*>(content.get());
        if (!vector || width <= 0 || height <= 0) return nullptr;
        const VectorKey key{content.get(), width, height, {source.x, source.y, source.width, source.height}};
        if (const auto found = vectors.find(key); found != vectors.end()) {
            if (found->second.owner.lock() == content) {
                found->second.lastUsedFrame = frameNumber;
                return &found->second;
            }
            flush();
            vectors.erase(found);
        }
        const auto pixels = vector->rasterize(source, width, height);
        if (pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height)) return nullptr;
        ImageTexture texture;
        texture.owner = content;
        texture.lastUsedFrame = frameNumber;
        texture.width = width, texture.height = height;
        texture.view = createTexture(width, height, DXGI_FORMAT_B8G8R8A8_UNORM, pixels.data(), static_cast<UINT>(width * 4));
        return &vectors.emplace(key, std::move(texture)).first->second;
    }

    void Renderer::Impl::trimCaches() {
        // 帧末调用：本帧的绘制都已提交
        transient.clear();
        std::erase_if(images, [](const auto& item) { return item.second.owner.expired(); });
        // 矢量图按像素尺寸各存一份，缩放动画会留下一串不再用的尺寸
        std::erase_if(vectors, [&](const auto& item) { return item.second.owner.expired() || item.second.lastUsedFrame + 120 < frameNumber; });
        // 离屏 RT 池空闲回收：硬件保留 60 帧，WARP 软渲染收紧至 2 帧以尽快归还内存
        const std::uint64_t rtIdleFrames = adapterInfo.software ? 2u : 60u;
        const std::size_t evicted = std::erase_if(offscreenPool, [&](const OffscreenTarget& rt) {
            return !rt.inUse && rt.lastUsedFrame + rtIdleFrames < frameNumber;
        });
        counters.offscreenTargetsEvicted += evicted;
        if (dedicatedBytes > dedicatedBudget) resetAtlas();
    }
}
