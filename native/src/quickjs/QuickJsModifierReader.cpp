#include "QuickJsModifierReader.h"
#include "ModifierNativeProtocol.h"
#include "QuickJsBrushReader.h"
#include "QuickJsPainterResources.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <span>

namespace canton::quickjs {
    namespace {
        bool fieldsMatch(JSContext* context, JSValueConst object, std::span<const std::string_view> fields, std::string_view owner) {
            JSPropertyEnum* properties = nullptr;
            std::uint32_t count = 0;
            if (JS_GetOwnPropertyNames(context, &properties, &count, object, JS_GPN_STRING_MASK | JS_GPN_SYMBOL_MASK) < 0) return false;

            bool valid = true;
            for (std::uint32_t i = 0; i < count; ++i) {
                const char* name = JS_AtomToCString(context, properties[i].atom);
                if (name == nullptr) {
                    valid = false;
                    break;
                }
                if (std::find(fields.begin(), fields.end(), name) == fields.end()) {
                    JS_ThrowTypeError(context, "Canton %.*s 不接受字段 '%s'", static_cast<int>(owner.size()), owner.data(), name);
                    valid = false;
                }
                JS_FreeCString(context, name);
                if (!valid) break;
            }
            JS_FreePropertyEnum(context, properties, count);
            return valid;
        }

        bool fieldsMatch(JSContext* context, JSValueConst object, std::initializer_list<std::string_view> fields, std::string_view owner) {
            return fieldsMatch(context, object, std::span<const std::string_view>(fields.begin(), fields.size()), owner);
        }

        // 准入字段由 Modifier 原生协议表生成
        bool modifierFieldsMatch(JSContext* context, JSValueConst value, std::string_view type) {
            if (const auto* entry = findModifierProtocol(type)) return fieldsMatch(context, value, entry->fields, type);
            JS_ThrowTypeError(context, "Canton Modifier 类型 '%.*s' 未定义", static_cast<int>(type.size()), type.data());
            return false;
        }
    }

    JSValue QuickJsModifierReader::throwTypeError(const char* message) {
        failed_ = true;
        JS_ThrowTypeError(context_, "%s", message);
        return JS_EXCEPTION;
    }

    JSValue QuickJsModifierReader::throwUnknownModifier(std::string_view type) {
        failed_ = true;
        JS_ThrowTypeError(context_, "Canton Modifier 类型 '%.*s' 未定义", static_cast<int>(type.size()), type.data());
        return JS_EXCEPTION;
    }

    JSValueConst QuickJsModifierReader::payloadFor(JSValueConst element, ScopedValue& value, std::string_view type) {
        if (!JS_IsObject(element) || JS_IsArray(element)) {
            failed_ = true;
            JS_ThrowTypeError(context_, "Canton '%.*s' 的 Modifier 元素需要对象", static_cast<int>(type.size()), type.data());
            return JS_UNDEFINED;
        }
        value = ScopedValue(context_, JS_GetPropertyStr(context_, element, "value"));
        if (!JS_IsObject(value.get()) || JS_IsArray(value.get())) {
            failed_ = true;
            JS_ThrowTypeError(context_, "Canton Modifier '%.*s' 的 value 需要对象", static_cast<int>(type.size()), type.data());
            return JS_UNDEFINED;
        }
        return value.get();
    }

    float QuickJsModifierReader::numberField(JSValueConst object, const char* key, float fallback) const {
        ScopedValue value(context_, JS_GetPropertyStr(context_, object, key));
        if (JS_IsUndefined(value.get())) return fallback;
        if (!JS_IsNumber(value.get())) {
            JS_ThrowTypeError(context_, "Canton Modifier 字段 '%s' 需要数值", key);
            return fallback;
        }
        return static_cast<float>(reader_.toDouble(value.get()));
    }

    bool QuickJsModifierReader::boolField(JSValueConst object, const char* key, bool fallback) const {
        ScopedValue value(context_, JS_GetPropertyStr(context_, object, key));
        if (JS_IsUndefined(value.get())) return fallback;
        if (!JS_IsBool(value.get())) {
            JS_ThrowTypeError(context_, "Canton Modifier 字段 '%s' 需要布尔值", key);
            return fallback;
        }
        return reader_.toBool(value.get());
    }

    float QuickJsModifierReader::requiredNumberField(JSValueConst object, const char* key, std::string_view owner) {
        ScopedValue value(context_, JS_GetPropertyStr(context_, object, key));
        if (JS_IsUndefined(value.get()) || JS_IsNull(value.get())) {
            failed_ = true;
            JS_ThrowTypeError(context_, "Canton %.*s 缺少数值字段 '%s'", static_cast<int>(owner.size()), owner.data(), key);
            return 0.0f;
        }
        if (!JS_IsNumber(value.get())) {
            failed_ = true;
            JS_ThrowTypeError(context_, "Canton %.*s 字段 '%s' 需要数值", static_cast<int>(owner.size()), owner.data(), key);
            return 0.0f;
        }
        return static_cast<float>(reader_.toDouble(value.get()));
    }

