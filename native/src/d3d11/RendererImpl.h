#pragma once

#include <d3d11/Com.h>
#include <d3d11/DWriteText.h>
#include <d3d11/PathRaster.h>
#include <d3d11/Renderer.h>

#include <core/Affine.h>
#include <core/Log.h>

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>

#include <array>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string_view>
#include <unordered_map>

namespace canton::d3d11 {
    // 一个实例一张四边形，字段含义见 shaders/Quad.hlsl
    struct QuadInstance {
        float rect[4]{};
        // a、b、c、d；tx、ty、种类、标志
        float xform0[4]{1, 0, 0, 1};
        float xform1[4]{};
        // 形状中心与半尺寸
        float shape[4]{};
        float radii[4]{};
        // 覆盖范围的距离下限与上限、模糊标准差、是否椭圆
        float params[4]{};
        float color[4]{};
        // 笔刷种类、渐变查找表的行
        float brush[4]{};
        float brushGeometry[4]{};
        float brushRow0[4]{1, 0, 0, 0};
        float brushRow1[4]{0, 1, 0, 0};
        float uv[4]{};
        float shape2[4]{};
        float radii2[4]{};
    };

    enum class QuadKind { RoundedRect = 0, Ellipse = 1, Image = 2, Coverage = 3, DropShadow = 4, InnerShadow = 5 };

    struct FrameConstants {
        float viewport[2]{};
        float padding[2]{};
        float clipRow0[4]{};
        float clipRow1[4]{};
        float clipShape[4]{};
        float clipRadii[4]{};
        float clipFlags[4]{};
    };

    // 圆角矩形或椭圆裁剪，在像素着色器里解析求覆盖率
    struct AnalyticClip {
        int kind = 0;
        core::Affine inverse;
        float shape[4]{};
        float radii[4]{};
    };

    struct RenderState {
        core::Affine transform;
        PixelRect scissor;
        AnalyticClip clip;
        // 裁剪掩码在掩码池里的下标，-1 表示没有
        int mask = -1;
        // 掩码由这层状态申请，出栈时归还掩码池
        bool ownsMask = false;
    };

    struct MaskTarget {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> target;
        ComPtr<ID3D11ShaderResourceView> view;
        bool used = false;
    };

    // 图集里或独立纹理上的一块覆盖率，origin 是它相对取整后设备原点的位置
    struct CoverageEntry {
        ComPtr<ID3D11ShaderResourceView> dedicated;
        int x = 0, y = 0, width = 0, height = 0;
        int originX = 0, originY = 0;
        int textureWidth = 0, textureHeight = 0;
    };

    struct GlyphKey {
        IDWriteFontFace* face = nullptr;
        std::uint16_t glyph = 0;
        std::uint8_t subX = 0, subY = 0;
        bool bold = false;
        float emSize = 0;
        float linear[4]{};
        bool operator==(const GlyphKey&) const = default;
    };

    struct PathKey {
        const core::PathData* path = nullptr;
        float values[8]{};
        float strokeWidth = 0;
        std::uint8_t cap = 0, join = 0;
        bool operator==(const PathKey&) const = default;
    };

    inline void hashCombine(std::size_t& seed, std::size_t value) noexcept {
        seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
    }

    struct GlyphKeyHash {
        std::size_t operator()(const GlyphKey& key) const noexcept {
            auto seed = std::hash<const void*>()(key.face);
            hashCombine(seed, key.glyph | (key.subX << 16) | (key.subY << 20) | (key.bold ? 1u << 24 : 0u));
            hashCombine(seed, std::hash<float>()(key.emSize));
            for (const auto value : key.linear) hashCombine(seed, std::hash<float>()(value));
            return seed;
        }
    };

    struct PathKeyHash {
        std::size_t operator()(const PathKey& key) const noexcept {
            auto seed = std::hash<const void*>()(key.path);
            for (const auto value : key.values) hashCombine(seed, std::hash<float>()(value));
            hashCombine(seed, std::hash<float>()(key.strokeWidth));
            hashCombine(seed, key.cap | (key.join << 8));
            return seed;
        }
    };

