#include "QuickJsNativeApi.h"

#include "QuickJsModifierReader.h"
#include "QuickJsRuntimeContext.h"
#include "QuickJsValueReader.h"

#include <core/PropSchema.h>
#include <core/Version.h>
#include <core/Log.h>

#include <atomic>
#include <cmath>
#include <iterator>
#include <optional>

namespace canton::quickjs {
    namespace {
        std::atomic<canton::core::WindowId> nextGlobalWindowId{1};

        QuickJsRuntimeContext* runtime(JSContext* context) {
            return static_cast<QuickJsRuntimeContext*>(JS_GetContextOpaque(context));
        }

        canton::core::NodeType nodeTypeFor(QuickJsRuntimeContext& runtime, canton::core::NodeId id) {
            const auto it = runtime.nodeTypes.find(id);
            return it == runtime.nodeTypes.end() ? canton::core::NodeType::Unknown : it->second;
        }

        std::string payloadStringField(JSContext* context, QuickJsValueReader& reader, JSValueConst object, const char* key, std::string_view fallback = {}) {
            if (!JS_IsObject(object)) return std::string(fallback);
            ScopedValue value(context, JS_GetPropertyStr(context, object, key));
            if (JS_IsUndefined(value.get()) || JS_IsNull(value.get())) return std::string(fallback);
            return reader.toString(value.get());
        }

        core::LogLevel readLogLevel(JSContext* context, QuickJsValueReader& reader, JSValueConst value) {
            const auto level = reader.toString(value);
            if (level == "v") return core::LogLevel::Verbose;
            if (level == "d") return core::LogLevel::Debug;
            if (level == "i") return core::LogLevel::Info;
            if (level == "w") return core::LogLevel::Warn;
            if (level == "e") return core::LogLevel::Error;
            JS_ThrowTypeError(context, "Log 级别无效: %s", level.c_str());
            return core::LogLevel::Info;
        }

        std::string readArgs(JSContext* context, QuickJsValueReader& reader, JSValueConst value) {
            if (!JS_IsArray(value)) return {};
            ScopedValue length(context, JS_GetPropertyStr(context, value, "length"));
            std::uint32_t count = 0;
            if (JS_ToUint32(context, &count, length.get()) < 0) return {};
            std::string result;
            for (std::uint32_t i = 0; i < count; ++i) {
                ScopedValue arg(context, JS_GetPropertyUint32(context, value, i));
                if (i != 0) result.push_back(' ');
                result += reader.toString(arg.get());
            }
            return result;
        }

        std::uint32_t readIndex(JSContext* context, JSValueConst value, bool allowZero = false) {
            double number = 0;
            if (!JS_IsNumber(value) || JS_ToFloat64(context, &number, value) < 0 || !std::isfinite(number) || std::floor(number) != number || number < (allowZero ? 0 : 1) || number > UINT32_MAX) {
                JS_ThrowTypeError(context, "Canton identity/index must be an exact uint32 number");
                return 0;
            }
            return static_cast<std::uint32_t>(number);
        }

        bool readSizeField(JSContext* context, JSValueConst object, const char* key, bool required, canton::core::Size& out) {
            ScopedValue value(context, JS_GetPropertyStr(context, object, key));
            if (JS_IsUndefined(value.get()) || JS_IsNull(value.get())) {
                if (!required) return false;
                JS_ThrowTypeError(context, "窗口配置缺少 %s", key);
                return false;
            }
            if (!JS_IsObject(value.get()) || JS_IsArray(value.get())) {
                JS_ThrowTypeError(context, "窗口配置 %s 需要 { width, height }", key);
                return false;
            }
            ScopedValue w(context, JS_GetPropertyStr(context, value.get(), "width"));
            ScopedValue h(context, JS_GetPropertyStr(context, value.get(), "height"));
            double width = 0, height = 0;
            if (!JS_IsNumber(w.get()) || !JS_IsNumber(h.get()) || JS_ToFloat64(context, &width, w.get()) < 0 || JS_ToFloat64(context, &height, h.get()) < 0 || !std::isfinite(width) || !std::isfinite(height) || width <= 0 || height <= 0) {
                JS_ThrowRangeError(context, "窗口尺寸 %s 必须是正有限数值", key);
                return false;
            }
            out = {static_cast<float>(width), static_cast<float>(height)};
            return true;
        }

        bool readWindowConfig(JSContext* context, JSValueConst value, canton::core::WindowConfigSpec& out) {
            if (!JS_IsObject(value) || JS_IsArray(value)) {
                JS_ThrowTypeError(context, "窗口配置需要对象");
                return false;
            }
            QuickJsValueReader reader(context);
            ScopedValue ownerVal(context, JS_GetPropertyStr(context, value, "ownerWindowId"));
            if (!JS_IsUndefined(ownerVal.get()) && !JS_IsNull(ownerVal.get())) {
                const auto ownerId = readIndex(context, ownerVal.get());
                if (JS_HasException(context)) return false;
                out.ownerWindowId = ownerId;
            }
            out.title = payloadStringField(context, reader, value, "title");
            out.icon = payloadStringField(context, reader, value, "icon");
            ScopedValue visibleVal(context, JS_GetPropertyStr(context, value, "visible"));
            if (!JS_IsUndefined(visibleVal.get())) {
                if (!JS_IsBool(visibleVal.get())) {
                    JS_ThrowTypeError(context, "窗口 visible 需要布尔值");
                    return false;
                }
                out.visible = JS_ToBool(context, visibleVal.get()) > 0;
            }
            if (!readSizeField(context, value, "sizeDp", true, out.sizeDp) && JS_HasException(context)) return false;
            canton::core::Size minSize{}, maxSize{};
            if (readSizeField(context, value, "minSizeDp", false, minSize)) out.minSizeDp = minSize;
            else if (JS_HasException(context)) return false;
            if (readSizeField(context, value, "maxSizeDp", false, maxSize)) out.maxSizeDp = maxSize;
            else if (JS_HasException(context)) return false;

            ScopedValue posVal(context, JS_GetPropertyStr(context, value, "position"));
            if (JS_IsString(posVal.get())) {
                const auto preset = reader.toString(posVal.get());
                if (preset == "platformDefault")
                    out.position = {canton::core::WindowPositionPreset::PlatformDefault, 0, 0};
                else if (preset == "centerScreen")
                    out.position = {canton::core::WindowPositionPreset::CenterScreen, 0, 0};
                else if (preset == "centerOwner")
                    out.position = {canton::core::WindowPositionPreset::CenterOwner, 0, 0};
                else {
                    JS_ThrowTypeError(context, "窗口位置预设无效: %s", preset.c_str());
                    return false;
                }
            } else if (JS_IsObject(posVal.get()) && !JS_IsArray(posVal.get())) {
                ScopedValue xVal(context, JS_GetPropertyStr(context, posVal.get(), "x"));
                ScopedValue yVal(context, JS_GetPropertyStr(context, posVal.get(), "y"));
                double x = 0, y = 0;
                if (!JS_IsNumber(xVal.get()) || !JS_IsNumber(yVal.get()) || JS_ToFloat64(context, &x, xVal.get()) < 0 || JS_ToFloat64(context, &y, yVal.get()) < 0 || !std::isfinite(x) || !std::isfinite(y)) {
                    JS_ThrowRangeError(context, "窗口坐标 { x, y } 必须是有限数值");
                    return false;
                }
                out.position = {canton::core::WindowPositionPreset::Coordinates, static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y))};
            } else if (!JS_IsUndefined(posVal.get())) {
                JS_ThrowTypeError(context, "窗口 position 无效");
                return false;
            }

