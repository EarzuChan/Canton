#include "QuickJsPlatformApi.h"

#include "QuickJsRuntimeContext.h"
#include "QuickJsValueReader.h"

#include <core/Clock.h>

#include <cmath>
#include <cstring>
#include <iterator>
#include <stdexcept>

namespace canton::quickjs {
    namespace {
        QuickJsRuntimeContext* runtimeOf(JSContext* context) {
            return static_cast<QuickJsRuntimeContext*>(JS_GetContextOpaque(context));
        }

        std::string readStringProp(JSContext* ctx, QuickJsValueReader& reader, JSValueConst obj, const char* key, std::string_view fallback = {}) {
            if (!JS_IsObject(obj)) return std::string(fallback);
            ScopedValue v(ctx, JS_GetPropertyStr(ctx, obj, key));
            if (JS_IsUndefined(v.get()) || JS_IsNull(v.get())) return std::string(fallback);
            return reader.toString(v.get());
        }

        bool readBoolProp(JSContext* ctx, JSValueConst obj, const char* key, bool fallback) {
            if (!JS_IsObject(obj)) return fallback;
            ScopedValue v(ctx, JS_GetPropertyStr(ctx, obj, key));
            if (JS_IsUndefined(v.get()) || JS_IsNull(v.get())) return fallback;
            return JS_ToBool(ctx, v.get()) > 0;
        }

        double readNumberProp(JSContext* ctx, JSValueConst obj, const char* key, double fallback) {
            if (!JS_IsObject(obj)) return fallback;
            ScopedValue v(ctx, JS_GetPropertyStr(ctx, obj, key));
            if (JS_IsUndefined(v.get()) || JS_IsNull(v.get())) return fallback;
            double out = fallback;
            if (JS_ToFloat64(ctx, &out, v.get()) < 0 || !std::isfinite(out)) return fallback;
            return out;
        }

        std::string readByteLike(JSContext* ctx, JSValueConst value) {
            if (JS_IsUndefined(value) || JS_IsNull(value)) return {};
            if (JS_IsString(value)) {
                size_t len = 0;
                const char* s = JS_ToCStringLen(ctx, &len, value);
                if (!s) return {};
                std::string out(s, len);
                JS_FreeCString(ctx, s);
                return out;
            }
            size_t byteOffset = 0, byteLength = 0, bytesPerElem = 0;
            ScopedValue ab(ctx, JS_GetTypedArrayBuffer(ctx, value, &byteOffset, &byteLength, &bytesPerElem));
            if (!JS_IsException(ab.get())) {
                size_t abSize = 0;
                const auto* ptr = JS_GetArrayBuffer(ctx, &abSize, ab.get());
                if (ptr && byteOffset + byteLength <= abSize) {
                    return std::string(reinterpret_cast<const char*>(ptr + byteOffset), byteLength);
                }
            } else {
                JS_FreeValue(ctx, JS_GetException(ctx));
            }
            size_t abSize = 0;
            if (const auto* ptr = JS_GetArrayBuffer(ctx, &abSize, value)) {
                return std::string(reinterpret_cast<const char*>(ptr), abSize);
            }
            return {};
        }

        JSValue scheduleTimerImpl(JSContext* ctx, int argc, JSValueConst* argv, bool repeating) {
            auto* self = runtimeOf(ctx);
            if (!self) return JS_ThrowInternalError(ctx, "QuickJS 运行时不可用");
            if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
                return JS_ThrowTypeError(ctx, "定时器回调必须是函数，不支持字符串代码求值");
            }
            double delayMs = 0.0;
            if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
                if (JS_ToFloat64(ctx, &delayMs, argv[1]) < 0) return JS_EXCEPTION;
                if (!std::isfinite(delayMs) || delayMs < 0.0) delayMs = 0.0;
            }

            std::uint64_t id = 0;
            if (self->platform && self->platform->scheduleTimer) {
                id = self->platform->scheduleTimer(delayMs, repeating);
            } else {
                id = self->nextFallbackTimerId++;
                const double now = core::monotonicMillis();
                self->fallbackTimerDue[id] = now + delayMs;
            }

