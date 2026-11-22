#include "QuickJsHotTransport.h"
#include "QuickJsValueReader.h"

#include <atomic>
#include <filesystem>
#include <stdexcept>
#include <string_view>

namespace canton::quickjs {
    namespace {
        bool startsWithDotSpecifier(std::string_view specifier) {
            return specifier == "." || specifier == ".." || specifier.starts_with("./") || specifier.starts_with("../");
        }

        char* normalizeLive(JSContext* context, const char* moduleBaseName, const char* moduleName, void* opaque) {
            if (opaque == nullptr || moduleName == nullptr) return nullptr;
            std::string name(moduleName);
            if (startsWithDotSpecifier(name)) {
                const std::string base(moduleBaseName ? moduleBaseName : "/");
                name = (std::filesystem::path(base.substr(0, base.find('?'))).parent_path() / name).lexically_normal().generic_string();
            }
            if (!name.starts_with('/')) {
                JS_ThrowReferenceError(context, "unresolved live ESM specifier '%s'", moduleName);
                return nullptr;
            }
            return js_strdup(context, name.c_str());
        }

        JSModuleDef* loadLive(JSContext* context, const char* moduleName, void* opaque) {
            const auto* live = static_cast<const QuickJsRuntimeContext::QuickJsLiveState*>(opaque);
            try {
                if (!live) throw std::runtime_error("no module source installed");
                const auto source = live->sources.find(moduleName);
                if (source == live->sources.end()) throw std::runtime_error("live ESM snapshot has no module");
                const auto& text = source->second.source;
                const JSValue value = JS_Eval(context, text.data(), text.size(), moduleName, JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY);
                ScopedValue compiled(context, value);
                if (JS_IsException(value)) return nullptr;
                if (JS_VALUE_GET_TAG(value) != JS_TAG_MODULE) throw std::runtime_error("payload is not an ESM module");
                auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(value));
                ScopedValue meta(context, JS_GetImportMeta(context, module));
                if (JS_IsException(meta.get()) || JS_SetPropertyStr(context, meta.get(), "url", JS_NewString(context, moduleName)) < 0) return nullptr;
                return module;
            } catch (const std::exception& error) {
                JS_ThrowReferenceError(context, "UI module '%s': %s", moduleName, error.what());
                return nullptr;
            }
        }

        void installLiveSnapshot(QuickJsRuntimeContext& state, const LiveModuleSnapshot& snapshot) {
            if (!state.live) state.live = std::make_unique<QuickJsRuntimeContext::QuickJsLiveState>();
            for (const auto& module : snapshot.modules) state.live->sources.insert_or_assign(module.url, module);
            JS_SetModuleLoaderFunc(state.runtime, &normalizeLive, &loadLive, state.live.get());
        }

