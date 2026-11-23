#include <core/GraphicsEffects.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace canton::core {
    std::optional<BlendMode> parseBlendMode(std::string_view name) noexcept {
        constexpr std::pair<std::string_view, BlendMode> table[]{
            {"Clear", BlendMode::Clear},
            {"Src", BlendMode::Src},
            {"Dst", BlendMode::Dst},
            {"SrcOver", BlendMode::SrcOver},
            {"DstOver", BlendMode::DstOver},
            {"SrcIn", BlendMode::SrcIn},
            {"DstIn", BlendMode::DstIn},
            {"SrcOut", BlendMode::SrcOut},
            {"DstOut", BlendMode::DstOut},
            {"SrcAtop", BlendMode::SrcAtop},
            {"DstAtop", BlendMode::DstAtop},
            {"Xor", BlendMode::Xor},
            {"Plus", BlendMode::Plus},
            {"Modulate", BlendMode::Modulate},
            {"Screen", BlendMode::Screen},
            {"Overlay", BlendMode::Overlay},
            {"Darken", BlendMode::Darken},
            {"Lighten", BlendMode::Lighten},
            {"ColorDodge", BlendMode::ColorDodge},
            {"ColorBurn", BlendMode::ColorBurn},
            {"HardLight", BlendMode::HardLight},
            {"SoftLight", BlendMode::SoftLight},
            {"Difference", BlendMode::Difference},
            {"Exclusion", BlendMode::Exclusion},
            {"Multiply", BlendMode::Multiply},
            {"Hue", BlendMode::Hue},
            {"Saturation", BlendMode::Saturation},
            {"Color", BlendMode::Color},
            {"Luminosity", BlendMode::Luminosity},
        };
        for (const auto& [key, value] : table) {
            if (key == name) return value;
        }
        return std::nullopt;
    }

    std::optional<TileMode> parseTileMode(std::string_view name) noexcept {
        if (name == "Clamp") return TileMode::Clamp;
        if (name == "Repeated") return TileMode::Repeated;
        if (name == "Mirror") return TileMode::Mirror;
        if (name == "Decal") return TileMode::Decal;
        return std::nullopt;
    }

    std::optional<CompositingStrategy> parseCompositingStrategy(std::string_view name) noexcept {
        if (name == "Auto") return CompositingStrategy::Auto;
        if (name == "Offscreen") return CompositingStrategy::Offscreen;
        if (name == "ModulateAlpha") return CompositingStrategy::ModulateAlpha;
        return std::nullopt;
    }

    RuntimeEffectHandle RuntimeEffectRegistry::registerProgram(RuntimeEffectProgram program) {
        std::lock_guard lock(mutex_);
        const auto id = nextIdentity_++;
        const auto gen = nextGeneration_++;
        program.identity = id;
        program.revision = 1;
        entries_.emplace(id, Entry{gen, std::make_shared<const RuntimeEffectProgram>(std::move(program))});
        return {id, gen};
    }

    void RuntimeEffectRegistry::updateProgram(RuntimeEffectHandle handle, RuntimeEffectProgram nextProgram) {
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(handle.identity);
        if (it == entries_.end() || it->second.generation != handle.generation) {
            throw std::invalid_argument("RuntimeEffect 句柄无效或已释放");
        }
        nextProgram.identity = handle.identity;
        nextProgram.revision = it->second.program ? (it->second.program->revision + 1) : 1;
        it->second.program = std::make_shared<const RuntimeEffectProgram>(std::move(nextProgram));
    }

    RuntimeEffectProgramRef RuntimeEffectRegistry::resolve(RuntimeEffectHandle handle) const {
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(handle.identity);
        if (it == entries_.end() || it->second.generation != handle.generation) return {};
        return it->second.program;
    }

    void RuntimeEffectRegistry::release(RuntimeEffectHandle handle) {
        std::lock_guard lock(mutex_);
        const auto it = entries_.find(handle.identity);
        if (it != entries_.end() && it->second.generation == handle.generation) entries_.erase(it);
    }

    void RuntimeEffectRegistry::clear() {
        std::lock_guard lock(mutex_);
        entries_.clear();
    }

    namespace {
        void writeBytes(std::vector<std::uint8_t>& buffer, std::uint32_t offset, const void* src, std::size_t size) {
            if (offset + size > buffer.size()) {
                throw std::out_of_range("着色器 uniform 写入越界");
            }
            std::memcpy(buffer.data() + offset, src, size);
        }

        std::uint32_t align16(std::uint32_t size) noexcept {
            return (size + 15u) & ~15u;
        }
    }

    std::vector<PackedConstantBuffer> packRuntimeShaderUniforms(const ShaderReflection& reflection, std::span<const ShaderUniformInput> uniforms) {
        std::vector<PackedConstantBuffer> result;
        result.reserve(reflection.buffers.size());
        std::unordered_map<std::uint32_t, std::size_t> slotToIndex;
        for (const auto& desc : reflection.buffers) {
            if (desc.slot > 11u) continue;
            slotToIndex[desc.slot] = result.size();
            const auto padded = std::max<std::uint32_t>(16u, align16(desc.byteSize));
            result.push_back({desc.slot, std::vector<std::uint8_t>(padded, 0)});
        }

        std::unordered_map<std::string_view, const ShaderUniformField*> fieldMap;
        fieldMap.reserve(reflection.uniforms.size());
        for (const auto& field : reflection.uniforms) {
            fieldMap.emplace(field.name, &field);
        }

        for (const auto& input : uniforms) {
            const auto it = fieldMap.find(input.name);
            if (it == fieldMap.end()) continue;
            const auto& field = *it->second;
            const auto bufIt = slotToIndex.find(field.bufferSlot);
            if (bufIt == slotToIndex.end()) continue;
            auto& bytes = result[bufIt->second].bytes;
            const std::uint32_t count = std::max<std::uint32_t>(1u, field.elements);
            const std::uint32_t componentsPerElement = field.rows * field.columns;
            const std::uint32_t expectedTotal = count * componentsPerElement;

            if (input.kind == ShaderUniformInputKind::Color) {
                if (field.scalarType != ShaderScalarType::Float || field.rows != 1 || field.columns != 4 || field.elements != 0) {
                    throw std::invalid_argument("setColor('" + input.name + "') 目标字段必须是 HLSL float4 类型");
                }
                const float a = static_cast<float>((input.color >> 24u) & 0xffu) / 255.0f;
                const float rgba[4]{
                    static_cast<float>((input.color >> 16u) & 0xffu) / 255.0f * a,
                    static_cast<float>((input.color >> 8u) & 0xffu) / 255.0f * a,
                    static_cast<float>(input.color & 0xffu) / 255.0f * a,
                    a,
                };
                writeBytes(bytes, field.startOffset, rgba, sizeof(rgba));
                continue;
            }

            if (input.kind == ShaderUniformInputKind::Float) {
                if (field.scalarType != ShaderScalarType::Float) {
                    throw std::invalid_argument("setFloat('" + input.name + "') 与着色器字段标量类型不符（需要整型请改用 setInt）");
                }
                if (input.floatValues.size() != expectedTotal) {
                    throw std::invalid_argument("setFloat('" + input.name + "') 分量数不符：期望 " + std::to_string(expectedTotal) + "，实际 " + std::to_string(input.floatValues.size()));
                }
                if (field.rows == 1) {
                    const std::uint32_t elemBytes = field.columns * 4u;
                    const std::uint32_t stride = align16(elemBytes);
                    for (std::uint32_t e = 0; e < count; ++e) {
                        writeBytes(bytes, field.startOffset + e * stride, input.floatValues.data() + e * field.columns, elemBytes);
                    }
                } else {
                    // 矩阵按行列布局写入：HLSL 默认列主序（每列占 16 字节步长），row_major 按每行 16 字节步长
                    const std::uint32_t majorCount = field.rowMajor ? field.rows : field.columns;
                    const std::uint32_t minorCount = field.rowMajor ? field.columns : field.rows;
                    const std::uint32_t elemStride = majorCount * 16u;
                    for (std::uint32_t e = 0; e < count; ++e) {
                        const float* base = input.floatValues.data() + e * componentsPerElement;
                        const std::uint32_t elemOffset = field.startOffset + e * elemStride;
                        for (std::uint32_t m = 0; m < majorCount; ++m) {
                            float vec[4]{};
                            for (std::uint32_t n = 0; n < minorCount; ++n) {
                                const std::uint32_t r = field.rowMajor ? m : n;
                                const std::uint32_t c = field.rowMajor ? n : m;
                                vec[n] = base[r * field.columns + c];
                            }
                            writeBytes(bytes, elemOffset + m * 16u, vec, minorCount * 4u);
                        }
                    }
                }
                continue;
            }

            // Int / UInt / Bool
            if (field.scalarType == ShaderScalarType::Float) {
                throw std::invalid_argument("setInt('" + input.name + "') 与着色器字段标量类型不符（浮点字段请改用 setFloat）");
            }
            if (input.intValues.size() != expectedTotal) {
                throw std::invalid_argument("setInt('" + input.name + "') 分量数不符：期望 " + std::to_string(expectedTotal) + "，实际 " + std::to_string(input.intValues.size()));
            }
            const std::uint32_t elemBytes = field.columns * 4u;
            const std::uint32_t stride = align16(elemBytes);
            for (std::uint32_t e = 0; e < count; ++e) {
                for (std::uint32_t r = 0; r < field.rows; ++r) {
                    std::uint32_t words[4]{};
                    for (std::uint32_t c = 0; c < field.columns; ++c) {
                        const auto val = input.intValues[e * componentsPerElement + r * field.columns + c];
                        words[c] = field.scalarType == ShaderScalarType::Bool ? (val != 0 ? 1u : 0u) : static_cast<std::uint32_t>(val);
                    }
                    const std::uint32_t dstOffset = field.startOffset + e * (field.rows > 1 ? field.rows * 16u : stride) + r * 16u;
                    writeBytes(bytes, dstOffset, words, elemBytes);
                }
            }
        }
        return result;
    }

    Rect renderEffectOutputBounds(const RenderEffectRef& effect, Rect bounds) noexcept {
        if (!effect) return bounds;
        for (const auto& step : effect->steps) {
            if (step.kind == RenderEffectStepKind::Blur) {
                const float padX = step.radiusX > 0.0f ? std::ceil(step.radiusX * 1.5f) : 0.0f;
                const float padY = step.radiusY > 0.0f ? std::ceil(step.radiusY * 1.5f) : 0.0f;
                bounds.x -= padX;
                bounds.y -= padY;
                bounds.width += padX * 2.0f;
                bounds.height += padY * 2.0f;
            } else if (step.kind == RenderEffectStepKind::Offset) {
                const float left = std::min(bounds.x, bounds.x + step.offsetX);
                const float top = std::min(bounds.y, bounds.y + step.offsetY);
                const float right = std::max(bounds.x + bounds.width, bounds.x + bounds.width + step.offsetX);
                const float bottom = std::max(bounds.y + bounds.height, bounds.y + bounds.height + step.offsetY);
                bounds = {left, top, right - left, bottom - top};
            }
        }
        return bounds;
    }

    RenderLayerHandle RenderLayerRegistry::create() {
        std::lock_guard lock(mutex_);
        const auto id = nextIdentity_++;
        const auto gen = nextGeneration_++;
        active_.emplace(id, gen);
        return {id, gen};
    }

    bool RenderLayerRegistry::alive(RenderLayerHandle handle) const {
        std::lock_guard lock(mutex_);
        const auto it = active_.find(handle.identity);
        return it != active_.end() && it->second == handle.generation;
    }

    void RenderLayerRegistry::release(RenderLayerHandle handle) {
        std::lock_guard lock(mutex_);
        const auto it = active_.find(handle.identity);
        if (it != active_.end() && it->second == handle.generation) active_.erase(it);
    }

    void RenderLayerRegistry::clear() {
        std::lock_guard lock(mutex_);
        active_.clear();
    }
}
