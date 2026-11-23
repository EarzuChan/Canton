#pragma once

#include "QuickJsPainterResources.h"
#include "QuickJsValueReader.h"
#include <core/GraphicsEffects.h>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace canton::quickjs {
    class QuickJsEffectsReader {
       public:
        QuickJsEffectsReader(JSContext* context, const canton::core::RuntimeEffectRegistry* effects = nullptr, const canton::core::RenderLayerRegistry* layers = nullptr)
            : context_(context), effects_(effects), layers_(layers) {}

        [[nodiscard]] canton::core::ColorFilter readColorFilter(JSValueConst value) const {
            using namespace canton::core;
            if (!JS_IsObject(value) || JS_IsArray(value)) throw std::invalid_argument("colorFilter 需要对象");
            ScopedValue typeVal(context_, JS_GetPropertyStr(context_, value, "type"));
            if (JS_IsUndefined(typeVal.get())) {
                // 兼容既有 M.paint({ colorFilter: { tint } })
                ScopedValue tintVal(context_, JS_GetPropertyStr(context_, value, "tint"));
                if (JS_IsUndefined(tintVal.get())) throw std::invalid_argument("colorFilter 缺少 type 或 tint 字段");
                ColorFilter filter;
                filter.kind = ColorFilterKind::Tint;
                filter.color = readColor(tintVal.get());
                filter.blendMode = BlendMode::SrcIn;
                return filter;
            }
            const auto type = readString(typeVal.get(), "colorFilter.type");
            ColorFilter filter;
            if (type == "tint") {
                filter.kind = ColorFilterKind::Tint;
                ScopedValue colorVal(context_, JS_GetPropertyStr(context_, value, "color"));
                filter.color = readColor(colorVal.get());
                ScopedValue blendVal(context_, JS_GetPropertyStr(context_, value, "blendMode"));
                if (!JS_IsUndefined(blendVal.get())) {
                    const auto name = readString(blendVal.get(), "colorFilter.blendMode");
                    const auto parsed = parseBlendMode(name);
                    if (!parsed) throw std::invalid_argument("colorFilter.blendMode 无效：" + name);
                    filter.blendMode = *parsed;
                }
                return filter;
            }
            if (type == "colorMatrix") {
                filter.kind = ColorFilterKind::ColorMatrix;
                ScopedValue matVal(context_, JS_GetPropertyStr(context_, value, "matrix"));
                if (!JS_IsArray(matVal.get())) throw std::invalid_argument("colorFilter.matrix 需要 20 元数组");
                const auto len = arrayLength(matVal.get());
                if (len < 20) throw std::invalid_argument("colorFilter.matrix 需要 20 个有限数值");
                for (std::uint32_t i = 0; i < 20; ++i) {
                    ScopedValue item(context_, JS_GetPropertyUint32(context_, matVal.get(), i));
                    filter.matrix[i] = readFinite(item.get(), "colorFilter.matrix");
                }
                return filter;
            }
            if (type == "lighting") {
                filter.kind = ColorFilterKind::Lighting;
                ScopedValue mulVal(context_, JS_GetPropertyStr(context_, value, "multiply"));
                ScopedValue addVal(context_, JS_GetPropertyStr(context_, value, "add"));
                filter.multiply = readColor(mulVal.get());
                filter.add = readColor(addVal.get());
                return filter;
            }
            throw std::invalid_argument("colorFilter.type 无效：" + type);
        }

        [[nodiscard]] canton::core::RuntimeEffectHandle readEffectHandle(JSValueConst value) const {
            if (!JS_IsObject(value) || JS_IsArray(value)) throw std::invalid_argument("RuntimeEffect 句柄无效");
            return {readHandleUint64(value, "identity"), readHandleUint64(value, "generation")};
        }

        [[nodiscard]] canton::core::RenderLayerHandle readLayerHandle(JSValueConst value) const {
            if (!JS_IsObject(value) || JS_IsArray(value)) throw std::invalid_argument("RenderLayer 句柄无效");
            canton::core::RenderLayerHandle handle{readHandleUint64(value, "identity"), readHandleUint64(value, "generation")};
            if (!handle.valid()) throw std::invalid_argument("RenderLayer 句柄无效");
            if (layers_ && !layers_->alive(handle)) throw std::invalid_argument("RenderLayer 已释放");
            return handle;
        }

        [[nodiscard]] canton::core::RuntimeShaderRef readRuntimeShader(JSValueConst value) const {
            using namespace canton::core;
            if (!JS_IsObject(value) || JS_IsArray(value)) throw std::invalid_argument("RuntimeShader 快照无效");
            if (!effects_) throw std::runtime_error("运行时缺少 RuntimeEffect 注册表");
            ScopedValue effectVal(context_, JS_GetPropertyStr(context_, value, "effect"));
            const auto handle = readEffectHandle(effectVal.get());
            const auto program = effects_->resolve(handle);
            if (!program) throw std::invalid_argument("RuntimeEffect 无效或已释放");

            RuntimeShaderData data;
            data.program = program;
            ScopedValue versionVal(context_, JS_GetPropertyStr(context_, value, "version"));
            data.version = readUint64(versionVal.get(), "RuntimeShader.version");

            std::vector<ShaderUniformInput> uniformInputs;
            ScopedValue uniformsVal(context_, JS_GetPropertyStr(context_, value, "uniforms"));
            if (JS_IsArray(uniformsVal.get())) {
                const auto count = arrayLength(uniformsVal.get());
                uniformInputs.reserve(count);
                for (std::uint32_t i = 0; i < count; ++i) {
                    ScopedValue item(context_, JS_GetPropertyUint32(context_, uniformsVal.get(), i));
                    if (!JS_IsObject(item.get())) throw std::invalid_argument("RuntimeShader uniform 条目无效");
                    ShaderUniformInput input;
                    ScopedValue nameVal(context_, JS_GetPropertyStr(context_, item.get(), "name"));
                    ScopedValue kindVal(context_, JS_GetPropertyStr(context_, item.get(), "kind"));
                    input.name = readString(nameVal.get(), "uniform.name");
                    const auto kind = readString(kindVal.get(), "uniform.kind");
                    if (kind == "float") {
                        input.kind = ShaderUniformInputKind::Float;
                        ScopedValue vals(context_, JS_GetPropertyStr(context_, item.get(), "values"));
                        if (!JS_IsArray(vals.get())) throw std::invalid_argument("uniform.values 需要数组");
                        const auto len = arrayLength(vals.get());
                        input.floatValues.reserve(len);
                        for (std::uint32_t j = 0; j < len; ++j) {
                            ScopedValue v(context_, JS_GetPropertyUint32(context_, vals.get(), j));
                            input.floatValues.push_back(readFinite(v.get(), "uniform.values"));
                        }
                    } else if (kind == "int") {
                        input.kind = ShaderUniformInputKind::Int;
                        ScopedValue vals(context_, JS_GetPropertyStr(context_, item.get(), "values"));
                        if (!JS_IsArray(vals.get())) throw std::invalid_argument("uniform.values 需要数组");
                        const auto len = arrayLength(vals.get());
                        input.intValues.reserve(len);
                        for (std::uint32_t j = 0; j < len; ++j) {
                            ScopedValue v(context_, JS_GetPropertyUint32(context_, vals.get(), j));
                            std::int32_t iv = 0;
                            if (!JS_IsNumber(v.get()) || JS_ToInt32(context_, &iv, v.get()) < 0) throw std::invalid_argument("uniform 整型值无效");
                            input.intValues.push_back(iv);
                        }
                    } else if (kind == "color") {
                        input.kind = ShaderUniformInputKind::Color;
                        ScopedValue col(context_, JS_GetPropertyStr(context_, item.get(), "color"));
                        input.color = readColor(col.get());
                    } else {
                        throw std::invalid_argument("uniform.kind 无效：" + kind);
                    }
                    uniformInputs.push_back(std::move(input));
                }
            }
            data.packedBuffers = packRuntimeShaderUniforms(program->reflection, uniformInputs);

            ScopedValue texturesVal(context_, JS_GetPropertyStr(context_, value, "textures"));
            if (JS_IsArray(texturesVal.get())) {
                const auto count = arrayLength(texturesVal.get());
                data.textures.reserve(count);
                for (std::uint32_t i = 0; i < count; ++i) {
                    ScopedValue item(context_, JS_GetPropertyUint32(context_, texturesVal.get(), i));
                    if (!JS_IsObject(item.get())) throw std::invalid_argument("RuntimeShader texture 条目无效");
                    ShaderTextureInput tex;
                    ScopedValue nameVal(context_, JS_GetPropertyStr(context_, item.get(), "name"));
                    tex.name = readString(nameVal.get(), "texture.name");
                    ScopedValue painterVal(context_, JS_GetPropertyStr(context_, item.get(), "painter"));
                    const auto snapshot = QuickJsPainterResources::read(context_, painterVal.get());
                    if (!snapshot) throw std::invalid_argument("RuntimeShader 绑定的 Painter 无效");
                    tex.painter = *snapshot;
                    ScopedValue txVal(context_, JS_GetPropertyStr(context_, item.get(), "tileModeX"));
                    ScopedValue tyVal(context_, JS_GetPropertyStr(context_, item.get(), "tileModeY"));
                    ScopedValue nearestVal(context_, JS_GetPropertyStr(context_, item.get(), "nearest"));
                    tex.tileModeX = static_cast<TileMode>(readIntRange(txVal.get(), 0, 3, "tileModeX"));
                    tex.tileModeY = static_cast<TileMode>(readIntRange(tyVal.get(), 0, 3, "tileModeY"));
                    tex.nearest = JS_ToBool(context_, nearestVal.get()) > 0;
                    data.textures.push_back(std::move(tex));
                }
            }
            return RuntimeShaderRef(std::move(data));
        }

        [[nodiscard]] canton::core::RenderEffectRef readRenderEffect(JSValueConst value) const {
            using namespace canton::core;
            if (JS_IsUndefined(value) || JS_IsNull(value)) return {};
            if (!JS_IsObject(value) || JS_IsArray(value)) throw std::invalid_argument("renderEffect 需要对象");
            ScopedValue stepsVal(context_, JS_GetPropertyStr(context_, value, "steps"));
            if (!JS_IsArray(stepsVal.get())) throw std::invalid_argument("renderEffect.steps 需要数组");
            const auto count = arrayLength(stepsVal.get());
            RenderEffectData data;
            data.steps.reserve(count);
            for (std::uint32_t i = 0; i < count; ++i) {
                ScopedValue item(context_, JS_GetPropertyUint32(context_, stepsVal.get(), i));
                if (!JS_IsObject(item.get())) throw std::invalid_argument("renderEffect step 无效");
                ScopedValue typeVal(context_, JS_GetPropertyStr(context_, item.get(), "type"));
                const auto type = readString(typeVal.get(), "renderEffect.step.type");
                RenderEffectStep step;
                if (type == "blur") {
                    step.kind = RenderEffectStepKind::Blur;
                    ScopedValue rx(context_, JS_GetPropertyStr(context_, item.get(), "radiusX"));
                    ScopedValue ry(context_, JS_GetPropertyStr(context_, item.get(), "radiusY"));
                    ScopedValue edge(context_, JS_GetPropertyStr(context_, item.get(), "edgeTreatment"));
                    step.radiusX = readFinite(rx.get(), "blur.radiusX");
                    step.radiusY = readFinite(ry.get(), "blur.radiusY");
                    if (step.radiusX < 0.0f || step.radiusY < 0.0f) throw std::invalid_argument("RenderEffect.blur 半径不能为负数");
                    step.edgeTreatment = static_cast<TileMode>(readIntRange(edge.get(), 0, 3, "blur.edgeTreatment"));
                } else if (type == "offset") {
                    step.kind = RenderEffectStepKind::Offset;
                    ScopedValue ox(context_, JS_GetPropertyStr(context_, item.get(), "x"));
                    ScopedValue oy(context_, JS_GetPropertyStr(context_, item.get(), "y"));
                    step.offsetX = readFinite(ox.get(), "offset.x");
                    step.offsetY = readFinite(oy.get(), "offset.y");
                } else if (type == "colorFilter") {
                    step.kind = RenderEffectStepKind::ColorFilter;
                    ScopedValue filterVal(context_, JS_GetPropertyStr(context_, item.get(), "filter"));
                    step.colorFilter = readColorFilter(filterVal.get());
                } else if (type == "shader") {
                    step.kind = RenderEffectStepKind::Shader;
                    ScopedValue shaderVal(context_, JS_GetPropertyStr(context_, item.get(), "shader"));
                    ScopedValue slotVal(context_, JS_GetPropertyStr(context_, item.get(), "uniformShaderName"));
                    step.shader = readRuntimeShader(shaderVal.get());
                    step.uniformShaderName = readString(slotVal.get(), "shader.uniformShaderName");
                } else {
                    throw std::invalid_argument("renderEffect.step.type 无效：" + type);
                }
                data.steps.push_back(std::move(step));
            }
            return RenderEffectRef(std::move(data));
        }

        [[nodiscard]] canton::core::RenderLayerSnapshotRef readRenderLayerSnapshot(JSValueConst value) const {
            using namespace canton::core;
            if (!JS_IsObject(value) || JS_IsArray(value)) throw std::invalid_argument("RenderLayer 快照无效");
            RenderLayerSnapshot snap;
            snap.handle = readLayerHandle(value);
            ScopedValue verVal(context_, JS_GetPropertyStr(context_, value, "version"));
            snap.version = readUint64(verVal.get(), "RenderLayer.version");
            snap.alpha = readNumberField(value, "alpha", 1.0f);
            if (snap.alpha < 0.0f || snap.alpha > 1.0f) throw std::invalid_argument("RenderLayer.alpha 需要在 0 到 1 之间");
            ScopedValue blendVal(context_, JS_GetPropertyStr(context_, value, "blendMode"));
            snap.blendMode = static_cast<BlendMode>(readIntRange(blendVal.get(), 0, 28, "RenderLayer.blendMode"));
            ScopedValue filterVal(context_, JS_GetPropertyStr(context_, value, "colorFilter"));
            if (!JS_IsUndefined(filterVal.get()) && !JS_IsNull(filterVal.get())) {
                snap.colorFilter = readColorFilter(filterVal.get());
            }
            ScopedValue effectVal(context_, JS_GetPropertyStr(context_, value, "renderEffect"));
            if (!JS_IsUndefined(effectVal.get()) && !JS_IsNull(effectVal.get())) {
                snap.renderEffect = readRenderEffect(effectVal.get());
            }
            ScopedValue clipVal(context_, JS_GetPropertyStr(context_, value, "clip"));
            snap.clip = JS_ToBool(context_, clipVal.get()) > 0;
            ScopedValue shapeVal(context_, JS_GetPropertyStr(context_, value, "shape"));
            if (JS_IsObject(shapeVal.get()) && !JS_IsArray(shapeVal.get())) {
                ScopedValue stVal(context_, JS_GetPropertyStr(context_, shapeVal.get(), "type"));
                const auto st = readString(stVal.get(), "RenderLayer.shape.type");
                if (st == "circle") {
                    snap.shape.type = 2;
                } else if (st == "rounded") {
                    snap.shape.type = 1;
                    snap.shape.corners = {
                        readNumberField(shapeVal.get(), "topStart", 0.0f),
                        readNumberField(shapeVal.get(), "topEnd", 0.0f),
                        readNumberField(shapeVal.get(), "bottomEnd", 0.0f),
                        readNumberField(shapeVal.get(), "bottomStart", 0.0f),
                    };
                } else {
                    snap.shape.type = 0;
                }
            }
            snap.translationX = readNumberField(value, "translationX", 0.0f);
            snap.translationY = readNumberField(value, "translationY", 0.0f);
            snap.scaleX = readNumberField(value, "scaleX", 1.0f);
            snap.scaleY = readNumberField(value, "scaleY", 1.0f);
            snap.rotationZ = readNumberField(value, "rotationZ", 0.0f);
            snap.originX = readNumberField(value, "originX", 0.5f);
            snap.originY = readNumberField(value, "originY", 0.5f);
            snap.topLeftX = readNumberField(value, "topLeftX", 0.0f);
            snap.topLeftY = readNumberField(value, "topLeftY", 0.0f);
            ScopedValue compVal(context_, JS_GetPropertyStr(context_, value, "compositingStrategy"));
            snap.compositingStrategy = static_cast<CompositingStrategy>(readIntRange(compVal.get(), 0, 2, "RenderLayer.compositingStrategy"));
            return RenderLayerSnapshotRef(std::move(snap));
        }

       private:
        JSContext* context_;
        const canton::core::RuntimeEffectRegistry* effects_;
        const canton::core::RenderLayerRegistry* layers_;

        std::uint64_t readHandleUint64(JSValueConst obj, const char* key) const {
            ScopedValue val(context_, JS_GetPropertyStr(context_, obj, key));
            std::uint64_t out = 0;
            if (JS_IsBigInt(val.get())) {
                if (JS_ToBigUint64(context_, &out, val.get()) < 0) throw std::invalid_argument(std::string("句柄 ") + key + " 无效");
                return out;
            }
            double num = 0;
            if (!JS_IsNumber(val.get()) || JS_ToFloat64(context_, &num, val.get()) < 0 || !std::isfinite(num) || num < 0 || std::floor(num) != num) {
                throw std::invalid_argument(std::string("句柄 ") + key + " 无效");
            }
            return static_cast<std::uint64_t>(num);
        }

        std::uint64_t readUint64(JSValueConst val, const char* name) const {
            double num = 0;
            if (!JS_IsNumber(val) || JS_ToFloat64(context_, &num, val) < 0 || !std::isfinite(num) || num < 0 || std::floor(num) != num) {
                throw std::invalid_argument(std::string(name) + " 需要非负整数");
            }
            return static_cast<std::uint64_t>(num);
        }

        std::uint32_t arrayLength(JSValueConst arr) const {
            ScopedValue len(context_, JS_GetPropertyStr(context_, arr, "length"));
            std::uint32_t out = 0;
            if (JS_ToUint32(context_, &out, len.get()) < 0) throw std::invalid_argument("数组长度不可读");
            return out;
        }

        float readFinite(JSValueConst val, const char* name) const {
            double num = 0;
            if (!JS_IsNumber(val) || JS_ToFloat64(context_, &num, val) < 0 || !std::isfinite(num)) {
                throw std::invalid_argument(std::string(name) + " 需要有限数值");
            }
            return static_cast<float>(num);
        }

        float readNumberField(JSValueConst obj, const char* key, float fallback) const {
            ScopedValue val(context_, JS_GetPropertyStr(context_, obj, key));
            if (JS_IsUndefined(val.get())) return fallback;
            return readFinite(val.get(), key);
        }

        int readIntRange(JSValueConst val, int minVal, int maxVal, const char* name) const {
            double num = 0;
            if (!JS_IsNumber(val) || JS_ToFloat64(context_, &num, val) < 0 || !std::isfinite(num) || std::floor(num) != num || num < minVal || num > maxVal) {
                throw std::invalid_argument(std::string(name) + " 超出有效范围");
            }
            return static_cast<int>(num);
        }

        std::uint32_t readColor(JSValueConst val) const {
            double num = 0;
            if (!JS_IsNumber(val) || JS_ToFloat64(context_, &num, val) < 0 || !std::isfinite(num) || std::floor(num) != num || num < 0 || num > 4294967295.0) {
                throw std::invalid_argument("颜色必须是 uint32 数值");
            }
            return static_cast<std::uint32_t>(num);
        }

        std::string readString(JSValueConst val, const char* name) const {
            if (!JS_IsString(val)) throw std::invalid_argument(std::string(name) + " 需要字符串");
            const char* chars = JS_ToCString(context_, val);
            if (!chars) throw std::invalid_argument(std::string(name) + " 不可读");
            std::string result(chars);
            JS_FreeCString(context_, chars);
            return result;
        }
    };
}