        JSValue toJs(JSContext* context, const HotValue& input) {
            return std::visit(
                [&](const auto& value) -> JSValue {
                    using T = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<T, std::monostate>)
                        return JS_NULL;
                    else if constexpr (std::is_same_v<T, bool>)
                        return JS_NewBool(context, value);
                    else if constexpr (std::is_same_v<T, double>)
                        return JS_NewFloat64(context, value);
                    else if constexpr (std::is_same_v<T, std::string>)
                        return JS_NewStringLen(context, value.data(), value.size());
                    else if constexpr (std::is_same_v<T, HotValue::Array>) {
                        auto result = JS_NewArray(context);
                        for (std::uint32_t i = 0; i < value.size(); ++i) JS_SetPropertyUint32(context, result, i, toJs(context, value[i]));
                        return result;
                    } else {
                        auto result = JS_NewObject(context);
                        for (const auto& [key, item] : value) JS_SetPropertyStr(context, result, key.c_str(), toJs(context, item));
                        return result;
                    }
                },
                input.value);
        }

        bool fromJs(JSContext* context, JSValueConst value, HotValue& output, unsigned depth = 0) {
            if (depth > 32) return false;
            if (JS_IsNull(value) || JS_IsUndefined(value)) return true;
            if (JS_IsBool(value))
                output.value = JS_ToBool(context, value) != 0;
            else if (JS_IsNumber(value)) {
                double number = 0;
                if (JS_ToFloat64(context, &number, value) < 0) return false;
                output.value = number;
            } else if (JS_IsString(value)) {
                const char* string = JS_ToCString(context, value);
                if (!string) return false;
                output.value = std::string(string);
                JS_FreeCString(context, string);
            } else if (JS_IsArray(value)) {
                ScopedValue length(context, JS_GetPropertyStr(context, value, "length"));
                std::uint32_t size = 0;
                if (JS_ToUint32(context, &size, length.get()) < 0 || size > 100000) return false;
                HotValue::Array array(size);
                for (std::uint32_t i = 0; i < size; ++i) {
                    ScopedValue item(context, JS_GetPropertyUint32(context, value, i));
                    if (!fromJs(context, item.get(), array[i], depth + 1)) return false;
                }
                output.value = std::move(array);
            } else if (JS_IsObject(value) && !JS_IsFunction(context, value)) {
                JSPropertyEnum* properties = nullptr;
                std::uint32_t count = 0;
                if (JS_GetOwnPropertyNames(context, &properties, &count, value, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) return false;
                HotValue::Object object;
                bool ok = true;
                for (std::uint32_t i = 0; i < count && ok; ++i) {
                    const char* name = JS_AtomToCString(context, properties[i].atom);
                    ScopedValue item(context, JS_GetProperty(context, value, properties[i].atom));
                    HotValue child;
                    ok = name && fromJs(context, item.get(), child, depth + 1);
                    if (ok) object.emplace(name, std::move(child));
                    JS_FreeCString(context, name);
                }
                JS_FreePropertyEnum(context, properties, count);
                if (!ok) return false;
                output.value = std::move(object);
            } else
                return false;
            return true;
        }
    }

    void installHotTransport(JSContext* context) {
        static std::atomic<std::uint64_t> nextSession{1};
        auto& state = *static_cast<QuickJsRuntimeContext*>(JS_GetContextOpaque(context));
        if (!state.live) state.live = std::make_unique<QuickJsRuntimeContext::QuickJsLiveState>();
        state.live->session = std::to_string(nextSession.fetch_add(1));
        ScopedValue global(context, JS_GetGlobalObject(context));
        ScopedValue transport(context, JS_NewObject(context));
        JS_SetPropertyStr(context, transport.get(), "session", JS_NewString(context, state.live->session.c_str()));
        JS_SetPropertyStr(context, transport.get(), "send",
                          JS_NewCFunction(
                              context,
                              [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
                                  if (argc < 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "HMR send requires an event name");
                                  const char* event = JS_ToCString(ctx, argv[0]);
                                  if (!event) return JS_EXCEPTION;
                                  HotMessage message;
                                  message.type = "custom";
                                  message.event = event;
                                  JS_FreeCString(ctx, event);
                                  if (argc > 1 && !fromJs(ctx, argv[1], message.data)) return JS_ThrowTypeError(ctx, "HMR custom data must be an acyclic value tree");
                                  auto& runtime = *static_cast<QuickJsRuntimeContext*>(JS_GetContextOpaque(ctx));
                                  if (runtime.live) runtime.live->messages.push_back(std::move(message));
                                  return JS_UNDEFINED;
                              },
                              "send", 2));
        JS_SetPropertyStr(context, transport.get(), "reload",
                          JS_NewCFunction(
                              context,
                              [](JSContext* ctx, JSValueConst, int, JSValueConst*) -> JSValue {
                                  QuickJsDiagnosticAction action;
                                  action.kind = QuickJsDiagnosticActionKind::RequestReload;
                                  static_cast<QuickJsRuntimeContext*>(JS_GetContextOpaque(ctx))->diagnosticActions.push_back(std::move(action));
                                  return JS_UNDEFINED;
                              },
                              "reload", 0));
        JS_SetPropertyStr(context, transport.get(), "report",
                          JS_NewCFunction(
                              context,
                              [](JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) -> JSValue {
                                  QuickJsToastRequest toast;
                                  toast.level = core::LogLevel::Error;
                                  toast.tag = "HotTransport";
                                  const char* text = argc ? JS_ToCString(ctx, argv[0]) : nullptr;
                                  toast.title = "热更新失败";
                                  toast.content = text ? text : "热更新失败";
                                  JS_FreeCString(ctx, text);
                                  static_cast<QuickJsRuntimeContext*>(JS_GetContextOpaque(ctx))->recordToast(std::move(toast));
                                  return JS_UNDEFINED;
                              },
                              "report", 1));
        JS_SetPropertyStr(context, global.get(), "__CANTON_HOT_TRANSPORT__", JS_DupValue(context, transport.get()));
    }

    JSValue hotMessageValue(JSContext* context, const HotMessage& message) {
        HotValue::Array updates;
        for (const auto& update : message.updates) updates.push_back({HotValue::Object{{"type", {update.type}}, {"path", {update.path}}, {"acceptedPath", {update.acceptedPath}}, {"timestamp", {update.timestamp}}, {"explicitImportRequired", {update.explicitImportRequired}}, {"firstInvalidatedBy", {update.firstInvalidatedBy}}}});
        HotValue::Array paths;
        for (const auto& path : message.paths) paths.push_back({path});
        return toJs(context, {HotValue::Object{{"type", {message.type}}, {"updates", {std::move(updates)}}, {"paths", {std::move(paths)}}, {"event", {message.event}}, {"data", message.data}}});
    }

    ScriptExecutionResult QuickJsScriptHost::executeLiveModules(const LiveModuleSnapshot& snapshot) {
        return runModuleTask(__func__, true, [&](QuickJsRuntimeContext& state, std::string&) {
            installLiveSnapshot(state, snapshot);
            installHotTransport(state.context);
            auto* module = loadLive(state.context, snapshot.entry.c_str(), state.live.get());
            if (!module) return false;
            const auto moduleVal = JS_MKPTR(JS_TAG_MODULE, module);
            if (JS_ResolveModule(state.context, moduleVal) < 0) return false;
            ScopedValue result(state.context, JS_EvalFunction(state.context, JS_DupValue(state.context, moduleVal)));
            return !JS_IsException(result.get());
        });
    }

    CallbackInvokeResult QuickJsScriptHost::applyHotUpdate(const LiveModuleSnapshot& snapshot, const HotMessage& message) {
        auto* current = runtimeContext();
        if (!current || !current->context) return {false, "HMR runtime 尚未初始化"};
        const auto executed = runModuleTask(__func__, false, [&](QuickJsRuntimeContext& state, std::string& customError) {
            if (message.event == "canton:import-ready") {
                const auto* data = std::get_if<HotValue::Object>(&message.data.value);
                if (!data || !data->contains("session") || !std::holds_alternative<std::string>(data->at("session").value) || !state.live || std::get<std::string>(data->at("session").value) != state.live->session) return true;
            }
            installLiveSnapshot(state, snapshot);
            ScopedValue global(state.context, JS_GetGlobalObject(state.context));
            ScopedValue receive(state.context, JS_GetPropertyStr(state.context, global.get(), "__CANTON_HOT_RECEIVE__"));
            if (!JS_IsFunction(state.context, receive.get())) {
                customError = "HMR runtime 未安装";
                return false;
            }
            ScopedValue argument(state.context, hotMessageValue(state.context, message));
            JSValueConst argv[] = {argument.get()};
            ScopedValue result(state.context, JS_Call(state.context, receive.get(), JS_UNDEFINED, 1, argv));
            return !JS_IsException(result.get());
        });
        return {executed.ok, executed.error};
    }

    std::vector<HotMessage> QuickJsScriptHost::takeHotMessages() {
        auto* state = runtimeContext();
        return state && state->live ? std::exchange(state->live->messages, {}) : std::vector<HotMessage>{};
    }
}