    struct ImageTexture {
        std::weak_ptr<const core::PainterContent> owner;
        ComPtr<ID3D11ShaderResourceView> view;
        int width = 0, height = 0;
        std::uint64_t lastUsedFrame = 0;
    };

    struct VectorKey {
        const core::PainterContent* content = nullptr;
        int width = 0, height = 0;
        float source[4]{};
        bool operator==(const VectorKey&) const = default;
    };

    struct VectorKeyHash {
        std::size_t operator()(const VectorKey& key) const noexcept {
            auto seed = std::hash<const void*>()(key.content);
            hashCombine(seed, static_cast<std::size_t>(key.width) * 8191u + static_cast<std::size_t>(key.height));
            for (const auto value : key.source) hashCombine(seed, std::hash<float>()(value));
            return seed;
        }
    };

    class TargetImpl;

    struct Renderer::Impl {
        static constexpr std::string_view TAG = "D3D11Renderer";
        static constexpr int atlasSize = 2048;
        // 超过这个边长的覆盖率不进图集，单独成纹理
        static constexpr int atlasItemLimit = 512;
        static constexpr int gradientRows = 256;
        static constexpr int gradientWidth = 256;
        static constexpr std::size_t instanceCapacity = 4096;

        explicit Impl(RendererOptions options);

        // —— 设备与随设备重建的资源 ——
        RendererOptions options;
        AdapterInfo adapterInfo;
        ComPtr<IDXGIFactory2> factory;
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        ComPtr<ID3D11VertexShader> vertexShader;
        ComPtr<ID3D11PixelShader> pixelShader;
        ComPtr<ID3D11InputLayout> inputLayout;
        ComPtr<ID3D11Buffer> instanceBuffer;
        ComPtr<ID3D11Buffer> constantBuffer;
        ComPtr<ID3D11BlendState> blend;
        ComPtr<ID3D11BlendState> replace;
        ComPtr<ID3D11RasterizerState> rasterizer;
        ComPtr<ID3D11SamplerState> sampler;
        ComPtr<ID3D11Texture2D> atlasTexture;
        ComPtr<ID3D11ShaderResourceView> atlasView;
        ComPtr<ID3D11Texture2D> gradientTexture;
        ComPtr<ID3D11ShaderResourceView> gradientView;
        ComPtr<ID3D11ShaderResourceView> whiteView;
        std::vector<MaskTarget> masks;
        int maskWidth = 0, maskHeight = 0;
        // 设备每重建一次加一，呈现目标据此重建自己的缓冲
        std::uint64_t deviceGeneration = 0;

        void createDevice();
        void createDeviceResources();
        void releaseDeviceResources();
        void rebuildDevice();
        [[nodiscard]] bool deviceLost() const;
        ComPtr<ID3D11ShaderResourceView> createTexture(int width, int height, DXGI_FORMAT format, const void* pixels, UINT pitch, ComPtr<ID3D11Texture2D>* texture = nullptr);

        // —— 缓存 ——
        struct Shelf {
            int y = 0, height = 0, used = 0;
        };

        std::vector<Shelf> shelves;
        int atlasBottom = 0;
        // 缓存里独立纹理占用的字节数，超出预算时与图集一起清空
        std::size_t dedicatedBytes = 0;
        std::unordered_map<GlyphKey, std::pair<ComPtr<IDWriteFontFace>, CoverageEntry>, GlyphKeyHash> glyphs;
        std::unordered_map<PathKey, std::pair<core::PathRef, CoverageEntry>, PathKeyHash> paths;
        std::unordered_map<const core::Gradient*, std::pair<core::GradientRef, int>> gradients;
        std::unordered_map<const core::PainterContent*, ImageTexture> images;
        std::unordered_map<VectorKey, ImageTexture, VectorKeyHash> vectors;
        // 本帧临时用到的独立纹理，帧末释放
        std::vector<ComPtr<ID3D11ShaderResourceView>> transient;
        std::uint64_t frameNumber = 0;
        std::set<std::string> reportedNatives;
        RenderCounters counters;