    std::string QuickJsModifierReader::requiredStringField(JSValueConst object, const char* key, std::string_view owner) {
        ScopedValue value(context_, JS_GetPropertyStr(context_, object, key));
        if (JS_IsUndefined(value.get()) || JS_IsNull(value.get())) {
            failed_ = true;
            JS_ThrowTypeError(context_, "Canton %.*s 缺少字符串字段 '%s'", static_cast<int>(owner.size()), owner.data(), key);
            return {};
        }
        if (!JS_IsString(value.get())) {
            failed_ = true;
            JS_ThrowTypeError(context_, "Canton %.*s 字段 '%s' 需要字符串", static_cast<int>(owner.size()), owner.data(), key);
            return {};
        }
        return reader_.toString(value.get());
    }

    bool QuickJsModifierReader::readFontStyle(JSValueConst style, canton::core::TextStyle& result) {
        const auto fail = [&](const char* message) {
            failed_ = true;
            JS_ThrowTypeError(context_, "%s", message);
            return false;
        };
        ScopedValue family(context_, JS_GetPropertyStr(context_, style, "fontFamily"));
        if (!JS_IsUndefined(family.get())) {
            if (!JS_IsArray(family.get())) return fail("fontFamily 需要字符串数组");
            ScopedValue length(context_, JS_GetPropertyStr(context_, family.get(), "length"));
            const auto count = static_cast<std::uint32_t>(reader_.toDouble(length.get()));
            if (count > 16) return fail("fontFamily 最多 16 个族名");
            for (std::uint32_t index = 0; index < count; ++index) {
                ScopedValue item(context_, JS_GetPropertyUint32(context_, family.get(), index));
                if (!JS_IsString(item.get())) return fail("fontFamily 的每一项需要字符串");
                auto name = reader_.toString(item.get());
                if (name.empty() || name.size() > 128 || name.find(canton::core::fontFamilySeparator) != std::string::npos) return fail("fontFamily 的族名不能为空、超长或含换行");
                result.fontFamily.push_back(std::move(name));
            }
        }
        const auto weight = numberField(style, "fontWeight", static_cast<float>(canton::core::defaultFontWeight));
        if (JS_HasException(context_)) {
            failed_ = true;
            return false;
        }
        if (!std::isfinite(weight) || std::floor(weight) != weight || weight < canton::core::minFontWeight || weight > canton::core::maxFontWeight) return fail("fontWeight 需要 1 到 1000 的整数");
        result.fontWeight = static_cast<int>(weight);
        ScopedValue fontStyle(context_, JS_GetPropertyStr(context_, style, "fontStyle"));
        if (!JS_IsUndefined(fontStyle.get())) {
            const auto name = JS_IsString(fontStyle.get()) ? reader_.toString(fontStyle.get()) : std::string{};
            if (name != "normal" && name != "italic") return fail("fontStyle 只能是 normal 或 italic");
            result.italic = name == "italic";
        }
        result.fontSynthesis = boolField(style, "fontSynthesis", true);
        if (JS_HasException(context_)) {
            failed_ = true;
            return false;
        }
        return true;
    }

    canton::core::ModifierPadding QuickJsModifierReader::readPadding(JSValueConst value) {
        return {
            requiredNumberField(value, "start", "padding modifier"),
            requiredNumberField(value, "top", "padding modifier"),
            requiredNumberField(value, "end", "padding modifier"),
            requiredNumberField(value, "bottom", "padding modifier"),
        };
    }

    std::pair<float, float> QuickJsModifierReader::transformOriginFrom(JSValueConst value) const {
        const auto name = reader_.stringField(value, "transformOrigin");
        if (name == "TopStart") return {0.0f, 0.0f};
        if (name == "TopCenter") return {0.5f, 0.0f};
        if (name == "TopEnd") return {1.0f, 0.0f};
        if (name == "CenterStart") return {0.0f, 0.5f};
        if (name == "Center") return {0.5f, 0.5f};
        if (name == "CenterEnd") return {1.0f, 0.5f};
        if (name == "BottomStart") return {0.0f, 1.0f};
        if (name == "BottomCenter") return {0.5f, 1.0f};
        if (name == "BottomEnd") return {1.0f, 1.0f};
        ScopedValue origin(context_, JS_GetPropertyStr(context_, value, "transformOrigin"));
        if (JS_IsObject(origin.get()) && !JS_IsArray(origin.get())) {
            if (!fieldsMatch(context_, origin.get(), {"x", "y"}, "transformOrigin")) return {};
            return {
                numberField(origin.get(), "x", 0.5f),
                numberField(origin.get(), "y", 0.5f),
            };
        }
        if (!JS_IsUndefined(origin.get())) JS_ThrowTypeError(context_, "transformOrigin 需要有效的对齐名称或 { x, y }");
        return {0.5f, 0.5f};
    }

    void QuickJsModifierReader::colorOrBrush(JSValueConst object, canton::core::PaintStyleSemantics& style) const {
        ScopedValue color(context_, JS_GetPropertyStr(context_, object, "color"));
        ScopedValue brush(context_, JS_GetPropertyStr(context_, object, "brush"));
        if (!JS_IsUndefined(color.get())) {
            if (!JS_IsUndefined(brush.get())) JS_ThrowTypeError(context_, "颜色输入不能同时提供 color 和 brush");
            style.color = reader_.colorValue(color.get());
            return;
        }
        if (JS_IsUndefined(brush.get())) return;
        try {
            auto value = QuickJsBrushReader(context_).read(brush.get());
            style.color = value.color;
            style.gradient = std::move(value.gradient);
        } catch (const std::exception& error) {
            JS_ThrowTypeError(context_, "%s", error.what());
        }
    }