            const auto placement = payloadStringField(context, reader, value, "placement", "normal");
            if (placement == "normal")
                out.placement = canton::core::WindowPlacement::Normal;
            else if (placement == "minimized")
                out.placement = canton::core::WindowPlacement::Minimized;
            else if (placement == "maximized")
                out.placement = canton::core::WindowPlacement::Maximized;
            else if (placement == "fullscreen")
                out.placement = canton::core::WindowPlacement::Fullscreen;
            else {
                JS_ThrowTypeError(context, "窗口 placement 无效: %s", placement.c_str());
                return false;
            }

            const auto modality = payloadStringField(context, reader, value, "modality", "none");
            if (modality == "none")
                out.modality = canton::core::WindowModality::None;
            else if (modality == "owner") {
                if (!out.ownerWindowId) {
                    JS_ThrowTypeError(context, "modality='owner' 需要有效的 ownerWindowId");
                    return false;
                }
                out.modality = canton::core::WindowModality::Owner;
            } else if (modality == "application")
                out.modality = canton::core::WindowModality::Application;
            else {
                JS_ThrowTypeError(context, "窗口 modality 无效: %s", modality.c_str());
                return false;
            }

            const auto chrome = payloadStringField(context, reader, value, "chrome", "system");
            if (chrome == "system")
                out.chrome = canton::core::WindowChrome::System;
            else if (chrome == "customTitleBar")
                out.chrome = canton::core::WindowChrome::CustomTitleBar;
            else if (chrome == "frameless")
                out.chrome = canton::core::WindowChrome::Frameless;
            else {
                JS_ThrowTypeError(context, "窗口 chrome 无效: %s", chrome.c_str());
                return false;
            }

            const auto role = payloadStringField(context, reader, value, "role", "standard");
            if (role == "standard")
                out.role = canton::core::WindowRole::Standard;
            else if (role == "utility")
                out.role = canton::core::WindowRole::Utility;
            else {
                JS_ThrowTypeError(context, "窗口 role 无效: %s", role.c_str());
                return false;
            }

            const auto boolField = [&](const char* key, bool& target) {
                ScopedValue item(context, JS_GetPropertyStr(context, value, key));
                if (JS_IsUndefined(item.get())) return true;
                if (!JS_IsBool(item.get())) {
                    JS_ThrowTypeError(context, "窗口 %s 需要布尔值", key);
                    return false;
                }
                target = JS_ToBool(context, item.get()) > 0;
                return true;
            };
            if (!boolField("resizable", out.resizable) || !boolField("minimizable", out.minimizable) || !boolField("maximizable", out.maximizable) || !boolField("closable", out.closable) || !boolField("alwaysOnTop", out.alwaysOnTop) || !boolField("transparent", out.transparent)) return false;

            ScopedValue taskbarVal(context, JS_GetPropertyStr(context, value, "showInTaskbar"));
            if (!JS_IsUndefined(taskbarVal.get()) && !JS_IsNull(taskbarVal.get())) {
                if (!JS_IsBool(taskbarVal.get())) {
                    JS_ThrowTypeError(context, "窗口 showInTaskbar 需要布尔值");
                    return false;
                }
                out.showInTaskbar = JS_ToBool(context, taskbarVal.get()) > 0;
            }

            const auto backdrop = payloadStringField(context, reader, value, "backdrop", "none");
            if (backdrop == "none")
                out.backdrop = canton::core::WindowBackdrop::None;
            else if (backdrop == "mica")
                out.backdrop = canton::core::WindowBackdrop::Mica;
            else if (backdrop == "micaAlt")
                out.backdrop = canton::core::WindowBackdrop::MicaAlt;
            else if (backdrop == "acrylic")
                out.backdrop = canton::core::WindowBackdrop::Acrylic;
            else {
                JS_ThrowTypeError(context, "窗口 backdrop 无效: %s", backdrop.c_str());
                return false;
            }

