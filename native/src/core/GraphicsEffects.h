#pragma once

#include "Brush.h"
#include "Geometry.h"
#include "Painter.h"

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace canton::core {
    // 严格对齐 Compose 29 种 BlendMode（0..14 走硬件 ID3D11BlendState，15..28 走局部拷出 + 混合像素着色器）
    enum class BlendMode : std::uint8_t {
        Clear = 0,
        Src = 1,
        Dst = 2,
        SrcOver = 3,
        DstOver = 4,
        SrcIn = 5,
        DstIn = 6,
        SrcOut = 7,
        DstOut = 8,
        SrcAtop = 9,
        DstAtop = 10,
        Xor = 11,
        Plus = 12,
        Modulate = 13,
        Screen = 14,
        Overlay = 15,
        Darken = 16,
        Lighten = 17,
        ColorDodge = 18,
        ColorBurn = 19,
        HardLight = 20,
        SoftLight = 21,
        Difference = 22,
        Exclusion = 23,
        Multiply = 24,
        Hue = 25,
        Saturation = 26,
        Color = 27,
        Luminosity = 28,
    };

    constexpr bool isHardwareBlendMode(BlendMode mode) noexcept {
        return static_cast<std::uint8_t>(mode) <= static_cast<std::uint8_t>(BlendMode::Screen);
    }

    std::optional<BlendMode> parseBlendMode(std::string_view name) noexcept;

    enum class TileMode : std::uint8_t {
        Clamp = 0,
        Repeated = 1,
        Mirror = 2,
        Decal = 3,
    };

    std::optional<TileMode> parseTileMode(std::string_view name) noexcept;

    enum class CompositingStrategy : std::uint8_t {
        Auto = 0,
        Offscreen = 1,
        ModulateAlpha = 2,
    };

    std::optional<CompositingStrategy> parseCompositingStrategy(std::string_view name) noexcept;

    enum class ColorFilterKind : std::uint8_t {
        None = 0,
        Tint = 1,
        ColorMatrix = 2,
        Lighting = 3,
    };

    // 色彩矩阵第 5 列平移偏置（下标 4, 9, 14, 19）对齐 Compose 使用 [0..255] 量纲
    struct ColorFilter {
        ColorFilterKind kind = ColorFilterKind::None;
        std::uint32_t color = 0;
        BlendMode blendMode = BlendMode::SrcIn;
        std::array<float, 20> matrix{
            1, 0, 0, 0, 0,
            0, 1, 0, 0, 0,
            0, 0, 1, 0, 0,
            0, 0, 0, 1, 0,
        };
        std::uint32_t multiply = 0xffffffffu;
        std::uint32_t add = 0;

        [[nodiscard]] explicit operator bool() const noexcept {
            return kind != ColorFilterKind::None;
        }

        bool operator==(const ColorFilter&) const = default;
    };

    enum class ShaderScalarType : std::uint8_t {
        Float = 0,
        Int = 1,
        UInt = 2,
        Bool = 3,
    };

    struct ShaderUniformField {
        std::string name;
        std::uint32_t bufferSlot = 0;
        std::uint32_t startOffset = 0;
        std::uint32_t size = 0;
        ShaderScalarType scalarType = ShaderScalarType::Float;
        std::uint32_t rows = 1;
        std::uint32_t columns = 1;
        std::uint32_t elements = 0;
        bool rowMajor = false;
        bool operator==(const ShaderUniformField&) const = default;
    };

    struct ShaderBufferDesc {
        std::uint32_t slot = 0;
        std::uint32_t byteSize = 0;
        bool operator==(const ShaderBufferDesc&) const = default;
    };

    struct ShaderResourceSlot {
        std::string name;
        std::uint32_t bindPoint = 0;
        bool operator==(const ShaderResourceSlot&) const = default;
    };

    struct ShaderReflection {
        std::vector<ShaderBufferDesc> buffers;
        std::vector<ShaderUniformField> uniforms;
        std::vector<ShaderResourceSlot> textures;
        std::vector<ShaderResourceSlot> samplers;
        bool operator==(const ShaderReflection&) const = default;
    };

    struct ShaderCompileOutput {
        std::vector<std::uint8_t> dxbc;
        ShaderReflection reflection;
    };

    // 抽象着色器编译与反射接口：core 不依赖 D3D11，由 d3d11 实现并经 host/测试装配进 runtime
    class ShaderCompiler {
       public:
        virtual ~ShaderCompiler() = default;
        [[nodiscard]] virtual ShaderCompileOutput compileHlsl(std::string_view source, std::string_view entryPoint, std::string_view debugName) const = 0;
        [[nodiscard]] virtual ShaderReflection reflectDxbc(std::span<const std::uint8_t> dxbc, std::string_view debugName) const = 0;
    };

    // 不可变单版编译结果；热替换时产出新 revision 实例，失败时保留旧实例
    struct RuntimeEffectProgram {
        std::uint64_t identity = 0;
        std::uint64_t revision = 1;
        std::string debugName;
        std::string entryPoint;
        std::vector<std::uint8_t> dxbc;
        ShaderReflection reflection;
    };

    using RuntimeEffectProgramRef = std::shared_ptr<const RuntimeEffectProgram>;

    struct RuntimeEffectHandle {
        std::uint64_t identity = 0;
        std::uint64_t generation = 0;

        [[nodiscard]] bool valid() const noexcept {
            return identity != 0 && generation != 0;
        }

        bool operator==(const RuntimeEffectHandle&) const = default;
    };

    // 每个宿主/运行时实例独立持有的 RuntimeEffect 注册表（非全局单例）
    class RuntimeEffectRegistry {
       public:
        RuntimeEffectHandle registerProgram(RuntimeEffectProgram program);
        void updateProgram(RuntimeEffectHandle handle, RuntimeEffectProgram nextProgram);
        [[nodiscard]] RuntimeEffectProgramRef resolve(RuntimeEffectHandle handle) const;
        void release(RuntimeEffectHandle handle);
        void clear();

       private:
        struct Entry {
            std::uint64_t generation = 0;
            RuntimeEffectProgramRef program;
        };

        mutable std::mutex mutex_;
        std::uint64_t nextIdentity_ = 1;
        std::uint64_t nextGeneration_ = 1;
        std::unordered_map<std::uint64_t, Entry> entries_;
    };

    enum class ShaderUniformInputKind : std::uint8_t {
        Float = 0,
        Int = 1,
        Color = 2,
    };

    struct ShaderUniformInput {
        std::string name;
        ShaderUniformInputKind kind = ShaderUniformInputKind::Float;
        std::vector<float> floatValues;
        std::vector<std::int32_t> intValues;
        std::uint32_t color = 0;
        bool operator==(const ShaderUniformInput&) const = default;
    };

    struct ShaderTextureInput {
        std::string name;
        PainterSnapshot painter;
        TileMode tileModeX = TileMode::Clamp;
        TileMode tileModeY = TileMode::Clamp;
        bool nearest = false;
        bool operator==(const ShaderTextureInput&) const = default;
    };

    struct PackedConstantBuffer {
        std::uint32_t slot = 0;
        std::vector<std::uint8_t> bytes;
        bool operator==(const PackedConstantBuffer&) const = default;
    };

    // 按 D3DReflect 布局校验字段类型/分量数并封包各 cbuffer 槽位（含 16 字节数组与矩阵对齐、预乘色转换）
    [[nodiscard]] std::vector<PackedConstantBuffer> packRuntimeShaderUniforms(const ShaderReflection& reflection, std::span<const ShaderUniformInput> uniforms);

    struct RuntimeShaderData {
        RuntimeEffectProgramRef program;
        std::uint64_t version = 0;
        std::vector<PackedConstantBuffer> packedBuffers;
        std::vector<ShaderTextureInput> textures;

        bool operator==(const RuntimeShaderData& other) const {
            return program == other.program && version == other.version && packedBuffers == other.packedBuffers && textures == other.textures;
        }
    };

    using RuntimeShaderRef = SharedValue<RuntimeShaderData>;

    enum class RenderEffectStepKind : std::uint8_t {
        Blur = 0,
        Offset = 1,
        ColorFilter = 2,
        Shader = 3,
    };

    struct RenderEffectStep {
        RenderEffectStepKind kind = RenderEffectStepKind::Blur;
        float radiusX = 0.0f;
        float radiusY = 0.0f;
        TileMode edgeTreatment = TileMode::Clamp;
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        ColorFilter colorFilter;
        RuntimeShaderRef shader;
        std::string uniformShaderName;
        bool operator==(const RenderEffectStep&) const = default;
    };

    struct RenderEffectData {
        std::vector<RenderEffectStep> steps;
        bool operator==(const RenderEffectData&) const = default;
    };

    using RenderEffectRef = SharedValue<RenderEffectData>;

    // 计算 RenderEffect 链对绘制边界的外扩（模糊与偏移）
    [[nodiscard]] Rect renderEffectOutputBounds(const RenderEffectRef& effect, Rect bounds) noexcept;

    struct RenderLayerHandle {
        std::uint64_t identity = 0;
        std::uint64_t generation = 0;

        [[nodiscard]] bool valid() const noexcept {
            return identity != 0 && generation != 0;
        }

        bool operator==(const RenderLayerHandle&) const = default;
    };

    struct RenderLayerShape {
        std::uint8_t type = 0;  // 0 Rectangle, 1 Rounded, 2 Circle
        CornerRadii corners;
        bool operator==(const RenderLayerShape&) const = default;
    };

    struct RenderLayerSnapshot {
        RenderLayerHandle handle;
        std::uint64_t version = 0;
        float alpha = 1.0f;
        BlendMode blendMode = BlendMode::SrcOver;
        ColorFilter colorFilter;
        RenderEffectRef renderEffect;
        bool clip = false;
        RenderLayerShape shape;
        float translationX = 0.0f;
        float translationY = 0.0f;
        float scaleX = 1.0f;
        float scaleY = 1.0f;
        float rotationZ = 0.0f;
        float originX = 0.5f;
        float originY = 0.5f;
        float topLeftX = 0.0f;
        float topLeftY = 0.0f;
        CompositingStrategy compositingStrategy = CompositingStrategy::Auto;
        bool operator==(const RenderLayerSnapshot&) const = default;
    };

    using RenderLayerSnapshotRef = SharedValue<RenderLayerSnapshot>;

    class RenderLayerRegistry {
       public:
        RenderLayerHandle create();
        [[nodiscard]] bool alive(RenderLayerHandle handle) const;
        void release(RenderLayerHandle handle);
        void clear();

       private:
        mutable std::mutex mutex_;
        std::uint64_t nextIdentity_ = 1;
        std::uint64_t nextGeneration_ = 1;
        std::unordered_map<std::uint64_t, std::uint64_t> active_;
    };
}