    canton::core::PaintStyleSemantics QuickJsModifierReader::paintStyle(JSValueConst value, canton::core::PaintStyleKind kind) const {
        canton::core::PaintStyleSemantics style;
        style.kind = kind;
        colorOrBrush(value, style);
        style.strokeWidth = numberField(value, "width", 1.0f);
        ScopedValue shape(context_, JS_GetPropertyStr(context_, value, "shape"));
        if (JS_IsObject(shape.get()) && !JS_IsArray(shape.get())) {
            // 形状名只在读取边界解析一次
            const auto type = reader_.stringField(shape.get(), "type");
            if (type != "rectangle" && type != "rounded" && type != "circle") {
                JS_ThrowTypeError(context_, "shape.type 需要 rectangle、rounded 或 circle");
                return style;
            }
            style.shapeType = type == "rounded" ? canton::core::ShapeKind::Rounded : type == "circle" ? canton::core::ShapeKind::Circle : canton::core::ShapeKind::Rectangle;
            if (style.shapeType == canton::core::ShapeKind::Rounded) {
                if (!fieldsMatch(context_, shape.get(), {"type", "topStart", "topEnd", "bottomEnd", "bottomStart"}, "shape")) return style;
                style.corners = {numberField(shape.get(), "topStart"), numberField(shape.get(), "topEnd"), numberField(shape.get(), "bottomEnd"), numberField(shape.get(), "bottomStart")};
            } else if (!fieldsMatch(context_, shape.get(), {"type"}, "shape"))
                return style;
        } else if (!JS_IsUndefined(shape.get()))
            JS_ThrowTypeError(context_, "shape 需要形状对象");
        style.alpha = numberField(value, "value", 1.0f);
        return style;
    }