            const auto corner = payloadStringField(context, reader, value, "cornerPreference", "default");
            if (corner == "default")
                out.cornerPreference = canton::core::WindowCornerPreference::Default;
            else if (corner == "round")
                out.cornerPreference = canton::core::WindowCornerPreference::Round;
            else if (corner == "roundSmall")
                out.cornerPreference = canton::core::WindowCornerPreference::RoundSmall;
            else if (corner == "square")
                out.cornerPreference = canton::core::WindowCornerPreference::Square;
            else {
                JS_ThrowTypeError(context, "窗口 cornerPreference 无效: %s", corner.c_str());
                return false;
            }
            using namespace canton::core;
            if (out.transparent && (out.chrome != WindowChrome::Frameless || out.backdrop != WindowBackdrop::None ||
                                    out.cornerPreference == WindowCornerPreference::Round || out.cornerPreference == WindowCornerPreference::RoundSmall)) {
                JS_ThrowTypeError(context, "透明窗口要求 chrome='frameless'、无背景材质且不使用系统圆角");
                return false;
            }
            return !JS_HasException(context);
        }

        bool validateHostValue(QuickJsRuntimeContext& runtime, canton::core::NodeId id, canton::core::HostInput input, const canton::core::PropValue& value, std::string& error) {
            if (input == canton::core::HostInput::MeasurePolicy) {
                const auto previous = runtime.measurePolicies.find(id);
                try {
                    runtime.measurePolicies[id] = canton::core::readMeasurePolicy(value, previous == runtime.measurePolicies.end() ? nullptr : &previous->second);
                } catch (const std::exception& failure) {
                    error = failure.what();
                    return false;
                }
                return true;
            }
            return canton::core::validateSetPropMutation(nodeTypeFor(runtime, id), std::string(canton::core::hostInputName(input)), value, error);
        }

        JSValue nativeConfigureApp(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (!self || argc < 1 || !JS_IsObject(argv[0]) || JS_IsArray(argv[0])) return JS_ThrowTypeError(context, "configureApp 需要配置对象");
            QuickJsValueReader reader(context);
            canton::core::AppConfigSpec spec;
            ScopedValue exitVal(context, JS_GetPropertyStr(context, argv[0], "exitOnLastWindowClosed"));
            if (!JS_IsUndefined(exitVal.get())) {
                if (!JS_IsBool(exitVal.get())) return JS_ThrowTypeError(context, "exitOnLastWindowClosed 需要布尔值");
                spec.exitOnLastWindowClosed = JS_ToBool(context, exitVal.get()) > 0;
            }
            spec.appId = payloadStringField(context, reader, argv[0], "appId");
            spec.icon = payloadStringField(context, reader, argv[0], "icon");
            if (JS_HasException(context)) return JS_EXCEPTION;
            self->pendingAppConfig = std::move(spec);
            if (self->wakeOwner) self->wakeOwner();
            return JS_UNDEFINED;
        }

        JSValue nativeRequestAppExit(JSContext* context, JSValueConst, int, JSValueConst*) {
            auto* self = runtime(context);
            if (!self) return JS_UNDEFINED;
            self->appExitRequested = true;
            if (self->wakeOwner) self->wakeOwner();
            return JS_UNDEFINED;
        }

        JSValue nativeAllocateWindowId(JSContext* context, JSValueConst, int, JSValueConst*) {
            const auto id = nextGlobalWindowId.fetch_add(1, std::memory_order_relaxed);
            if (id == 0) return JS_ThrowRangeError(context, "窗口身份已耗尽");
            return JS_NewUint32(context, id);
        }

        JSValue nativeCreateWindow(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (!self || argc < 3) return JS_ThrowTypeError(context, "createWindow 需要 windowId、rootNodeId 和配置");
            const auto windowId = readIndex(context, argv[0]);
            if (JS_HasException(context)) return JS_EXCEPTION;
            const auto rootNodeId = readIndex(context, argv[1]);
            if (JS_HasException(context)) return JS_EXCEPTION;
            canton::core::WindowConfigSpec config;
            if (!readWindowConfig(context, argv[2], config)) return JS_EXCEPTION;
            self->nodeWindows[rootNodeId] = windowId;
            self->windowMutations.push_back({WindowMutationKind::Create, windowId, rootNodeId, std::move(config)});
            return JS_UNDEFINED;
        }

        JSValue nativeUpdateWindow(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (!self || argc < 2) return JS_ThrowTypeError(context, "updateWindow 需要 windowId 和配置");
            const auto windowId = readIndex(context, argv[0]);
            if (JS_HasException(context)) return JS_EXCEPTION;
            canton::core::WindowConfigSpec config;
            if (!readWindowConfig(context, argv[1], config)) return JS_EXCEPTION;
            self->windowMutations.push_back({WindowMutationKind::Update, windowId, 0, std::move(config)});
            return JS_UNDEFINED;
        }

        JSValue nativeDestroyWindow(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (!self || argc < 1) return JS_ThrowTypeError(context, "destroyWindow 需要 windowId");
            const auto windowId = readIndex(context, argv[0]);
            if (JS_HasException(context)) return JS_EXCEPTION;
            self->windowMutations.push_back({WindowMutationKind::Destroy, windowId, 0, {}});
            return JS_UNDEFINED;
        }

        JSValue nativeWindowCommand(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (!self || argc < 2 || !JS_IsString(argv[1])) return JS_ThrowTypeError(context, "windowCommand 需要 windowId 和命令名称");
            const auto windowId = readIndex(context, argv[0]);
            if (JS_HasException(context)) return JS_EXCEPTION;
            QuickJsValueReader reader(context);
            const auto cmd = reader.toString(argv[1]);
            canton::core::WindowCommandKind kind;
            if (cmd == "activate")
                kind = canton::core::WindowCommandKind::Activate;
            else if (cmd == "requestClose")
                kind = canton::core::WindowCommandKind::RequestClose;
            else
                return JS_ThrowTypeError(context, "windowCommand 不支持命令: %s", cmd.c_str());
            self->windowCommands.push_back({windowId, kind});
            if (self->wakeOwner) self->wakeOwner();
            return JS_UNDEFINED;
        }

        JSValue nativeInstallWindowDriver(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (!self || argc != 1 || !JS_IsObject(argv[0]) || JS_IsArray(argv[0])) return JS_ThrowTypeError(context, "窗口驱动需要回调表对象");
            if (!JS_IsUndefined(self->onWindowCloseRequest)) return JS_ThrowTypeError(context, "一个 Owner 只能安装一个窗口驱动");
            ScopedValue closeReq(context, JS_GetPropertyStr(context, argv[0], "onCloseRequest"));
            ScopedValue geomChanged(context, JS_GetPropertyStr(context, argv[0], "onGeometryChanged"));
            ScopedValue focusChanged(context, JS_GetPropertyStr(context, argv[0], "onFocusChanged"));
            ScopedValue dpiChanged(context, JS_GetPropertyStr(context, argv[0], "onDpiChanged"));
            if (!JS_IsFunction(context, closeReq.get()) || !JS_IsFunction(context, geomChanged.get()) || !JS_IsFunction(context, focusChanged.get()) || !JS_IsFunction(context, dpiChanged.get())) {
                return JS_ThrowTypeError(context, "窗口驱动回调必须全部是函数");
            }
            self->onWindowCloseRequest = JS_DupValue(context, closeReq.get());
            self->onWindowGeometryChanged = JS_DupValue(context, geomChanged.get());
            self->onWindowFocusChanged = JS_DupValue(context, focusChanged.get());
            self->onWindowDpiChanged = JS_DupValue(context, dpiChanged.get());
            return JS_UNDEFINED;
        }

        JSValue nativeBeginRearrange(JSContext* context, JSValueConst, int, JSValueConst*) {
            auto* self = runtime(context);
            try {
                self->beginRearrange();
            } catch (const std::exception& error) {
                return JS_ThrowInternalError(context, "%s", error.what());
            }
            return JS_UNDEFINED;
        }

        JSValue nativeSubmitRearrange(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (!self->rearrangeCheckpoint || !JS_IsUndefined(self->rearrangeCompletion)) return JS_ThrowTypeError(context, "没有可提交的重排候选");
            if (argc != 1 || !JS_IsFunction(context, argv[0])) return JS_ThrowTypeError(context, "重排提交必须提供结果回调");
            self->rearrangeCompletion = JS_DupValue(context, argv[0]);
            self->stampRearrangeSubmission();
            return JS_UNDEFINED;
        }

        JSValue nativeAbortRearrange(JSContext* context, JSValueConst, int, JSValueConst*) {
            auto* self = runtime(context);
            self->abortRearrange();
            JS_FreeValue(context, self->rearrangeCompletion);
            self->rearrangeCompletion = JS_UNDEFINED;
            return JS_UNDEFINED;
        }

        JSValue nativeContinueRearrange(JSContext* context, JSValueConst, int, JSValueConst*) {
            auto* self = runtime(context);
            if (!self || !self->materializingLayout || !self->rearrangeCheckpoint || JS_IsUndefined(self->rearrangeCompletion)) return JS_ThrowTypeError(context, "只有原生测量材料化可以继续已提交候选");
            self->stampRearrangeSubmission();
            return JS_UNDEFINED;
        }

        JSValue nativeInstallLayoutDriver(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (!self || argc != 1 || !JS_IsFunction(context, argv[0]) || !JS_IsUndefined(self->materializeLayout)) return JS_ThrowTypeError(context, "一个 Owner 只能安装一个 Layout 材料化驱动");
            self->materializeLayout = JS_DupValue(context, argv[0]);
            return JS_UNDEFINED;
        }

        JSValue nativeCreateNode(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (self == nullptr || argc < 2) return JS_UNDEFINED;
            QuickJsValueReader reader(context);
            const auto id = readIndex(context, argv[0]);
            if (JS_HasException(context)) return JS_EXCEPTION;
            if (!JS_IsString(argv[1])) return JS_ThrowTypeError(context, "Canton node type must be a string");
            const auto type = canton::core::nodeTypeFromName(reader.toString(argv[1]));
            if (type == canton::core::NodeType::Unknown) return JS_ThrowTypeError(context, "Canton unknown native node type");
            if (id == 0 || self->nodeTypes.contains(id)) return JS_ThrowTypeError(context, "Canton createNode requires a fresh nonzero node id");
            canton::core::WindowId windowId = 0;
            if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2])) {
                windowId = readIndex(context, argv[2], true);
                if (JS_HasException(context)) return JS_EXCEPTION;
            }
            const auto generation = canton::core::allocateRuntimeIdentity();
            self->nodeGenerations[id] = generation;
            self->nodeWindows[id] = windowId;
            if (self->rootNodeId == 0) self->rootNodeId = id;
            self->nodeTypes[id] = type;
            self->push(id, canton::core::CreateNodeMutation{id, type, generation});
            return JS_UNDEFINED;
        }

        JSValue nativeDeleteNode(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (self == nullptr || argc < 1) return JS_UNDEFINED;
            const auto id = readIndex(context, argv[0]);
            if (JS_HasException(context)) return JS_EXCEPTION;
            const auto windowId = self->windowOfNode(id);
            self->retireSubtree(id);
            if (auto* transaction = self->transactionForWindow(windowId)) {
                transaction->operations.emplace_back(canton::core::DeleteNodeMutation{id});
            }
            return JS_UNDEFINED;
        }

        JSValue nativeInsertChild(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (self == nullptr || argc < 3) return JS_UNDEFINED;
            const auto parent = readIndex(context, argv[0]);
            if (JS_HasException(context)) return JS_EXCEPTION;
            const auto child = readIndex(context, argv[1]);
            if (JS_HasException(context)) return JS_EXCEPTION;
            const auto index = readIndex(context, argv[2], true);
            if (JS_HasException(context)) return JS_EXCEPTION;
            try {
                self->attachChild(parent, child, index);
            } catch (const std::exception& error) {
                return JS_ThrowTypeError(context, "%s", error.what());
            }
            self->push(parent, canton::core::InsertChildMutation{parent, child, index});
            return JS_UNDEFINED;
        }

        JSValue nativeRemoveChild(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (self == nullptr || argc < 2) return JS_UNDEFINED;
            const auto parent = readIndex(context, argv[0]);
            if (JS_HasException(context)) return JS_EXCEPTION;
            const auto child = readIndex(context, argv[1]);
            if (JS_HasException(context)) return JS_EXCEPTION;
            self->detachChild(parent, child);
            self->push(parent, canton::core::RemoveChildMutation{parent, child});
            return JS_UNDEFINED;
        }

        JSValue nativeSetProp(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (self == nullptr || argc < 3) return JS_UNDEFINED;
            QuickJsValueReader reader(context);
            const auto id = readIndex(context, argv[0]);
            if (JS_HasException(context)) return JS_EXCEPTION;
            if (!self->nodeGenerations.contains(id)) return JS_ThrowReferenceError(context, "Canton node does not exist");
            const auto key = reader.toString(argv[1]);
            if (JS_IsFunction(context, argv[2])) {
                return JS_ThrowTypeError(context, "原生输入 %s 不接受函数，事件回调须通过正式 Modifier 字段注册", key.c_str());
            }
            auto value = reader.propValue(argv[2]);
            if (JS_HasException(context)) return JS_EXCEPTION;
            const auto input = canton::core::hostInputFromName(key);
            if (!input) return JS_ThrowTypeError(context, "Canton unsupported host input: %s", key.c_str());
            std::string propError;
            if (!validateHostValue(*self, id, *input, value, propError)) {
                return JS_ThrowTypeError(context, "%s", propError.c_str());
            }
            self->setHostInput(id, *input, std::move(value));
            return JS_UNDEFINED;
        }

        JSValue nativeSetModifier(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (self == nullptr || argc < 2) return JS_UNDEFINED;
            const auto id = readIndex(context, argv[0]);
            if (JS_HasException(context)) return JS_EXCEPTION;
            if (!self->nodeGenerations.contains(id)) return JS_ThrowReferenceError(context, "Canton node does not exist");
            QuickJsModifierReader modifierReader(context, self->events, self->transactionForNode(id));
            const auto previous = self->modifierInputDescriptors(id);
            auto modifier = modifierReader.read(id, argv[1], nullptr, previous);
            if (modifierReader.failed() || JS_HasException(context)) {
                return JS_EXCEPTION;
            }
            self->setModifierChain(id, std::move(modifier));
            return JS_UNDEFINED;
        }

        JSValue bindingValue(JSContext* context, canton::core::BindingHandle handle) {
            const auto result = JS_NewObject(context);
            JS_SetPropertyStr(context, result, "identity", JS_NewBigUint64(context, handle.identity));
            JS_SetPropertyStr(context, result, "generation", JS_NewBigUint64(context, handle.generation));
            return result;
        }

        std::optional<canton::core::BindingHandle> readBinding(JSContext* context, JSValueConst value) {
            if (!JS_IsObject(value)) {
                JS_ThrowTypeError(context, "Canton binding handle must be an object");
                return std::nullopt;
            }
            ScopedValue identity(context, JS_GetPropertyStr(context, value, "identity"));
            ScopedValue generation(context, JS_GetPropertyStr(context, value, "generation"));
            if (!JS_IsBigInt(identity.get()) || !JS_IsBigInt(generation.get())) {
                JS_ThrowTypeError(context, "Canton binding identity and generation must be bigint");
                return std::nullopt;
            }
            canton::core::BindingHandle handle;
            if (JS_ToBigUint64(context, &handle.identity, identity.get()) < 0 || JS_ToBigUint64(context, &handle.generation, generation.get()) < 0) return std::nullopt;
            ScopedValue canonicalIdentity(context, JS_NewBigUint64(context, handle.identity));
            ScopedValue canonicalGeneration(context, JS_NewBigUint64(context, handle.generation));
            if (!handle.valid() || !JS_IsStrictEqual(context, identity.get(), canonicalIdentity.get()) || !JS_IsStrictEqual(context, generation.get(), canonicalGeneration.get())) {
                JS_ThrowRangeError(context, "Canton binding identity and generation must be nonzero uint64");
                return std::nullopt;
            }
            return handle;
        }

        JSValue nativeModifierInstances(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (!self || argc != 1) return JS_ThrowTypeError(context, "Canton modifierInstances expects node id");
            const auto id = readIndex(context, argv[0]);
            if (JS_HasException(context)) return JS_EXCEPTION;
            std::vector<const QuickJsRuntimeContext::PublishedModifier*> instances;
            for (const auto& [_, instance] : self->publishedModifiers) {
                if (instance.node.id == id && self->nodeGenerations.contains(id) && self->nodeGenerations.at(id) == instance.node.generation) instances.push_back(&instance);
            }
            std::sort(instances.begin(), instances.end(), [](const auto* left, const auto* right) { return left->position < right->position; });
            const auto result = JS_NewArray(context);
            std::uint32_t index = 0;
            for (const auto* instance : instances) {
                const auto item = bindingValue(context, {instance->handle.identity, instance->handle.generation});
                JS_SetPropertyStr(context, item, "key", JS_NewString(context, instance->descriptor.key.c_str()));
                const auto kind = canton::core::modifierKindName(instance->descriptor.value);
                JS_SetPropertyStr(context, item, "kind", JS_NewStringLen(context, kind.data(), kind.size()));
                JS_SetPropertyUint32(context, result, index++, item);
            }
            return result;
        }

        JSValue nativeRegisterModifierBinding(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (!self || argc != 2) return JS_ThrowTypeError(context, "Canton registerModifierBinding expects node id and instance handle");
            const auto id = readIndex(context, argv[0]);
            if (JS_HasException(context)) return JS_EXCEPTION;
            const auto handle = readBinding(context, argv[1]);
            if (!handle) return JS_EXCEPTION;
            const auto found = self->publishedModifiers.find(handle->identity);
            if (found == self->publishedModifiers.end() || found->second.handle.generation != handle->generation || found->second.node.id != id || !self->nodeGenerations.contains(id) || self->nodeGenerations.at(id) != found->second.node.generation) {
                ++self->rejectedBindingUpdates;
                return JS_ThrowReferenceError(context, "Canton Modifier instance is retired or not published");
            }
            std::vector<canton::core::BindingHandle> previous;
            for (const auto& [_, binding] : self->bindings) {
                const auto* target = std::get_if<canton::core::ModifierInputTarget>(&binding.target);
                if (target && target->modifier == found->second.handle) previous.push_back(binding.handle);
            }
            for (auto binding : previous) self->retireBinding(binding);
            return bindingValue(context, self->registerBinding(canton::core::ModifierInputTarget{found->second.node, found->second.handle}));
        }

        JSValue nativeRegisterBinding(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (self == nullptr || argc != 2 || !JS_IsNumber(argv[0]) || !JS_IsString(argv[1])) return JS_ThrowTypeError(context, "Canton registerBinding expects node id and input name");
            QuickJsValueReader reader(context);
            const auto id = readIndex(context, argv[0]);
            if (JS_HasException(context)) return JS_EXCEPTION;
            if (!self->nodeGenerations.contains(id)) return JS_ThrowReferenceError(context, "Canton binding node does not exist");
            const canton::core::NodeHandle node{id, self->nodeGenerations.at(id)};
            const auto name = reader.toString(argv[1]);
            canton::core::BindingHandle handle;
            if (name == "modifier") {
                if (const auto previous = self->modifierBindings.find(id); previous != self->modifierBindings.end()) self->retireBinding(previous->second);
                handle = self->registerBinding(canton::core::ModifierChainTarget{node});
                self->modifierBindings[id] = handle;
            } else {
                const auto input = canton::core::hostInputFromName(name);
                if (!input) return JS_ThrowTypeError(context, "Canton unsupported binding input: %s", name.c_str());
                if (const auto previous = self->hostBindings[id].find(*input); previous != self->hostBindings[id].end()) self->retireBinding(previous->second);
                handle = self->registerBinding(canton::core::HostInputTarget{node, *input});
                self->hostBindings[id][*input] = handle;
            }
            return bindingValue(context, handle);
        }

        JSValue nativeUpdateBinding(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (self == nullptr || argc != 2) return JS_ThrowTypeError(context, "Canton updateBinding expects handle and value");
            const auto handle = readBinding(context, argv[0]);
            if (!handle) return JS_EXCEPTION;
            const auto found = self->bindings.find(handle->identity);
            if (found == self->bindings.end() || found->second.handle != *handle) {
                ++self->rejectedBindingUpdates;
                return JS_ThrowReferenceError(context, "Canton binding is retired or belongs to another context");
            }
            const auto target = found->second.target;
            return std::visit(
                [&](const auto& input) -> JSValue {
                    using T = std::decay_t<decltype(input)>;
                    if constexpr (std::is_same_v<T, canton::core::HostInputTarget>) {
                        QuickJsValueReader reader(context);
                        auto value = reader.propValue(argv[1]);
                        if (JS_HasException(context)) return JS_EXCEPTION;
                        std::string error;
                        if (!validateHostValue(*self, input.node.id, input.input, value, error)) return JS_ThrowTypeError(context, "%s", error.c_str());
                        self->updateBinding(*handle, std::move(value));
                    } else if constexpr (std::is_same_v<T, canton::core::ModifierChainTarget>) {
                        QuickJsModifierReader reader(context, self->events, self->transactionForNode(input.node.id));
                        const auto previous = self->modifierInputDescriptors(input.node.id);
                        auto value = reader.read(input.node.id, argv[1], nullptr, previous);
                        if (reader.failed() || JS_HasException(context)) return JS_EXCEPTION;
                        self->updateBinding(*handle, std::move(value));
                    } else if constexpr (std::is_same_v<T, canton::core::ModifierInputTarget>) {
                        auto& inputs = self->modifierInputs[input.node.id];
                        const auto instance = std::find_if(inputs.begin(), inputs.end(), [&](const auto& value) { return value.handle == input.modifier; });
                        if (instance == inputs.end()) return JS_ThrowReferenceError(context, "Modifier 实例已从当前候选退出");
                        ScopedValue array(context, JS_NewArray(context));
                        JS_SetPropertyUint32(context, array.get(), 0, JS_DupValue(context, argv[1]));
                        QuickJsModifierReader reader(context, self->events, self->transactionForNode(input.node.id));
                        auto descriptors = reader.read(input.node.id, array.get(), &instance->descriptor.value);
                        if (reader.failed() || JS_HasException(context)) return JS_EXCEPTION;
                        self->updateBinding(*handle, std::move(descriptors.front().value));
                    }
                    return JS_UNDEFINED;
                },
                target);
        }

        JSValue nativeReleaseBinding(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (self == nullptr || argc != 1) return JS_ThrowTypeError(context, "Canton releaseBinding expects handle");
            const auto handle = readBinding(context, argv[0]);
            if (!handle) return JS_EXCEPTION;
            self->retireBinding(*handle);
            return JS_UNDEFINED;
        }

        JSValue nativeUnmount(JSContext* context, JSValueConst, int, JSValueConst*) {
            auto* self = runtime(context);
            if (self == nullptr) return JS_UNDEFINED;
            if (self->rootNodeId != 0 && self->windowOfNode(self->rootNodeId) == 0) {
                self->retireSubtree(self->rootNodeId);
                self->push(self->rootNodeId, canton::core::DeleteNodeMutation{self->rootNodeId});
            }
            self->events.releaseAll(self->currentTransaction());
            self->childrenByNode.clear();
            self->parentByNode.clear();
            self->nodeTypes.clear();
            self->publishedModifiers.clear();
            self->nodeGenerations.clear();
            self->nodeWindows.clear();
            self->hostBindings.clear();
            self->bindings.clear();
            self->modifierBindings.clear();
            self->rootNodeId = 0;
            self->frameRequested = false;
            self->framePrepared = false;
            for (auto* val : {&self->prepareFrame, &self->completeFrame, &self->disposeApp, &self->materializeLayout, &self->onWindowCloseRequest, &self->onWindowGeometryChanged, &self->onWindowFocusChanged, &self->onWindowDpiChanged}) {
                JS_FreeValue(context, *val);
                *val = JS_UNDEFINED;
            }
            return JS_UNDEFINED;
        }

        JSValue nativeLog(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (self == nullptr || argc != 3 || !JS_IsArray(argv[2])) return JS_ThrowTypeError(context, "Log 需要级别、TAG 和参数数组");
            QuickJsValueReader reader(context);
            const auto tag = reader.toString(argv[1]);
            if (tag.empty()) return JS_ThrowTypeError(context, "Log TAG 不能为空");
            const auto mapped = readLogLevel(context, reader, argv[0]);
            if (JS_HasException(context)) return JS_EXCEPTION;
            core::Log::write(mapped, tag, readArgs(context, reader, argv[2]));
            return JS_UNDEFINED;
        }

        JSValue nativeDiagnosticsToast(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (self == nullptr || argc != 5 || !JS_IsArray(argv[3])) return JS_ThrowTypeError(context, "DiagnosticsToast 需要级别、TAG、标题、参数数组和合并选项");
            QuickJsValueReader reader(context);
            const auto level = readLogLevel(context, reader, argv[0]);
            const auto tag = reader.toString(argv[1]);
            if (tag.empty()) return JS_ThrowTypeError(context, "DiagnosticsToast TAG 不能为空");
            QuickJsToastRequest toast;
            toast.level = level;
            toast.tag = tag;
            toast.title = reader.toString(argv[2]);
            if (toast.title.empty()) return JS_ThrowTypeError(context, "DiagnosticsToast 标题不能为空");
            toast.content = readArgs(context, reader, argv[3]);
            toast.coalesce = reader.toBool(argv[4]);
            self->recordToast(std::move(toast));
            return JS_UNDEFINED;
        }

        JSValue nativeReload(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (self == nullptr) return JS_UNDEFINED;
            QuickJsValueReader reader(context);
            QuickJsDiagnosticAction action;
            action.kind = QuickJsDiagnosticActionKind::RequestReload;
            action.message = "Script requested reload";
            const auto payload = argc > 0 ? argv[0] : JS_UNDEFINED;
            if (JS_IsObject(payload)) {
                action.path = payloadStringField(context, reader, payload, "path");
                ScopedValue timestamp(context, JS_GetPropertyStr(context, payload, "timestamp"));
                if (JS_IsNumber(timestamp.get())) action.timestamp = reader.toDouble(timestamp.get());
            }
            if (JS_HasException(context)) return JS_EXCEPTION;
            self->recordDiagnosticAction(std::move(action));
            return JS_UNDEFINED;
        }

        JSValue nativeDiagnosticsTriggerFakeError(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (self == nullptr) return JS_UNDEFINED;
            QuickJsValueReader reader(context);
            const auto message = argc > 0 ? payloadStringField(context, reader, argv[0], "Manual script diagnostic error") : std::string("Manual script diagnostic error");
            QuickJsDiagnosticAction action;
            action.kind = QuickJsDiagnosticActionKind::TriggerFakeError;
            action.message = message;
            self->recordDiagnosticAction(std::move(action));
            return JS_ThrowInternalError(context, "%s", message.c_str());
        }

        JSValue nativeDiagnosticsSetToastsEnabled(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (self == nullptr || argc < 1) return JS_UNDEFINED;
            QuickJsValueReader reader(context);
            QuickJsDiagnosticAction action;
            action.kind = QuickJsDiagnosticActionKind::SetToastsEnabled;
            action.enabled = reader.toBool(argv[0]);
            self->recordDiagnosticAction(std::move(action));
            return JS_UNDEFINED;
        }

        JSValue performanceMeasureNow(JSContext* context, JSValueConst, int, JSValueConst*) {
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            return JS_NewFloat64(context, std::chrono::duration<double, std::milli>(now).count());
        }

        JSValue performanceNow(JSContext* context, JSValueConst, int, JSValueConst*) {
            auto* self = runtime(context);
            return JS_NewFloat64(context, self == nullptr ? 0.0 : self->frameTimeMillis);
        }

        JSValue nativeInstallFrameDriver(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtime(context);
            if (!self || argc != 3) return JS_ThrowTypeError(context, "帧驱动需要准备、完成和销毁回调");
            if (!JS_IsUndefined(self->prepareFrame)) return JS_ThrowTypeError(context, "一个 Owner 只能安装一个帧驱动");
            for (int i = 0; i < argc; ++i)
                if (!JS_IsFunction(context, argv[i])) return JS_ThrowTypeError(context, "帧驱动参数必须是函数");

            self->prepareFrame = JS_DupValue(context, argv[0]);
            self->completeFrame = JS_DupValue(context, argv[1]);
            self->disposeApp = JS_DupValue(context, argv[2]);
            return JS_UNDEFINED;
        }

        JSValue nativeRequestFrame(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            if (auto* self = runtime(context)) {
                if (argc != 1 || !JS_IsBool(argv[0])) return JS_ThrowTypeError(context, "帧需求必须是布尔值");
                self->frameRequested = JS_ToBool(context, argv[0]);
                if (self->frameRequested && self->wakeOwner) self->wakeOwner();
            }
            return JS_UNDEFINED;
        }

        JSValue nativeFocusCommand(JSContext* context, JSValueConst, int argc, JSValueConst* argv) {
            if (argc < 2 || !JS_IsString(argv[0])) return JS_ThrowTypeError(context, "焦点命令需要操作与请求器身份");
            auto* self = runtime(context);
            QuickJsValueReader reader(context);
            const auto operation = reader.toString(argv[0]);
            const auto requester = readIndex(context, argv[1], operation != "request" && operation != "cancel");
            if (JS_HasException(context)) return JS_EXCEPTION;
            canton::core::WindowId windowId = 0;
            if (argc > 3 && !JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3])) {
                windowId = readIndex(context, argv[3], true);
                if (JS_HasException(context)) return JS_EXCEPTION;
            }
            canton::core::FocusCommand command;
            command.requester = requester;
            if (operation == "request")
                command.kind = canton::core::FocusCommandKind::Request;
            else if (operation == "clear")
                command.kind = canton::core::FocusCommandKind::Clear;
            else if (operation == "cancel")
                command.kind = canton::core::FocusCommandKind::Cancel;
            else if (operation == "move") {
                command.kind = canton::core::FocusCommandKind::Move;
                const auto direction = argc > 2 && !JS_IsUndefined(argv[2]) ? reader.toString(argv[2]) : "";
                constexpr std::string_view names[]{"next", "previous", "up", "down", "left", "right"};
                const auto found = std::find(std::begin(names), std::end(names), direction);
                if (found == std::end(names)) return JS_ThrowTypeError(context, "焦点方向无效");
                command.direction = static_cast<canton::core::FocusDirection>(found - std::begin(names));
            } else
                return JS_ThrowTypeError(context, "焦点操作无效");
            self->focusCommands.push_back(command);
            self->scopedFocusCommands.push_back({windowId, command});
            if (self->wakeOwner) self->wakeOwner();
            return JS_TRUE;
        }

        const JSCFunctionListEntry nativeApiFunctions[] = {
            JS_CFUNC_DEF("configureApp", 1, nativeConfigureApp),
            JS_CFUNC_DEF("requestAppExit", 0, nativeRequestAppExit),
            JS_CFUNC_DEF("allocateWindowId", 0, nativeAllocateWindowId),
            JS_CFUNC_DEF("createWindow", 3, nativeCreateWindow),
            JS_CFUNC_DEF("updateWindow", 2, nativeUpdateWindow),
            JS_CFUNC_DEF("destroyWindow", 1, nativeDestroyWindow),
            JS_CFUNC_DEF("windowCommand", 2, nativeWindowCommand),
            JS_CFUNC_DEF("installWindowDriver", 1, nativeInstallWindowDriver),
            JS_CFUNC_DEF("focusCommand", 4, nativeFocusCommand),
            JS_CFUNC_DEF("installLayoutDriver", 1, nativeInstallLayoutDriver),
            JS_CFUNC_DEF("continueRearrange", 0, nativeContinueRearrange),
            JS_CFUNC_DEF("currentTime", 0, performanceNow),
            JS_CFUNC_DEF("installFrameDriver", 3, nativeInstallFrameDriver),
            JS_CFUNC_DEF("requestFrame", 1, nativeRequestFrame),
            JS_CFUNC_DEF("beginRearrange", 0, nativeBeginRearrange),
            JS_CFUNC_DEF("submitRearrange", 1, nativeSubmitRearrange),
            JS_CFUNC_DEF("abortRearrange", 0, nativeAbortRearrange),
            JS_CFUNC_DEF("createNode", 3, nativeCreateNode),
            JS_CFUNC_DEF("deleteNode", 1, nativeDeleteNode),
            JS_CFUNC_DEF("insertChild", 3, nativeInsertChild),
            JS_CFUNC_DEF("removeChild", 2, nativeRemoveChild),
            JS_CFUNC_DEF("setProp", 3, nativeSetProp),
            JS_CFUNC_DEF("setModifier", 2, nativeSetModifier),
            JS_CFUNC_DEF("registerBinding", 2, nativeRegisterBinding),
            JS_CFUNC_DEF("modifierInstances", 1, nativeModifierInstances),
            JS_CFUNC_DEF("registerModifierBinding", 2, nativeRegisterModifierBinding),
            JS_CFUNC_DEF("updateBinding", 2, nativeUpdateBinding),
            JS_CFUNC_DEF("releaseBinding", 1, nativeReleaseBinding),
            JS_CFUNC_DEF("unmount", 0, nativeUnmount),
            JS_CFUNC_DEF("reload", 1, nativeReload),
            JS_CFUNC_DEF("log", 3, nativeLog),
            JS_CFUNC_DEF("diagnosticsToast", 5, nativeDiagnosticsToast),
            JS_CFUNC_DEF("diagnosticsRequestReload", 1, nativeReload),
            JS_CFUNC_DEF("diagnosticsTriggerFakeError", 1, nativeDiagnosticsTriggerFakeError),
            JS_CFUNC_DEF("diagnosticsSetToastsEnabled", 1, nativeDiagnosticsSetToastsEnabled),
        };
    }

    void QuickJsNativeApi::install(JSContext* context, QuickJsRuntimeContext& runtime) {
        ScopedValue global(context, JS_GetGlobalObject(context));
        ScopedValue native(context, JS_NewObject(context));
        JS_SetPropertyFunctionList(context, native.get(), nativeApiFunctions, static_cast<int>(std::size(nativeApiFunctions)));
        runtime.painters->install(native.get());
        JS_SetPropertyStr(context, native.get(), "readAsset",
                          JS_NewCFunction(
                              context,
                              [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
                                  auto& state = *static_cast<QuickJsRuntimeContext*>(JS_GetContextOpaque(ctx));
                                  if (!state.assetLoader) return JS_ThrowTypeError(ctx, "当前 App 不提供资产读取");
                                  if (argc != 2 || !JS_IsString(argv[0]) || state.assetRequests.size() >= 32) return JS_ThrowTypeError(ctx, "资产请求无效或超过并发预算");
                                  size_t length = 0;
                                  const auto* name = JS_ToCStringLen(ctx, &length, argv[0]);
                                  if (!name) return JS_EXCEPTION;
                                  const std::string reference(name, length);
                                  JS_FreeCString(ctx, name);
                                  if (reference.empty() || reference.size() > 4096 || reference.find('\0') != std::string::npos || reference.starts_with("data:")) return JS_ThrowTypeError(ctx, "资产地址无效");
                                  try {
                                      auto bytes = state.assetLoader(reference, state.wakeOwner);
                                      JSValue callbacks[2];
                                      auto promise = JS_NewPromiseCapability(ctx, callbacks);
                                      if (JS_IsException(promise)) return promise;
                                      state.assetRequests.push_back({std::move(bytes), callbacks[0], callbacks[1], JS_ToBool(ctx, argv[1]) != 0});
                                      return promise;
                                  } catch (const std::exception& error) {
                                      return JS_ThrowInternalError(ctx, "%s", error.what());
                                  }
                              },
                              "readAsset", 2));
        JS_SetPropertyStr(context, native.get(), "runtimeVersion", JS_NewUint32(context, canton::core::RuntimeVersion));
        JS_SetPropertyStr(context, global.get(), "__CANTON_NATIVE__", native.release());

        ScopedValue performance(context, JS_NewObject(context));
        JS_SetPropertyStr(context, performance.get(), "now", JS_NewCFunction(context, performanceNow, "now", 0));
        JS_SetPropertyStr(context, performance.get(), "measureNow", JS_NewCFunction(context, performanceMeasureNow, "measureNow", 0));
        JS_SetPropertyStr(context, global.get(), "performance", performance.release());
    }
}