            QuickJsRuntimeContext::TimerEntry entry;
            entry.id = id;
            entry.repeating = repeating;
            entry.intervalMs = delayMs;
            entry.callback = JS_DupValue(ctx, argv[0]);
            for (int i = 2; i < argc; ++i) {
                entry.args.push_back(JS_DupValue(ctx, argv[i]));
            }
            self->timers.emplace(id, std::move(entry));
            if (self->wakeOwner) self->wakeOwner();
            return JS_NewFloat64(ctx, static_cast<double>(id));
        }

        JSValue jsSetTimeout(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            return scheduleTimerImpl(ctx, argc, argv, false);
        }

        JSValue jsSetInterval(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            return scheduleTimerImpl(ctx, argc, argv, true);
        }

        JSValue jsClearTimer(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self || argc < 1 || JS_IsUndefined(argv[0]) || JS_IsNull(argv[0])) return JS_UNDEFINED;
            double raw = 0.0;
            if (JS_ToFloat64(ctx, &raw, argv[0]) < 0) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                return JS_UNDEFINED;
            }
            if (!std::isfinite(raw) || raw < 1.0) return JS_UNDEFINED;
            const auto id = static_cast<std::uint64_t>(raw);
            if (const auto it = self->timers.find(id); it != self->timers.end()) {
                JS_FreeValue(ctx, it->second.callback);
                for (auto& arg : it->second.args) JS_FreeValue(ctx, arg);
                self->timers.erase(it);
            }
            self->fallbackTimerDue.erase(id);
            if (self->platform && self->platform->cancelTimer) {
                self->platform->cancelTimer(id);
            }
            return JS_UNDEFINED;
        }

        JSValue microtaskJob(JSContext* ctx, int argc, JSValueConst* argv) {
            if (argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_UNDEFINED;
            return JS_Call(ctx, argv[0], JS_UNDEFINED, 0, nullptr);
        }

        JSValue jsQueueMicrotask(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            if (argc < 1 || !JS_IsFunction(ctx, argv[0])) {
                return JS_ThrowTypeError(ctx, "queueMicrotask 需要函数参数");
            }
            JSValueConst args[1] = {argv[0]};
            if (JS_EnqueueJob(ctx, microtaskJob, 1, args) < 0) return JS_EXCEPTION;
            return JS_UNDEFINED;
        }

        JSValue nativeAssetExists(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self || !self->assetExistsChecker) return JS_ThrowTypeError(ctx, "当前 App 不提供资产存在性查询");
            if (argc != 1 || !JS_IsString(argv[0]) || self->assetExistsRequests.size() >= 32) {
                return JS_ThrowTypeError(ctx, "资产查询参数无效或超过并发预算");
            }
            size_t len = 0;
            const char* raw = JS_ToCStringLen(ctx, &len, argv[0]);
            if (!raw) return JS_EXCEPTION;
            const std::string ref(raw, len);
            JS_FreeCString(ctx, raw);
            if (ref.empty() || ref.size() > 4096 || ref.find('\0') != std::string::npos || ref.starts_with("data:")) {
                return JS_ThrowTypeError(ctx, "资产地址无效");
            }
            try {
                auto fut = self->assetExistsChecker(ref, self->wakeOwner);
                JSValue callbacks[2];
                auto promise = JS_NewPromiseCapability(ctx, callbacks);
                if (JS_IsException(promise)) return promise;
                self->assetExistsRequests.push_back({std::move(fut), callbacks[0], callbacks[1]});
                return promise;
            } catch (const std::exception& error) {
                return JS_ThrowInternalError(ctx, "%s", error.what());
            }
        }

        JSValue nativeGetGraphicsEnvironment(JSContext* ctx, JSValueConst, int, JSValueConst*) {
            auto* self = runtimeOf(ctx);
            if (!self) return JS_UNDEFINED;
            return QuickJsPlatformApi::makeGraphicsEnvObject(ctx, *self);
        }

        JSValue nativeOnGraphicsEnvironmentChanged(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self) return JS_UNDEFINED;
            JS_FreeValue(ctx, self->onGraphicsEnvChanged);
            if (argc >= 1 && JS_IsFunction(ctx, argv[0])) {
                self->onGraphicsEnvChanged = JS_DupValue(ctx, argv[0]);
            } else {
                self->onGraphicsEnvChanged = JS_UNDEFINED;
            }
            return JS_UNDEFINED;
        }

        JSValue nativeGetLaunchRequest(JSContext* ctx, JSValueConst, int, JSValueConst*) {
            auto* self = runtimeOf(ctx);
            if (!self) return JS_UNDEFINED;
            core::LaunchRequest req;
            if (self->platform && self->platform->getLaunchRequest) {
                req = self->platform->getLaunchRequest();
            }
            return QuickJsPlatformApi::makeLaunchRequestObject(ctx, req);
        }

        JSValue nativeOnSecondInstance(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self) return JS_UNDEFINED;
            JS_FreeValue(ctx, self->onSecondInstance);
            if (argc >= 1 && JS_IsFunction(ctx, argv[0])) {
                self->onSecondInstance = JS_DupValue(ctx, argv[0]);
                // 若注册前已有排队的二次唤醒载荷，立即通知唤醒以便按序补发
                if (!self->bufferedSecondInstances.empty() && self->wakeOwner) self->wakeOwner();
            } else {
                self->onSecondInstance = JS_UNDEFINED;
            }
            return JS_UNDEFINED;
        }

        JSValue nativeGetKnownPath(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self || !self->platform || !self->platform->getKnownPath) {
                return JS_ThrowInternalError(ctx, "当前运行时未挂载路径解析能力");
            }
            if (argc < 1 || !JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "getKnownPath 需要目录种类名");
            QuickJsValueReader reader(ctx);
            const auto kind = reader.toString(argv[0]);
            try {
                const auto path = self->platform->getKnownPath(kind);
                return JS_NewStringLen(ctx, path.data(), path.size());
            } catch (const std::exception& error) {
                return JS_ThrowInternalError(ctx, "%s", error.what());
            }
        }

        JSValue nativeFsAsync(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self || !self->platform || !self->platform->fsAsync) {
                return JS_ThrowInternalError(ctx, "当前运行时未挂载异步文件系统");
            }
            if (argc < 2 || !JS_IsString(argv[0]) || !JS_IsString(argv[1])) {
                return JS_ThrowTypeError(ctx, "fsAsync 需要操作名和路径");
            }
            QuickJsValueReader reader(ctx);
            const auto op = reader.toString(argv[0]);
            const auto path = reader.toString(argv[1]);
            const auto path2 = (argc >= 3 && JS_IsString(argv[2])) ? reader.toString(argv[2]) : std::string{};
            const auto bytes = (argc >= 4) ? readByteLike(ctx, argv[3]) : std::string{};
            const bool flag = (argc >= 5) ? (JS_ToBool(ctx, argv[4]) > 0) : false;

            JSValue callbacks[2];
            auto promise = JS_NewPromiseCapability(ctx, callbacks);
            if (JS_IsException(promise)) return promise;
            try {
                auto fut = self->platform->fsAsync(op, path, path2, bytes, flag, self->wakeOwner);
                self->fsRequests.push_back({op, std::move(fut), callbacks[0], callbacks[1]});
                return promise;
            } catch (const std::exception& error) {
                JS_FreeValue(ctx, callbacks[0]);
                JS_FreeValue(ctx, callbacks[1]);
                JS_FreeValue(ctx, promise);
                return JS_ThrowInternalError(ctx, "%s", error.what());
            }
        }

        NetBridgeRequest parseNetRequest(JSContext* ctx, JSValueConst obj) {
            QuickJsValueReader reader(ctx);
            NetBridgeRequest req;
            req.method = readStringProp(ctx, reader, obj, "method", "GET");
            req.url = readStringProp(ctx, reader, obj, "url");
            req.timeoutMs = static_cast<int>(std::lround(readNumberProp(ctx, obj, "timeoutMs", 15000.0)));
            const double maxB = readNumberProp(ctx, obj, "maxBytes", static_cast<double>(32 * 1024 * 1024));
            req.maxBytes = maxB > 0 ? static_cast<std::size_t>(maxB) : 0;
            req.followRedirects = readBoolProp(ctx, obj, "followRedirects", true);
            req.maxRedirects = static_cast<int>(std::lround(readNumberProp(ctx, obj, "maxRedirects", 10.0)));
            req.useSystemProxy = readBoolProp(ctx, obj, "useSystemProxy", true);

            ScopedValue bodyVal(ctx, JS_GetPropertyStr(ctx, obj, "body"));
            if (!JS_IsUndefined(bodyVal.get()) && !JS_IsNull(bodyVal.get())) {
                req.body = readByteLike(ctx, bodyVal.get());
            }

            ScopedValue headersVal(ctx, JS_GetPropertyStr(ctx, obj, "headers"));
            if (JS_IsArray(headersVal.get())) {
                ScopedValue lenVal(ctx, JS_GetPropertyStr(ctx, headersVal.get(), "length"));
                std::uint32_t len = 0;
                if (JS_ToUint32(ctx, &len, lenVal.get()) == 0) {
                    for (std::uint32_t i = 0; i < len; ++i) {
                        ScopedValue pair(ctx, JS_GetPropertyUint32(ctx, headersVal.get(), i));
                        if (!JS_IsArray(pair.get())) continue;
                        ScopedValue k(ctx, JS_GetPropertyUint32(ctx, pair.get(), 0));
                        ScopedValue v(ctx, JS_GetPropertyUint32(ctx, pair.get(), 1));
                        req.headers.emplace_back(reader.toString(k.get()), reader.toString(v.get()));
                    }
                }
            }
            return req;
        }

        JSValue nativeNetRequest(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self || !self->platform || !self->platform->netRequestAsync) {
                return JS_ThrowInternalError(ctx, "当前运行时未挂载网络能力");
            }
            if (argc < 2 || !JS_IsObject(argv[1])) return JS_ThrowTypeError(ctx, "netRequest 需要 reqId 和选项");
            double rawId = 0;
            if (JS_ToFloat64(ctx, &rawId, argv[0]) < 0) return JS_EXCEPTION;
            const auto reqId = static_cast<std::uint64_t>(rawId);
            auto req = parseNetRequest(ctx, argv[1]);

            JSValue callbacks[2];
            auto promise = JS_NewPromiseCapability(ctx, callbacks);
            if (JS_IsException(promise)) return promise;
            try {
                auto fut = self->platform->netRequestAsync(reqId, std::move(req), self->wakeOwner);
                self->netRequests.push_back({reqId, false, std::move(fut), callbacks[0], callbacks[1], JS_UNDEFINED, nullptr});
                return promise;
            } catch (const std::exception& error) {
                JS_FreeValue(ctx, callbacks[0]);
                JS_FreeValue(ctx, callbacks[1]);
                JS_FreeValue(ctx, promise);
                return JS_ThrowInternalError(ctx, "%s", error.what());
            }
        }

        JSValue nativeNetDownload(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self || !self->platform || !self->platform->netDownloadAsync) {
                return JS_ThrowInternalError(ctx, "当前运行时未挂载下载能力");
            }
            if (argc < 3 || !JS_IsObject(argv[1]) || !JS_IsString(argv[2])) {
                return JS_ThrowTypeError(ctx, "netDownload 需要 reqId、选项和目标路径");
            }
            double rawId = 0;
            if (JS_ToFloat64(ctx, &rawId, argv[0]) < 0) return JS_EXCEPTION;
            const auto reqId = static_cast<std::uint64_t>(rawId);
            auto req = parseNetRequest(ctx, argv[1]);
            QuickJsValueReader reader(ctx);
            const auto destPath = reader.toString(argv[2]);
            JSValue onProgressFn = (argc >= 4 && JS_IsFunction(ctx, argv[3])) ? JS_DupValue(ctx, argv[3]) : JS_UNDEFINED;

            auto progressInbox = std::make_shared<QuickJsRuntimeContext::NetProgressInbox>();
            JSValue callbacks[2];
            auto promise = JS_NewPromiseCapability(ctx, callbacks);
            if (JS_IsException(promise)) {
                JS_FreeValue(ctx, onProgressFn);
                return promise;
            }
            try {
                auto wake = self->wakeOwner;
                auto fut = self->platform->netDownloadAsync(
                    reqId,
                    std::move(req),
                    destPath,
                    [progressInbox, wake](NetBridgeProgress p) {
                        {
                            const std::scoped_lock lock(progressInbox->mutex);
                            progressInbox->latest = p;
                            progressInbox->dirty = true;
                        }
                        if (wake) wake();
                    },
                    self->wakeOwner);
                self->netRequests.push_back({reqId, true, std::move(fut), callbacks[0], callbacks[1], onProgressFn, std::move(progressInbox)});
                return promise;
            } catch (const std::exception& error) {
                JS_FreeValue(ctx, onProgressFn);
                JS_FreeValue(ctx, callbacks[0]);
                JS_FreeValue(ctx, callbacks[1]);
                JS_FreeValue(ctx, promise);
                return JS_ThrowInternalError(ctx, "%s", error.what());
            }
        }

        JSValue nativeNetCancel(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self || !self->platform || !self->platform->netCancel || argc < 1) return JS_UNDEFINED;
            double rawId = 0;
            if (JS_ToFloat64(ctx, &rawId, argv[0]) < 0) {
                JS_FreeValue(ctx, JS_GetException(ctx));
                return JS_UNDEFINED;
            }
            self->platform->netCancel(static_cast<std::uint64_t>(rawId));
            return JS_UNDEFINED;
        }

        std::vector<core::TrayMenuItemSpec> parseTrayMenuItems(JSContext* ctx, QuickJsValueReader& reader, JSValueConst arr) {
            std::vector<core::TrayMenuItemSpec> out;
            if (!JS_IsArray(arr)) return out;
            ScopedValue lenVal(ctx, JS_GetPropertyStr(ctx, arr, "length"));
            std::uint32_t len = 0;
            if (JS_ToUint32(ctx, &len, lenVal.get()) != 0) return out;
            out.reserve(len);
            for (std::uint32_t i = 0; i < len; ++i) {
                ScopedValue itemVal(ctx, JS_GetPropertyUint32(ctx, arr, i));
                if (!JS_IsObject(itemVal.get())) continue;
                core::TrayMenuItemSpec item;
                const auto kind = readStringProp(ctx, reader, itemVal.get(), "kind", "item");
                if (kind == "separator")
                    item.kind = core::TrayMenuItemKind::Separator;
                else if (kind == "checkbox")
                    item.kind = core::TrayMenuItemKind::Checkbox;
                else if (kind == "radio")
                    item.kind = core::TrayMenuItemKind::Radio;
                else if (kind == "submenu")
                    item.kind = core::TrayMenuItemKind::SubMenu;
                else
                    item.kind = core::TrayMenuItemKind::Item;
                item.commandId = static_cast<std::uint32_t>(std::max(0.0, readNumberProp(ctx, itemVal.get(), "commandId", 0.0)));
                item.label = readStringProp(ctx, reader, itemVal.get(), "label");
                item.enabled = readBoolProp(ctx, itemVal.get(), "enabled", true);
                item.checked = readBoolProp(ctx, itemVal.get(), "checked", false);
                if (item.kind == core::TrayMenuItemKind::SubMenu) {
                    ScopedValue childrenVal(ctx, JS_GetPropertyStr(ctx, itemVal.get(), "children"));
                    item.children = parseTrayMenuItems(ctx, reader, childrenVal.get());
                }
                out.push_back(std::move(item));
            }
            return out;
        }

        JSValue nativeTrayUpsert(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self || argc < 1 || !JS_IsObject(argv[0])) return JS_ThrowTypeError(ctx, "trayUpsert 需要配置对象");
            if (!self->platform || !self->platform->upsertTray) return JS_UNDEFINED;
            QuickJsValueReader reader(ctx);
            core::TrayIconSpec spec;
            spec.id = readStringProp(ctx, reader, argv[0], "id", "default");
            spec.guid = readStringProp(ctx, reader, argv[0], "guid");
            spec.icon = readStringProp(ctx, reader, argv[0], "icon");
            spec.tooltip = readStringProp(ctx, reader, argv[0], "tooltip");
            spec.visible = readBoolProp(ctx, argv[0], "visible", true);
            self->platform->upsertTray(spec);
            return JS_UNDEFINED;
        }

        JSValue nativeTrayRemove(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self || argc < 1 || !JS_IsString(argv[0])) return JS_UNDEFINED;
            if (!self->platform || !self->platform->removeTray) return JS_UNDEFINED;
            QuickJsValueReader reader(ctx);
            self->platform->removeTray(reader.toString(argv[0]));
            return JS_UNDEFINED;
        }

        JSValue nativeTrayShowSystemMenu(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self || argc < 3 || !JS_IsArray(argv[0])) return JS_ThrowTypeError(ctx, "trayShowSystemMenu 需要菜单数组和屏幕坐标");
            if (!self->platform || !self->platform->showSystemTrayMenu) return JS_NewUint32(ctx, 0);
            QuickJsValueReader reader(ctx);
            auto items = parseTrayMenuItems(ctx, reader, argv[0]);
            double x = 0, y = 0;
            (void)JS_ToFloat64(ctx, &x, argv[1]);
            (void)JS_ToFloat64(ctx, &y, argv[2]);
            const auto cmd = self->platform->showSystemTrayMenu(items, static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)));
            return JS_NewUint32(ctx, cmd);
        }

        JSValue nativeTrayQueryAnchor(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self || argc < 1 || !JS_IsString(argv[0])) return JS_UNDEFINED;
            core::TrayAnchorRect anchor;
            if (self->platform && self->platform->queryTrayAnchor) {
                QuickJsValueReader reader(ctx);
                anchor = self->platform->queryTrayAnchor(reader.toString(argv[0]));
            }
            return QuickJsPlatformApi::makeTrayAnchorObject(ctx, anchor);
        }

        JSValue nativeOnTrayEvent(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self) return JS_UNDEFINED;
            JS_FreeValue(ctx, self->onTrayEvent);
            self->onTrayEvent = (argc >= 1 && JS_IsFunction(ctx, argv[0])) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
            return JS_UNDEFINED;
        }

        JSValue nativeSetWindowTaskbarProgress(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self || argc < 2) return JS_UNDEFINED;
            if (!self->platform || !self->platform->setWindowTaskbarProgress) return JS_UNDEFINED;
            std::uint32_t winId = 0;
            if (JS_ToUint32(ctx, &winId, argv[0]) != 0) return JS_EXCEPTION;
            core::TaskbarProgressSpec spec;
            if (JS_IsObject(argv[1])) {
                QuickJsValueReader reader(ctx);
                const auto state = readStringProp(ctx, reader, argv[1], "state", "none");
                if (state == "indeterminate")
                    spec.state = core::TaskbarProgressState::Indeterminate;
                else if (state == "normal")
                    spec.state = core::TaskbarProgressState::Normal;
                else if (state == "paused")
                    spec.state = core::TaskbarProgressState::Paused;
                else if (state == "error")
                    spec.state = core::TaskbarProgressState::Error;
                else
                    spec.state = core::TaskbarProgressState::None;
                spec.value = readNumberProp(ctx, argv[1], "value", 0.0);
            }
            self->platform->setWindowTaskbarProgress(winId, spec);
            return JS_UNDEFINED;
        }

        JSValue nativeSetWindowTaskbarBadge(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self || argc < 2) return JS_UNDEFINED;
            if (!self->platform || !self->platform->setWindowTaskbarBadge) return JS_UNDEFINED;
            std::uint32_t winId = 0;
            if (JS_ToUint32(ctx, &winId, argv[0]) != 0) return JS_EXCEPTION;
            std::optional<core::TaskbarBadgeSpec> spec;
            if (JS_IsObject(argv[1])) {
                QuickJsValueReader reader(ctx);
                core::TaskbarBadgeSpec badge;
                badge.icon = readStringProp(ctx, reader, argv[1], "icon");
                badge.label = readStringProp(ctx, reader, argv[1], "label");
                if (!badge.icon.empty()) spec = std::move(badge);
            }
            self->platform->setWindowTaskbarBadge(winId, spec);
            return JS_UNDEFINED;
        }

        JSValue nativeSetWindowTaskbarActions(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self || argc < 2) return JS_UNDEFINED;
            if (!self->platform || !self->platform->setWindowTaskbarActions) return JS_UNDEFINED;
            std::uint32_t winId = 0;
            if (JS_ToUint32(ctx, &winId, argv[0]) != 0) return JS_EXCEPTION;
            std::vector<core::TaskbarActionSpec> actions;
            if (JS_IsArray(argv[1])) {
                QuickJsValueReader reader(ctx);
                ScopedValue lenVal(ctx, JS_GetPropertyStr(ctx, argv[1], "length"));
                std::uint32_t len = 0;
                if (JS_ToUint32(ctx, &len, lenVal.get()) == 0) {
                    if (len > 7) return JS_ThrowRangeError(ctx, "taskbarActions 最多支持 7 个缩略图按钮");
                    actions.reserve(len);
                    for (std::uint32_t i = 0; i < len; ++i) {
                        ScopedValue item(ctx, JS_GetPropertyUint32(ctx, argv[1], i));
                        if (!JS_IsObject(item.get())) continue;
                        core::TaskbarActionSpec act;
                        act.id = readStringProp(ctx, reader, item.get(), "id");
                        act.icon = readStringProp(ctx, reader, item.get(), "icon");
                        act.tooltip = readStringProp(ctx, reader, item.get(), "tooltip");
                        act.enabled = readBoolProp(ctx, item.get(), "enabled", true);
                        act.dismissOnClick = readBoolProp(ctx, item.get(), "dismissOnClick", false);
                        actions.push_back(std::move(act));
                    }
                }
            }
            self->platform->setWindowTaskbarActions(winId, actions);
            return JS_UNDEFINED;
        }

        JSValue nativeOnWindowTaskbarAction(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self) return JS_UNDEFINED;
            JS_FreeValue(ctx, self->onTaskbarAction);
            self->onTaskbarAction = (argc >= 1 && JS_IsFunction(ctx, argv[0])) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
            return JS_UNDEFINED;
        }

        JSValue nativeMediaSessionUpdate(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self || !self->platform || !self->platform->updateMediaSession) return JS_UNDEFINED;
            core::MediaSessionSpec spec;
            if (argc >= 1 && JS_IsObject(argv[0])) {
                QuickJsValueReader reader(ctx);
                spec.active = readBoolProp(ctx, argv[0], "active", true);
                const auto status = readStringProp(ctx, reader, argv[0], "playbackStatus", "closed");
                if (status == "stopped")
                    spec.playbackStatus = core::MediaPlaybackStatus::Stopped;
                else if (status == "playing")
                    spec.playbackStatus = core::MediaPlaybackStatus::Playing;
                else if (status == "paused")
                    spec.playbackStatus = core::MediaPlaybackStatus::Paused;
                else
                    spec.playbackStatus = core::MediaPlaybackStatus::Closed;
                spec.title = readStringProp(ctx, reader, argv[0], "title");
                spec.artist = readStringProp(ctx, reader, argv[0], "artist");
                spec.album = readStringProp(ctx, reader, argv[0], "album");
                spec.thumbnail = readStringProp(ctx, reader, argv[0], "thumbnail");
                spec.canPlay = readBoolProp(ctx, argv[0], "canPlay", false);
                spec.canPause = readBoolProp(ctx, argv[0], "canPause", false);
                spec.canStop = readBoolProp(ctx, argv[0], "canStop", false);
                spec.canNext = readBoolProp(ctx, argv[0], "canNext", false);
                spec.canPrevious = readBoolProp(ctx, argv[0], "canPrevious", false);
            }
            self->platform->updateMediaSession(spec);
            return JS_UNDEFINED;
        }

        JSValue nativeOnMediaButton(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self) return JS_UNDEFINED;
            JS_FreeValue(ctx, self->onMediaButton);
            self->onMediaButton = (argc >= 1 && JS_IsFunction(ctx, argv[0])) ? JS_DupValue(ctx, argv[0]) : JS_UNDEFINED;
            return JS_UNDEFINED;
        }

        JSValue nativeSetJumpList(JSContext* ctx, JSValueConst, int argc, JSValueConst* argv) {
            auto* self = runtimeOf(ctx);
            if (!self || argc < 1 || !JS_IsArray(argv[0])) return JS_ThrowTypeError(ctx, "setJumpList 需要分类数组");
            if (!self->platform || !self->platform->setJumpList) return JS_UNDEFINED;
            QuickJsValueReader reader(ctx);
            std::vector<core::JumpListCategorySpec> categories;
            ScopedValue lenVal(ctx, JS_GetPropertyStr(ctx, argv[0], "length"));
            std::uint32_t len = 0;
            if (JS_ToUint32(ctx, &len, lenVal.get()) == 0) {
                for (std::uint32_t i = 0; i < len; ++i) {
                    ScopedValue catVal(ctx, JS_GetPropertyUint32(ctx, argv[0], i));
                    if (!JS_IsObject(catVal.get())) continue;
                    core::JumpListCategorySpec cat;
                    cat.title = readStringProp(ctx, reader, catVal.get(), "title");
                    ScopedValue itemsVal(ctx, JS_GetPropertyStr(ctx, catVal.get(), "items"));
                    if (JS_IsArray(itemsVal.get())) {
                        ScopedValue iLenVal(ctx, JS_GetPropertyStr(ctx, itemsVal.get(), "length"));
                        std::uint32_t iLen = 0;
                        if (JS_ToUint32(ctx, &iLen, iLenVal.get()) == 0) {
                            for (std::uint32_t j = 0; j < iLen; ++j) {
                                ScopedValue taskVal(ctx, JS_GetPropertyUint32(ctx, itemsVal.get(), j));
                                if (!JS_IsObject(taskVal.get())) continue;
                                core::JumpListTaskSpec task;
                                task.title = readStringProp(ctx, reader, taskVal.get(), "title");
                                task.icon = readStringProp(ctx, reader, taskVal.get(), "icon");
                                task.description = readStringProp(ctx, reader, taskVal.get(), "description");
                                ScopedValue argsVal(ctx, JS_GetPropertyStr(ctx, taskVal.get(), "args"));
                                if (JS_IsArray(argsVal.get())) {
                                    ScopedValue aLenVal(ctx, JS_GetPropertyStr(ctx, argsVal.get(), "length"));
                                    std::uint32_t aLen = 0;
                                    if (JS_ToUint32(ctx, &aLen, aLenVal.get()) == 0) {
                                        for (std::uint32_t k = 0; k < aLen; ++k) {
                                            ScopedValue a(ctx, JS_GetPropertyUint32(ctx, argsVal.get(), k));
                                            task.args.push_back(reader.toString(a.get()));
                                        }
                                    }
                                }
                                cat.items.push_back(std::move(task));
                            }
                        }
                    }
                    categories.push_back(std::move(cat));
                }
            }
            try {
                self->platform->setJumpList(categories);
                return JS_UNDEFINED;
            } catch (const std::exception& error) {
                return JS_ThrowInternalError(ctx, "%s", error.what());
            }
        }

        JSValue nativeClearJumpList(JSContext* ctx, JSValueConst, int, JSValueConst*) {
            auto* self = runtimeOf(ctx);
            if (!self || !self->platform || !self->platform->clearJumpList) return JS_UNDEFINED;
            try {
                self->platform->clearJumpList();
                return JS_UNDEFINED;
            } catch (const std::exception& error) {
                return JS_ThrowInternalError(ctx, "%s", error.what());
            }
        }

        const JSCFunctionListEntry platformApiFunctions[] = {
            JS_CFUNC_DEF("assetExists", 1, nativeAssetExists),
            JS_CFUNC_DEF("getGraphicsEnvironment", 0, nativeGetGraphicsEnvironment),
            JS_CFUNC_DEF("onGraphicsEnvironmentChanged", 1, nativeOnGraphicsEnvironmentChanged),
            JS_CFUNC_DEF("getLaunchRequest", 0, nativeGetLaunchRequest),
            JS_CFUNC_DEF("onSecondInstance", 1, nativeOnSecondInstance),
            JS_CFUNC_DEF("getKnownPath", 1, nativeGetKnownPath),
            JS_CFUNC_DEF("fsAsync", 5, nativeFsAsync),
            JS_CFUNC_DEF("netRequest", 2, nativeNetRequest),
            JS_CFUNC_DEF("netDownload", 4, nativeNetDownload),
            JS_CFUNC_DEF("netCancel", 1, nativeNetCancel),
            JS_CFUNC_DEF("trayUpsert", 1, nativeTrayUpsert),
            JS_CFUNC_DEF("trayRemove", 1, nativeTrayRemove),
            JS_CFUNC_DEF("trayShowSystemMenu", 3, nativeTrayShowSystemMenu),
            JS_CFUNC_DEF("trayQueryAnchor", 1, nativeTrayQueryAnchor),
            JS_CFUNC_DEF("onTrayEvent", 1, nativeOnTrayEvent),
            JS_CFUNC_DEF("setWindowTaskbarProgress", 2, nativeSetWindowTaskbarProgress),
            JS_CFUNC_DEF("setWindowTaskbarBadge", 2, nativeSetWindowTaskbarBadge),
            JS_CFUNC_DEF("setWindowTaskbarActions", 2, nativeSetWindowTaskbarActions),
            JS_CFUNC_DEF("onWindowTaskbarAction", 1, nativeOnWindowTaskbarAction),
            JS_CFUNC_DEF("mediaSessionUpdate", 1, nativeMediaSessionUpdate),
            JS_CFUNC_DEF("onMediaButton", 1, nativeOnMediaButton),
            JS_CFUNC_DEF("setJumpList", 1, nativeSetJumpList),
            JS_CFUNC_DEF("clearJumpList", 0, nativeClearJumpList),
        };

        constexpr const char* kAbortBootstrapJs = R"JS(