    canton::core::ModifierDescriptors QuickJsModifierReader::read(canton::core::NodeId id, JSValueConst modifier, const canton::core::ModifierValue* instanceInput, std::span<const canton::core::ModifierDescriptor* const> previous) {
        canton::core::ModifierDescriptors result;
        failed_ = false;
        if (JS_IsUndefined(modifier) || JS_IsNull(modifier)) {
            (void)throwTypeError("Canton setModifier 需要 Modifier 对象");
            return result;
        }
        ScopedValue elements(context_, JS_GetPropertyStr(context_, modifier, "elements"));
        JSValueConst array = JS_IsArray(elements.get()) ? elements.get() : modifier;
        if (!JS_IsArray(array)) {
            (void)throwTypeError("Canton Modifier.elements 需要数组");
            return result;
        }

        const auto length = reader_.arrayLength(array);

        struct PendingCallback {
            std::size_t index;
            canton::core::EventSlotKind kind;
            ScopedValue callback;
        };

        std::vector<PendingCallback> callbacks;

        for (std::uint32_t i = 0; i < length; ++i) {
            ScopedValue element(context_, JS_GetPropertyUint32(context_, array, i));
            if (!JS_IsObject(element.get()) || JS_IsArray(element.get())) {
                JS_ThrowTypeError(context_, "Canton Modifier 第 %u 项需要对象", i);
                return {};
            }
            const auto type = requiredStringField(element.get(), "type", "modifier element");
            if (failed_) return {};
            constexpr std::string_view elementFields[]{"type", "key", "value"};
            if (!fieldsMatch(context_, element.get(), elementFields, "Modifier 元素")) {
                failed_ = true;
                return {};
            }

            ScopedValue value(context_, JS_UNDEFINED);
            JSValueConst payload = payloadFor(element.get(), value, type);
            if (failed_ || JS_IsUndefined(payload)) return {};
            if (!modifierFieldsMatch(context_, payload, type)) {
                failed_ = true;
                return {};
            }
            // 回调字段的事件种类由 Modifier 原生协议表决定
            const auto eventKind = [protocol = findModifierProtocol(type)](std::string_view field) {
                return modifierCallbackKind(*protocol, field);
            };

            const auto key = reader_.stringField(element.get(), "key");
            if (type == "padding") {
                canton::core::LayoutModifierSemantics item;
                item.kind = canton::core::LayoutModifierKind::Padding;
                item.padding = readPadding(payload);
                if (failed_) return {};
                result.push_back({item, key});
            } else if (type == "intrinsicWidth" || type == "intrinsicHeight") {
                canton::core::LayoutModifierSemantics item;
                item.kind = type == "intrinsicWidth" ? canton::core::LayoutModifierKind::IntrinsicWidth : canton::core::LayoutModifierKind::IntrinsicHeight;
                item.intrinsicMaximum = boolField(payload, "maximum", false);
                result.push_back({item, key});
            } else if (type == "width" || type == "height" || type == "requiredWidth" || type == "requiredHeight") {
                canton::core::LayoutModifierSemantics item;
                if (type == "width")
                    item.kind = canton::core::LayoutModifierKind::Width;
                else if (type == "height")
                    item.kind = canton::core::LayoutModifierKind::Height;
                else if (type == "requiredWidth")
                    item.kind = canton::core::LayoutModifierKind::RequiredWidth;
                else
                    item.kind = canton::core::LayoutModifierKind::RequiredHeight;
                item.value = requiredNumberField(payload, type == "requiredWidth" ? "width" : type == "requiredHeight" ? "height" : "value", type);
                if (failed_) return {};
                result.push_back({item, key});
            } else if (type == "size" || type == "requiredSize") {
                canton::core::LayoutModifierSemantics item;
                item.kind = type == "size" ? canton::core::LayoutModifierKind::Size : canton::core::LayoutModifierKind::RequiredSize;
                item.width = requiredNumberField(payload, "width", type);
                item.height = requiredNumberField(payload, "height", type);
                if (failed_) return {};
                result.push_back({item, key});
            } else if (type == "fillMaxWidth" || type == "fillMaxHeight" || type == "fillMaxSize" || type == "fillParentMaxWidth" || type == "fillParentMaxHeight" || type == "fillParentMaxSize") {
                canton::core::LayoutModifierSemantics item;
                if (type == "fillMaxWidth")
                    item.kind = canton::core::LayoutModifierKind::FillMaxWidth;
                else if (type == "fillMaxHeight")
                    item.kind = canton::core::LayoutModifierKind::FillMaxHeight;
                else if (type == "fillMaxSize")
                    item.kind = canton::core::LayoutModifierKind::FillMaxSize;
                else if (type == "fillParentMaxWidth")
                    item.kind = canton::core::LayoutModifierKind::FillParentMaxWidth;
                else if (type == "fillParentMaxHeight")
                    item.kind = canton::core::LayoutModifierKind::FillParentMaxHeight;
                else
                    item.kind = canton::core::LayoutModifierKind::FillParentMaxSize;
                item.fraction = numberField(payload, "fraction", 1.0f);
                result.push_back({item, key});
            } else if (type == "widthIn" || type == "heightIn" || type == "sizeIn") {
                canton::core::LayoutModifierSemantics item;
                if (type == "widthIn")
                    item.kind = canton::core::LayoutModifierKind::WidthIn;
                else if (type == "heightIn")
                    item.kind = canton::core::LayoutModifierKind::HeightIn;
                else
                    item.kind = canton::core::LayoutModifierKind::SizeIn;
                item.minWidth = numberField(payload, "min", numberField(payload, "minWidth", -1.0f));
                item.maxWidth = numberField(payload, "max", numberField(payload, "maxWidth", -1.0f));
                item.minHeight = numberField(payload, "min", numberField(payload, "minHeight", -1.0f));
                item.maxHeight = numberField(payload, "max", numberField(payload, "maxHeight", -1.0f));
                result.push_back({item, key});
            } else if (type == "defaultMinSize") {
                canton::core::LayoutModifierSemantics item;
                item.kind = canton::core::LayoutModifierKind::DefaultMinSize;
                item.minWidth = numberField(payload, "minWidth");
                item.minHeight = numberField(payload, "minHeight");
                result.push_back({item, key});
            } else if (type == "verticalScroll" || type == "horizontalScroll") {
                canton::core::LayoutModifierSemantics item;
                item.kind = type == "verticalScroll" ? canton::core::LayoutModifierKind::VerticalScroll : canton::core::LayoutModifierKind::HorizontalScroll;
                ScopedValue callback(context_, JS_GetPropertyStr(context_, payload, "onScroll"));
                if (!JS_IsFunction(context_, callback.get())) {
                    JS_ThrowTypeError(context_, "Canton Modifier '%.*s' 的 onScroll 需要函数", static_cast<int>(type.size()), type.data());
                    return {};
                }
                item.scrollValue = numberField(payload, "value");
                item.enabled = boolField(payload, "enabled", true);
                callbacks.push_back({result.size(), eventKind("onScroll"), std::move(callback)});
                result.push_back({item, key});
            } else if (type == "animateContentSize") {
                canton::core::AnimateContentSizeModifier item;
                ScopedValue spec(context_, JS_GetPropertyStr(context_, payload, "animationSpec"));
                if (!JS_IsObject(spec.get()) || JS_IsArray(spec.get())) {
                    (void)throwTypeError("animateContentSize 需要 animationSpec 对象");
                    return {};
                }
                const auto kind = requiredStringField(spec.get(), "kind", "animationSpec");
                const auto fields = kind == "tween" ? std::initializer_list<std::string_view>{"kind", "durationMillis", "delayMillis", "x1", "y1", "x2", "y2", "iterations", "repeatMode"} : kind == "spring" ? std::initializer_list<std::string_view>{"kind", "stiffness", "dampingRatio", "visibilityThreshold"} : std::initializer_list<std::string_view>{"kind", "delayMillis", "iterations", "repeatMode"};
                if (!fieldsMatch(context_, spec.get(), fields, "animationSpec")) {
                    failed_ = true;
                    return {};
                }
                if (kind == "tween")
                    item.animationSpec.kind = canton::core::AnimationKind::Tween;
                else if (kind == "spring")
                    item.animationSpec.kind = canton::core::AnimationKind::Spring;
                else if (kind == "snap")
                    item.animationSpec.kind = canton::core::AnimationKind::Snap;
                else {
                    (void)throwTypeError("动画规格类型未定义");
                    return {};
                }
                auto& input = item.animationSpec;
                input.durationMillis = numberField(spec.get(), "durationMillis", 300);
                input.delayMillis = numberField(spec.get(), "delayMillis", 0);
                input.stiffness = numberField(spec.get(), "stiffness", 400);
                input.dampingRatio = numberField(spec.get(), "dampingRatio", 1);
                input.threshold = numberField(spec.get(), "visibilityThreshold", 0.01f);
                input.bezier = {numberField(spec.get(), "x1", 0.4f), numberField(spec.get(), "y1", 0), numberField(spec.get(), "x2", 0.2f), numberField(spec.get(), "y2", 1)};
                double iterations = 1;
                ScopedValue iterationValue(context_, JS_GetPropertyStr(context_, spec.get(), "iterations"));
                if (!JS_IsUndefined(iterationValue.get()) && (!JS_IsNumber(iterationValue.get()) || JS_ToFloat64(context_, &iterations, iterationValue.get()) < 0)) {
                    (void)throwTypeError("动画重复次数必须是数字");
                    return {};
                }
                if (!std::isfinite(iterations) || iterations < 1 || iterations > 2147483647.0 || std::floor(iterations) != iterations) {
                    (void)throwTypeError("动画重复次数必须是正整数");
                    return {};
                }
                input.iterations = static_cast<int>(iterations);
                const auto repeatMode = reader_.stringField(spec.get(), "repeatMode", "restart");
                if (repeatMode != "restart" && repeatMode != "reverse") {
                    (void)throwTypeError("动画重复模式未定义");
                    return {};
                }
                input.reverse = repeatMode == "reverse";
                item.clip = boolField(payload, "clip", true);
                result.push_back({item, key});
            } else if (type == "text" || type == "textField") {
                canton::core::TextModifier text;
                const auto editable = type == "textField";
                text.text = requiredStringField(payload, editable ? "value" : "text", type);
                const auto minLines = numberField(payload, "minLines", 1);
                const auto maxLines = numberField(payload, "maxLines", 0);
                if (!std::isfinite(minLines) || !std::isfinite(maxLines) || std::floor(minLines) != minLines || std::floor(maxLines) != maxLines || minLines < 1 || maxLines < 0 || minLines > 1000000 || maxLines > 1000000) {
                    (void)throwTypeError("文本行数必须是有效整数");
                    return {};
                }
                text.minLines = static_cast<int>(minLines);
                text.maxLines = static_cast<int>(maxLines);
                text.singleLine = boolField(payload, "singleLine", editable && text.minLines == 1 && text.maxLines <= 1);
                if (!enumField(payload, "textAlign", canton::core::parseTextAlign, text.textAlign) || !enumField(payload, "overflow", canton::core::parseTextOverflow, text.overflow)) return {};

                const auto styleName = editable ? "textStyle" : "style";
                ScopedValue style(context_, JS_GetPropertyStr(context_, payload, styleName));
                if (!JS_IsUndefined(style.get())) {
                    if (!JS_IsObject(style.get()) || JS_IsArray(style.get()) || !fieldsMatch(context_, style.get(), {"fontSize", "lineHeight", "color", "fontFamily", "fontWeight", "fontStyle", "fontSynthesis"}, styleName)) {
                        (void)throwTypeError((std::string(styleName) + " 需要正式文本样式对象").c_str());
                        return {};
                    }
                    text.style.fontSize = numberField(style.get(), "fontSize", canton::core::defaultFontSize);
                    text.style.lineHeight = numberField(style.get(), "lineHeight", 0);
                    if (!readFontStyle(style.get(), text.style)) return {};
                    ScopedValue color(context_, JS_GetPropertyStr(context_, style.get(), "color"));
                    if (!JS_IsUndefined(color.get())) {
                        text.color = reader_.colorValue(color.get());
                        if (JS_HasException(context_)) {
                            failed_ = true;
                            return {};
                        }
                    }
                }
                if (editable) {
                    canton::core::TextFieldModifier field;
                    field.value = text.text;
                    field.placeholder = reader_.stringField(payload, "placeholder");
                    field.presentation = std::move(text);
                    field.enabled = boolField(payload, "enabled", true);
                    field.selectAllOnFocus = boolField(payload, "selectAllOnFocus", false);
                    for (const auto* name : {"onValueChange", "onSubmit", "onChange", "onBlur"}) callbacks.push_back({result.size(), eventKind(name), ScopedValue(context_, JS_GetPropertyStr(context_, payload, name))});
                    result.push_back({std::move(field), key});
                } else
                    result.push_back({std::move(text), key});
            } else if (type == "paint") {
                canton::core::PaintModifier item;
                ScopedValue painter(context_, JS_GetPropertyStr(context_, payload, "painter"));
                const auto snapshot = QuickJsPainterResources::read(context_, painter.get());
                if (!snapshot) {
                    failed_ = true;
                    return {};
                }
                item.painter = *snapshot;
                item.alpha = numberField(payload, "alpha", 1.0f);
                item.sizeToIntrinsics = boolField(payload, "sizeToIntrinsics", true);
                if (!enumField(payload, "contentScale", canton::core::parseContentScale, item.contentScale) || !enumField(payload, "alignment", canton::core::parseImageAlignment, item.alignment)) return {};
                if (boolField(payload, "tile", false)) item.repeat = canton::core::ImageRepeat::Tile;
                ScopedValue slices(context_, JS_GetPropertyStr(context_, payload, "ninePatch"));
                if (!JS_IsUndefined(slices.get())) {
                    if (item.repeat == canton::core::ImageRepeat::Tile || !JS_IsObject(slices.get()) || JS_IsArray(slices.get()) || !fieldsMatch(context_, slices.get(), {"left", "top", "right", "bottom"}, "ninePatch")) {
                        if (!JS_HasException(context_)) JS_ThrowTypeError(context_, "ninePatch 需要 { left, top, right, bottom }，且不能与 tile 同用");
                        failed_ = true;
                        return {};
                    }
                    item.repeat = canton::core::ImageRepeat::NinePatch;
                    item.slices = {numberField(slices.get(), "left"), numberField(slices.get(), "top"), numberField(slices.get(), "right"), numberField(slices.get(), "bottom")};
                }
                ScopedValue filter(context_, JS_GetPropertyStr(context_, payload, "colorFilter"));
                if (!JS_IsUndefined(filter.get())) {
                    if (!JS_IsObject(filter.get()) || !fieldsMatch(context_, filter.get(), {"tint"}, "colorFilter")) {
                        failed_ = true;
                        return {};
                    }
                    ScopedValue tint(context_, JS_GetPropertyStr(context_, filter.get(), "tint"));
                    item.tint = reader_.colorValue(tint.get());
                    if (JS_HasException(context_)) {
                        failed_ = true;
                        return {};
                    }
                }
                result.push_back({item, key});
            } else if (type == "weight" || type == "align" || type == "matchParentSize" || type == "alignBy" || type == "fillCrossAxis") {
                canton::core::ParentDataModifierSemantics item;
                if (type == "weight") {
                    item.weight = requiredNumberField(payload, "weight", type);
                    item.weightFill = boolField(payload, "fill", true);
                } else if (type == "align") {
                    item.kind = canton::core::ParentDataKind::Align;
                    const auto name = requiredStringField(payload, "alignment", type);
                    item.align = canton::core::parseLayoutAlignment(name);
                    if (!failed_ && !item.align) {
                        JS_ThrowTypeError(context_, "align 不支持对齐值：'%s'", name.c_str());
                        failed_ = true;
                        return {};
                    }
                } else if (type == "alignBy") {
                    item.kind = canton::core::ParentDataKind::AlignBy;
                    const auto name = requiredStringField(payload, "line", type);
                    item.alignBy = canton::core::parseAlignmentLine(name);
                    if (!failed_ && !item.alignBy) {
                        JS_ThrowTypeError(context_, "alignBy 不支持对齐线：'%s'", name.c_str());
                        failed_ = true;
                        return {};
                    }
                } else if (type == "fillCrossAxis") {
                    item.kind = canton::core::ParentDataKind::FillCrossAxis;
                    item.fillCrossAxis = requiredNumberField(payload, "fraction", type);
                } else
                    item.kind = canton::core::ParentDataKind::MatchParentSize;
                result.push_back({item, key});
            } else if (type == "offset" || type == "absoluteOffset") {
                result.push_back({canton::core::OffsetModifier{numberField(payload, "x"), numberField(payload, "y")}, key});
            } else if (type == "drawBehind" || type == "drawWithContent" || type == "drawWithCache") {
                canton::core::DrawModifier item;
                item.kind = type == "drawBehind" ? canton::core::DrawModifierKind::Behind : type == "drawWithContent" ? canton::core::DrawModifierKind::WithContent : canton::core::DrawModifierKind::WithCache;
                ScopedValue revisionValue(context_, JS_GetPropertyStr(context_, payload, "revision"));
                if (!JS_IsUndefined(revisionValue.get()) && !JS_IsNumber(revisionValue.get())) {
                    (void)throwTypeError("绘制 revision 需要数值");
                    return {};
                }
                const auto revision = JS_IsUndefined(revisionValue.get()) ? 0.0 : reader_.toDouble(revisionValue.get());
                if (!std::isfinite(revision) || revision < 0 || std::floor(revision) != revision || revision > 9007199254740991.0) {
                    (void)throwTypeError("绘制 revision 需要非负安全整数");
                    return {};
                }
                item.revision = static_cast<std::uint64_t>(revision);
                item.transactional = boolField(payload, "transactional", false);
                ScopedValue callback(context_, JS_GetPropertyStr(context_, payload, "prepare"));
                if (!JS_IsFunction(context_, callback.get())) {
                    (void)throwTypeError("绘制 prepare 需要函数");
                    return {};
                }
                callbacks.push_back({result.size(), eventKind("prepare"), std::move(callback)});
                result.push_back({item, key});
            } else if (type == "graphicsLayer") {
                canton::core::TransformModifierSemantics item;
                item.translationX = numberField(payload, "translationX");
                item.translationY = numberField(payload, "translationY");
                item.scaleX = numberField(payload, "scaleX", 1.0f);
                item.scaleY = numberField(payload, "scaleY", 1.0f);
                item.rotationZ = numberField(payload, "rotationZ");
                const auto origin = transformOriginFrom(payload);
                item.transformOriginX = origin.first;
                item.transformOriginY = origin.second;
                item.alpha = numberField(payload, "alpha", 1.0f);
                item.clip = boolField(payload, "clip", false);
                result.push_back({item, key});
            } else if (type == "zIndex") {
                result.push_back({canton::core::ZIndexModifier{requiredNumberField(payload, "value", type)}, key});
            } else if (type == "dropShadow" || type == "innerShadow") {
                using canton::core::EffectKind;
                canton::core::EffectModifier effect;
                effect.kind = type == "dropShadow" ? EffectKind::DropShadow : EffectKind::InnerShadow;
                // 形状与颜色沿用背景的读取，颜色只属于阴影
                effect.shape = paintStyle(payload, canton::core::PaintStyleKind::Background);
                effect.color = effect.shape.color;
                effect.shape.color = 0;
                effect.radius = numberField(payload, "radius");
                effect.spread = numberField(payload, "spread");
                ScopedValue offset(context_, JS_GetPropertyStr(context_, payload, "offset"));
                if (!JS_IsUndefined(offset.get())) {
                    if (!JS_IsObject(offset.get()) || JS_IsArray(offset.get()) || !fieldsMatch(context_, offset.get(), {"x", "y"}, "offset")) {
                        if (!JS_HasException(context_)) JS_ThrowTypeError(context_, "offset 需要 { x, y }");
                        failed_ = true;
                        return {};
                    }
                    effect.offset = {numberField(offset.get(), "x"), numberField(offset.get(), "y")};
                }
                result.push_back({effect, key});
            } else if (type == "background" || type == "border" || type == "alpha") {
                auto kind = canton::core::PaintStyleKind::Background;
                if (type == "border")
                    kind = canton::core::PaintStyleKind::Border;
                else if (type == "alpha")
                    kind = canton::core::PaintStyleKind::Alpha;
                result.push_back({paintStyle(payload, kind), key});
            } else if (type == "clip") {
                result.push_back({canton::core::ClipModifier{paintStyle(payload, canton::core::PaintStyleKind::Background)}, key});
            } else if (type == "semantics") {
                canton::core::SemanticsModifier item;
                const auto role = reader_.stringField(payload, "role");
                constexpr std::string_view roles[]{"", "button", "slider", "switch", "checkbox", "textField", "image", "group"};
                const auto found = std::find(std::begin(roles), std::end(roles), role);
                if (found == std::end(roles)) {
                    (void)throwTypeError("semantics.role 角色名称无效");
                    return {};
                }
                item.role = static_cast<canton::core::SemanticsRole>(found - std::begin(roles));
                item.label = reader_.stringField(payload, "label");
                item.description = reader_.stringField(payload, "description");
                item.valueText = reader_.stringField(payload, "valueText");
                item.enabled = boolField(payload, "enabled", true);
                for (const auto* name : {"increment", "decrement"}) {
                    ScopedValue action(context_, JS_GetPropertyStr(context_, payload, name));
                    if (JS_IsUndefined(action.get()) || JS_IsNull(action.get())) continue;
                    if (!JS_IsFunction(context_, action.get())) {
                        JS_ThrowTypeError(context_, "semantics.%s 需要函数", name);
                        return {};
                    }
                    callbacks.push_back({result.size(), eventKind(name), std::move(action)});
                }
                if (JS_HasException(context_)) return {};
                result.push_back({item, key});
            } else if (type == "focusRequester" || type == "focusProperties" || type == "focusGroup" || type == "onFocusChanged") {
                canton::core::FocusModifier item;
                item.kind = type == "focusRequester" ? canton::core::FocusModifierKind::Requester : type == "focusGroup" ? canton::core::FocusModifierKind::Group : type == "onFocusChanged" ? canton::core::FocusModifierKind::Observer : canton::core::FocusModifierKind::Properties;
                const auto identity = [&](const char* name, bool required = false) -> std::uint32_t {
                    ScopedValue input(context_, JS_GetPropertyStr(context_, payload, name));
                    if (!required && JS_IsUndefined(input.get())) return 0;
                    double value = 0;
                    if (!JS_IsNumber(input.get()) || JS_ToFloat64(context_, &value, input.get()) < 0 || !std::isfinite(value) || std::floor(value) != value || value < (required ? 1 : 0) || value > static_cast<double>(UINT32_MAX)) {
                        (void)throwTypeError("焦点请求器身份必须是有效整数");
                        return 0;
                    }
                    return static_cast<std::uint32_t>(value);
                };
                if (item.kind == canton::core::FocusModifierKind::Requester) item.requester = identity("requester", true);
                if (item.kind == canton::core::FocusModifierKind::Properties) {
                    item.canFocus = boolField(payload, "canFocus", true);
                    constexpr const char* directions[]{"next", "previous", "up", "down", "left", "right"};
                    for (std::size_t i = 0; i < 6; ++i) item.directions[i] = identity(directions[i]);
                }
                if (JS_HasException(context_)) return {};
                if (item.kind == canton::core::FocusModifierKind::Observer) {
                    ScopedValue callback(context_, JS_GetPropertyStr(context_, payload, "callback"));
                    if (!JS_IsFunction(context_, callback.get())) {
                        (void)throwTypeError("onFocusChanged 需要函数");
                        return {};
                    }
                    callbacks.push_back({result.size(), eventKind("callback"), std::move(callback)});
                }
                result.push_back({item, key});
            } else if (type == "clickable" || type == "hoverable" || type == "focusable" || type == "onContextClick" || type == "pointerIcon" || type == "onKeyEvent" || type == "onWheel" || type == "draggable" || type == "pointerInput") {
                using InputKind = canton::core::InputModifierKind;
                canton::core::InputModifierSemantics item;
                item.enabled = boolField(payload, "enabled", true);
                item.focusable = boolField(payload, "focusable", true);
                // 必填回调缺失时报错；可选回调为 undefined 时不登记槽位
                const auto callback = [&](const char* name, bool required) {
                    ScopedValue value(context_, JS_GetPropertyStr(context_, payload, name));
                    const auto present = !JS_IsUndefined(value.get()) && !JS_IsNull(value.get());
                    if ((required || present) && !JS_IsFunction(context_, value.get())) {
                        JS_ThrowTypeError(context_, "%.*s.%s 需要函数", static_cast<int>(type.size()), type.data(), name);
                        return false;
                    }
                    if (present) callbacks.push_back({result.size(), eventKind(name), std::move(value)});
                    return true;
                };
                const auto readAxis = [&] {
                    const auto axis = reader_.stringField(payload, "axis");
                    if (axis == "horizontal")
                        item.axis = canton::core::DragAxis::Horizontal;
                    else if (axis == "vertical")
                        item.axis = canton::core::DragAxis::Vertical;
                    else if (axis.empty() || axis == "both")
                        item.axis = canton::core::DragAxis::Both;
                    else {
                        JS_ThrowTypeError(context_, "%.*s.axis 需要 horizontal、vertical 或 both", static_cast<int>(type.size()), type.data());
                        return false;
                    }
                    return true;
                };
                auto ok = true;
                if (type == "clickable")
                    ok = callback("onClick", true) && callback("onDoubleClick", false) && callback("interaction", false);
                else if (type == "hoverable") {
                    item.kind = InputKind::Hoverable;
                    ok = callback("onEnter", false) && callback("onExit", false) && callback("interaction", false);
                } else if (type == "focusable") {
                    item.kind = InputKind::Focusable;
                    ok = callback("interaction", false);
                } else if (type == "onContextClick") {
                    item.kind = InputKind::ContextClick;
                    ok = callback("callback", true);
                } else if (type == "onKeyEvent") {
                    item.kind = InputKind::KeyEvent;
                    ok = callback("callback", true);
                } else if (type == "pointerInput") {
                    item.kind = InputKind::PointerInput;
                    ok = callback("callback", true);
                } else if (type == "onWheel") {
                    item.kind = InputKind::Wheel;
                    ok = readAxis() && callback("callback", true);
                } else if (type == "pointerIcon") {
                    item.kind = InputKind::PointerIcon;
                    const auto icon = reader_.stringField(payload, "icon");
                    constexpr std::string_view icons[]{"default", "hand", "text", "crosshair", "resizeHorizontal", "resizeVertical", "grab", "grabbing", "none"};
                    const auto found = std::find(std::begin(icons), std::end(icons), icon);
                    if (found == std::end(icons)) {
                        (void)throwTypeError("pointerIcon 光标名称无效");
                        return {};
                    }
                    item.icon = static_cast<canton::core::PointerIcon>(found - std::begin(icons));
                } else {
                    item.kind = InputKind::Draggable;
                    if (!readAxis()) {
                        failed_ = true;
                        return {};
                    }
                    item.startImmediately = boolField(payload, "startImmediately", false);
                    item.pointerLock = boolField(payload, "pointerLock", false);
                    ok = callback("onDelta", true) && callback("onStart", false) && callback("onStop", false) && callback("interaction", false);
                }
                if (!ok) {
                    failed_ = true;
                    return {};
                }
                result.push_back({item, key});
            } else {
                (void)throwUnknownModifier(type);
                return {};
            }
        }

        if (failed_ || JS_HasException(context_)) {
            failed_ = true;
            return {};
        }
        try {
            canton::core::validateModifierDescriptors(result);
        } catch (const std::exception& error) {
            (void)throwTypeError(error.what());
            return {};
        }
        if (instanceInput && (result.size() != 1 || !canton::core::sameModifierKind(*instanceInput, result.front().value))) {
            (void)throwTypeError("Canton Modifier 单实例更新不能改变实例类型");
            return {};
        }
        // 整条描述通过校验之后才登记回调，失败的描述不会留下半条注册记录
        const auto matches = canton::core::matchModifierDescriptors(previous, result);
        std::vector<canton::core::EventSlotId> retained;
        for (auto& pending : callbacks) {
            const auto match = matches[pending.index];
            const auto* prior = instanceInput ? instanceInput : match < previous.size() ? &previous[match]->value : nullptr;
            const auto oldSlot = prior ? canton::core::modifierEventSlot(*prior, pending.kind) : canton::core::EventSlotId{};
            const auto slot = events_.updateModifierCallback(id, pending.kind, pending.callback.get(), oldSlot, transaction_);
            auto& input = result[pending.index].value;
            if (auto* draw = std::get_if<canton::core::DrawModifier>(&input))
                draw->prepare = slot;
            else if (auto* scroll = std::get_if<canton::core::LayoutModifierSemantics>(&input))
                scroll->eventSlot = slot;
            else if (auto* field = std::get_if<canton::core::TextFieldModifier>(&input)) {
                using Kind = canton::core::EventSlotKind;
                if (pending.kind == Kind::InputUpdate)
                    field->onValueChange = slot;
                else if (pending.kind == Kind::InputSubmit)
                    field->onSubmit = slot;
                else if (pending.kind == Kind::InputChange)
                    field->onChange = slot;
                else if (pending.kind == Kind::InputBlur)
                    field->onBlur = slot;
            } else if (auto* focus = std::get_if<canton::core::FocusModifier>(&input))
                focus->eventSlot = slot;
            else if (auto* semantics = std::get_if<canton::core::SemanticsModifier>(&input))
                (pending.kind == canton::core::EventSlotKind::SemanticsDecrement ? semantics->decrement : semantics->increment) = slot;
            else
                canton::core::inputEventSlot(std::get<canton::core::InputModifierSemantics>(input), pending.kind) = slot;
            if (slot.valid()) retained.push_back(slot);
        }
        if (!instanceInput) events_.releaseModifierCallbacksExcept(id, retained, transaction_);
        return result;
    }
}