        void resetCaches();
        void resetAtlas();
        std::optional<CoverageEntry> storeCoverage(const CoverageMask& mask, int originX, int originY, bool cacheable);
        const CoverageEntry* glyphCoverage(const GlyphRun& run, std::size_t index, const core::Affine& linear, float fractionX, float fractionY);
        std::optional<CoverageEntry> pathCoverage(const core::DrawOp& op, const core::Affine& transform, const PixelRect& clip);
        int gradientRow(const core::GradientRef& gradient);
        const ImageTexture* imageTexture(const std::shared_ptr<const core::PainterContent>& content);
        const ImageTexture* vectorTexture(const std::shared_ptr<const core::PainterContent>& content, core::Rect source, int width, int height);
        void trimCaches();

        // —— 一帧的绘制状态 ——
        TargetImpl* target = nullptr;
        ID3D11RenderTargetView* targetView = nullptr;
        int targetWidth = 0, targetHeight = 0;
        std::vector<RenderState> states;
        std::vector<QuadInstance> pending;
        ID3D11ShaderResourceView* batchImage = nullptr;
        ID3D11ShaderResourceView* batchCoverage = nullptr;
        core::ModifierHandle focused;
        std::span<const core::DrawOp> focusedOps;

        [[nodiscard]] RenderState& state() {
            return states.back();
        }

        void ensureMasks();
        void drawFrame(TargetImpl& output, const FrameInput& frame);
        void flush();
        void add(const QuadInstance& instance, ID3D11ShaderResourceView* image = nullptr, ID3D11ShaderResourceView* coverage = nullptr);
        void pushState();
        void popState();

        void replayFragment(const core::PlacedPaintFragment& placed, float alpha);
        void replayOps(std::span<const core::DrawOp> ops, float alpha, std::size_t& depth);
        void replayScoped(std::span<const core::DrawOp> ops, float alpha);
        void replayEffect(const core::EffectModifier& effect, core::Rect rect, float alpha);
        [[nodiscard]] bool invisible(const core::PaintBounds& bounds) const;

        void pushTransform(const core::DrawOp& op);
        void pushClip(const core::DrawOp& op);
        void clipToRect(core::Rect rect);
        // 把形状画进新的裁剪掩码：与当前掩码和解析裁剪相乘
        void clipToCoverage(const std::function<void()>& draw);

        [[nodiscard]] QuadInstance instanceFor(QuadKind kind, const core::Affine& transform) const;
        void setBrush(QuadInstance& instance, std::uint32_t color, float alpha, const core::GradientRef& gradient, core::Rect box, const core::Affine* toLocal = nullptr);
        void drawShape(const core::DrawOp& op, core::Rect rect, core::DrawShapeType shape, const core::CornerRadii& corners, float low, float high, float alpha);
        void drawLine(const core::DrawOp& op, float alpha);
        void drawPath(const core::DrawOp& op, float alpha);
        void drawCoverage(const CoverageEntry& entry, int deviceX, int deviceY, QuadInstance instance);
        void drawText(const core::DrawOp& op, float alpha);
        void drawPainter(const core::DrawOp& op, float alpha);
        void drawImage(const ImageTexture& texture, core::Rect source, core::Rect destination, core::Rect visible, const core::DrawOp& op, float alpha, bool antialias);
    };

    class TargetImpl final : public RenderTarget {
       public:
        [[nodiscard]] int width() const noexcept override {
            return width_;
        }

        [[nodiscard]] int height() const noexcept override {
            return height_;
        }

        Renderer::Impl* owner = nullptr;
        HWND window = nullptr;
        int width_ = 0, height_ = 0;
        // 缓冲属于哪一代设备、多大；不符时重建
        std::uint64_t generation = 0;
        int bufferWidth = 0, bufferHeight = 0;
        ComPtr<IDXGISwapChain1> swapChain;
        ComPtr<IDCompositionDevice> composition;
        ComPtr<IDCompositionTarget> compositionTarget;
        ComPtr<IDCompositionVisual> visual;
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> view;
        ComPtr<ID3D11Texture2D> staging;

        void ensure();
        void release();
    };

    [[nodiscard]] inline bool isDeviceLost(HRESULT code) noexcept {
        return code == DXGI_ERROR_DEVICE_REMOVED || code == DXGI_ERROR_DEVICE_RESET || code == DXGI_ERROR_DEVICE_HUNG || code == DXGI_ERROR_DRIVER_INTERNAL_ERROR;
    }
}