(() => {
    const createNamedError = (name, message) => {
        const err = new Error(message);
        err.name = name;
        return err;
    };
    const abortSym = Symbol('Canton.AbortSignal.trigger');
    class AbortSignal {
        #aborted = false;
        #reason = undefined;
        #listeners = [];
        onabort = null;
        get aborted() { return this.#aborted; }
        get reason() { return this.#reason; }
        throwIfAborted() {
            if (this.#aborted) throw this.#reason;
        }
        addEventListener(type, listener, options) {
            if (type !== 'abort' || typeof listener !== 'function') return;
            const once = !!(options && typeof options === 'object' && options.once);
            for (let i = 0; i < this.#listeners.length; i++) {
                if (this.#listeners[i].fn === listener) return;
            }
            this.#listeners.push({ fn: listener, once });
        }
        removeEventListener(type, listener) {
            if (type !== 'abort' || typeof listener !== 'function') return;
            for (let i = 0; i < this.#listeners.length; i++) {
                if (this.#listeners[i].fn === listener) {
                    this.#listeners.splice(i, 1);
                    return;
                }
            }
        }
        [abortSym](reason) {
            if (this.#aborted) return;
            this.#aborted = true;
            this.#reason = reason === undefined ? createNamedError('AbortError', 'The operation was aborted') : reason;
            const snapshot = this.#listeners.slice();
            this.#listeners = this.#listeners.filter(entry => !entry.once);
            if (typeof this.onabort === 'function') {
                this.onabort.call(this);
            }
            for (let i = 0; i < snapshot.length; i++) {
                snapshot[i].fn.call(this);
            }
        }
        static abort(reason) {
            const signal = new AbortSignal();
            signal[abortSym](reason);
            return signal;
        }
        static timeout(ms) {
            const delay = typeof ms === 'number' && Number.isFinite(ms) && ms > 0 ? ms : 0;
            const signal = new AbortSignal();
            setTimeout(() => {
                signal[abortSym](createNamedError('TimeoutError', 'The operation timed out'));
            }, delay);
            return signal;
        }
    }
    class AbortController {
        #signal = new AbortSignal();
        get signal() { return this.#signal; }
        abort(reason) { this.#signal[abortSym](reason); }
    }
    globalThis.AbortSignal = AbortSignal;
    globalThis.AbortController = AbortController;
})();
)JS";
    }

    JSValue QuickJsPlatformApi::makeGraphicsEnvObject(JSContext* ctx, const QuickJsRuntimeContext& runtime) {
        core::GraphicsEnvironmentInfo info;
        if (runtime.platform && runtime.platform->getGraphicsEnvironment) {
            info = runtime.platform->getGraphicsEnvironment();
        }
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "backend", JS_NewString(ctx, "d3d11"));
        JS_SetPropertyStr(ctx, obj, "adapterName", JS_NewStringLen(ctx, info.adapterName.data(), info.adapterName.size()));
        JS_SetPropertyStr(ctx, obj, "isSoftwareRenderer", JS_NewBool(ctx, info.isSoftwareRenderer));
        JS_SetPropertyStr(ctx, obj, "hardwareAccelerated", JS_NewBool(ctx, !info.isSoftwareRenderer));
        JS_SetPropertyStr(ctx, obj, "isRemoteSession", JS_NewBool(ctx, info.isRemoteSession));
        JS_SetPropertyStr(ctx, obj, "systemTransparency", JS_NewBool(ctx, info.systemTransparency));
        JS_SetPropertyStr(ctx, obj, "systemAnimations", JS_NewBool(ctx, info.systemAnimations));
        return obj;
    }

    JSValue QuickJsPlatformApi::makeLaunchRequestObject(JSContext* ctx, const core::LaunchRequest& request) {
        JSValue obj = JS_NewObject(ctx);
        JSValue args = JS_NewArray(ctx);
        for (std::uint32_t i = 0; i < request.args.size(); ++i) {
            const auto& a = request.args[i];
            JS_SetPropertyUint32(ctx, args, i, JS_NewStringLen(ctx, a.data(), a.size()));
        }
        JS_SetPropertyStr(ctx, obj, "args", args);
        JS_SetPropertyStr(ctx, obj, "workingDirectory", JS_NewStringLen(ctx, request.workingDirectory.data(), request.workingDirectory.size()));
        return obj;
    }

    JSValue QuickJsPlatformApi::makeTrayAnchorObject(JSContext* ctx, const core::TrayAnchorRect& anchor) {
        JSValue obj = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, obj, "left", JS_NewInt32(ctx, anchor.left));
        JS_SetPropertyStr(ctx, obj, "top", JS_NewInt32(ctx, anchor.top));
        JS_SetPropertyStr(ctx, obj, "right", JS_NewInt32(ctx, anchor.right));
        JS_SetPropertyStr(ctx, obj, "bottom", JS_NewInt32(ctx, anchor.bottom));
        JS_SetPropertyStr(ctx, obj, "cursorX", JS_NewInt32(ctx, anchor.cursorX));
        JS_SetPropertyStr(ctx, obj, "cursorY", JS_NewInt32(ctx, anchor.cursorY));
        JS_SetPropertyStr(ctx, obj, "workLeft", JS_NewInt32(ctx, anchor.workLeft));
        JS_SetPropertyStr(ctx, obj, "workTop", JS_NewInt32(ctx, anchor.workTop));
        JS_SetPropertyStr(ctx, obj, "workRight", JS_NewInt32(ctx, anchor.workRight));
        JS_SetPropertyStr(ctx, obj, "workBottom", JS_NewInt32(ctx, anchor.workBottom));
        JS_SetPropertyStr(ctx, obj, "dpiScale", JS_NewFloat64(ctx, anchor.dpiScale));
        return obj;
    }

    void QuickJsPlatformApi::install(JSContext* context, JSValue nativeObj, JSValue globalObj, QuickJsRuntimeContext&) {
        JS_SetPropertyFunctionList(context, nativeObj, platformApiFunctions, static_cast<int>(std::size(platformApiFunctions)));

        JS_SetPropertyStr(context, globalObj, "setTimeout", JS_NewCFunction(context, jsSetTimeout, "setTimeout", 2));
        JS_SetPropertyStr(context, globalObj, "clearTimeout", JS_NewCFunction(context, jsClearTimer, "clearTimeout", 1));
        JS_SetPropertyStr(context, globalObj, "setInterval", JS_NewCFunction(context, jsSetInterval, "setInterval", 2));
        JS_SetPropertyStr(context, globalObj, "clearInterval", JS_NewCFunction(context, jsClearTimer, "clearInterval", 1));
        JS_SetPropertyStr(context, globalObj, "queueMicrotask", JS_NewCFunction(context, jsQueueMicrotask, "queueMicrotask", 1));

        ScopedValue boot(context, JS_Eval(context, kAbortBootstrapJs, std::strlen(kAbortBootstrapJs), "<canton-abort-bootstrap>", JS_EVAL_TYPE_GLOBAL));
        if (JS_IsException(boot.get())) {
            JS_FreeValue(context, JS_GetException(context));
        }
    }

}
