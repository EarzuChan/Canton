#include <quickjs/QuickJsScriptHost.h>

#include "QuickJsNativeApi.h"
#include "QuickJsPlatformApi.h"
#include "QuickJsRuntimeContext.h"
#include "QuickJsValueReader.h"
#include "QuickJsDrawReader.h"
#include "QuickJsScriptHeap.h"

#include <core/Clock.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace canton::quickjs {
    namespace {
        std::size_t scriptStackBudget() {
            ULONG_PTR lower = 0;
            ULONG_PTR upper = 0;
            GetCurrentThreadStackLimits(&lower, &upper);
            const auto current = reinterpret_cast<ULONG_PTR>(&lower);
            constexpr std::size_t hostReserve = 256 * 1024;
            if (current <= lower || current - lower <= hostReserve) throw std::runtime_error("Owner 线程没有足够的脚本栈空间");

            return (std::min)(std::size_t{2 * 1024 * 1024}, static_cast<std::size_t>(current - lower - hostReserve));
        }

        struct DrainJobsResult {
            bool ok = true;
            std::string error;
        };

        const char* placementLabel(canton::core::WindowPlacement placement) noexcept {
            switch (placement) {
                case canton::core::WindowPlacement::Normal:
                    return "normal";
                case canton::core::WindowPlacement::Minimized:
                    return "minimized";
                case canton::core::WindowPlacement::Maximized:
                    return "maximized";
                case canton::core::WindowPlacement::Fullscreen:
                    return "fullscreen";
            }
            return "normal";
        }

        bool launchModuleDefaultExport(QuickJsRuntimeContext& state, JSModuleDef* module, std::string& error) {
            ScopedValue ns(state.context, JS_GetModuleNamespace(state.context, module));
            if (JS_IsException(ns.get())) return false;
            ScopedValue defaultExport(state.context, JS_GetPropertyStr(state.context, ns.get(), "default"));
            if (JS_IsException(defaultExport.get())) return false;
            if (JS_IsUndefined(defaultExport.get())) return true;

            ScopedValue global(state.context, JS_GetGlobalObject(state.context));
            ScopedValue launcher(state.context, JS_GetPropertyStr(state.context, global.get(), "__CANTON_LAUNCH_APP__"));
            if (!JS_IsFunction(state.context, launcher.get())) {
                error = "入口模块导出了 default，但未通过 @canton/framework 的 appBuilder 构建应用定义";
                return false;
            }
            JSValueConst argv[] = {defaultExport.get()};
            ScopedValue launched(state.context, JS_Call(state.context, launcher.get(), JS_UNDEFINED, 1, argv));
            return !JS_IsException(launched.get());
        }
    }

    struct QuickJsScriptHost::Impl {
        explicit Impl(ScriptExecutionLimits value) : limits(value) {
            if (limits.semanticMillis <= 0 || limits.visualMillis <= 0 || limits.moduleMillis <= 0) throw std::invalid_argument("脚本任务预算必须为正数");
        }

        const ScriptExecutionLimits limits;
        ScriptHeap heap;
        QuickJsRuntimeContext runtime;

        ~Impl() {
            reset();
        }

        void reset() {
            if (runtime.context != nullptr) {
                TaskBudget budget(*this, "disposeApp");
                if (JS_IsFunction(runtime.context, runtime.disposeApp)) {
                    ScopedValue callback(runtime.context, JS_DupValue(runtime.context, runtime.disposeApp));
                    ScopedValue disposed(runtime.context, JS_Call(runtime.context, callback.get(), JS_UNDEFINED, 0, nullptr));
                    if (JS_IsException(disposed.get())) (void)quickJsExceptionText(runtime.context);
                }
                for (auto* val : {&runtime.prepareFrame, &runtime.completeFrame, &runtime.disposeApp, &runtime.materializeLayout, &runtime.onWindowCloseRequest, &runtime.onWindowGeometryChanged, &runtime.onWindowFocusChanged, &runtime.onWindowDpiChanged}) {
                    JS_FreeValue(runtime.context, *val);
                    *val = JS_UNDEFINED;
                }
                runtime.freePlatformHandles();
                runtime.bufferedSecondInstances.clear();
                for (const auto& prepared : runtime.preparedDrawCallbacks) JS_FreeValue(runtime.context, prepared.callback);
                runtime.preparedDrawCallbacks.clear();
                runtime.painters.reset();
                for (auto& request : runtime.assetRequests) {
                    JS_FreeValue(runtime.context, request.resolve);
                    JS_FreeValue(runtime.context, request.reject);
                }
                runtime.assetRequests.clear();
                JS_FreeValue(runtime.context, runtime.rearrangeCompletion);
                runtime.rearrangeCompletion = JS_UNDEFINED;
                if (runtime.rearrangeSubmission) runtime.rearrangeSubmission->cancelled = true;
                runtime.rearrangeSubmission.reset();
                runtime.submittedRearrange.reset();
                runtime.rearrangeCheckpoint.reset();
                runtime.events.reset(nullptr);
                for (auto& [promise, reason] : runtime.unhandledRejections) {
                    JS_FreeValue(runtime.context, promise);
                    JS_FreeValue(runtime.context, reason);
                }
                runtime.unhandledRejections.clear();
                JS_FreeContext(runtime.context);
                runtime.context = nullptr;
            }
            runtime.events.clearContext();
            if (runtime.runtime != nullptr) {
                JS_FreeRuntime(runtime.runtime);
                runtime.runtime = nullptr;
            }
            heap.reset();
            runtime.rootNodeId = 0;
            runtime.frameRequested = false;
            runtime.focusCommands.clear();
            runtime.scopedFocusCommands.clear();
            runtime.pendingAppConfig.reset();
            runtime.appExitRequested = false;
            runtime.windowMutations.clear();
            runtime.windowCommands.clear();
            runtime.windowTransactions.clear();
            runtime.nodeWindows.clear();
            runtime.framePrepared = false;
            runtime.frameTimeMillis = 0.0;
            runtime.publishedModifiers.clear();
            runtime.modifierInputs.clear();
            runtime.rejectedBindingUpdates = 0;
            runtime.remainingFrameJobs = QuickJsRuntimeContext::maxJobsPerFrame;
            runtime.pendingTransactions = nullptr;
            runtime.nodeTypes.clear();
            runtime.nodeGenerations.clear();
            runtime.hostBindings.clear();
            runtime.bindings.clear();
            runtime.modifierBindings.clear();
            runtime.childrenByNode.clear();
            runtime.parentByNode.clear();
            runtime.moduleLoader.clear();
            runtime.diagnosticToasts.clear();
            runtime.diagnosticActions.clear();
            runtime.live.reset();
        }

        void initialise(QuickJsScriptHost* owner) {
            reset();
            runtime.pendingTransactions = &owner->pendingTransactions_;
            runtime.runtime = JS_NewRuntime2(&ScriptHeap::functions, &heap);
            // 给宿主输入、布局与异常处理保留栈空间，先由引擎报告脚本栈溢出
            if (!runtime.runtime) throw std::bad_alloc();
            JS_SetMaxStackSize(runtime.runtime, scriptStackBudget());
            JS_SetInterruptHandler(
                runtime.runtime,
                [](JSRuntime*, void* opaque) {
                    auto& owner = *static_cast<Impl*>(opaque);
                    if (!owner.taskDepth || std::chrono::steady_clock::now() < owner.deadline) return 0;
                    owner.interrupted = true;
                    return 1;
                },
                this);
            JS_SetModuleLoaderFunc(runtime.runtime, &QuickJsModuleLoader::normalize, &QuickJsModuleLoader::load, &runtime.moduleLoader);
            runtime.context = JS_NewContext(runtime.runtime);
            if (!runtime.context) throw std::bad_alloc();
            JS_SetHostPromiseRejectionTracker(
                runtime.runtime,
                [](JSContext* context, JSValueConst promise, JSValueConst reason, bool handled, void* opaque) {
                    auto& state = *static_cast<QuickJsRuntimeContext*>(opaque);
                    if (handled) {
                        std::erase_if(state.unhandledRejections, [&](const auto& entry) {
                            if (!JS_IsStrictEqual(context, promise, entry.first)) return false;
                            JS_FreeValue(context, entry.first);
                            JS_FreeValue(context, entry.second);
                            return true;
                        });
                    } else
                        state.unhandledRejections.emplace_back(JS_DupValue(context, promise), JS_DupValue(context, reason));
                },
                &runtime);
            runtime.events.reset(runtime.context);
            JS_SetContextOpaque(runtime.context, &runtime);
            runtime.painters = std::make_unique<QuickJsPainterResources>(runtime.context, runtime.painterLoader, *runtime.fonts);
            QuickJsNativeApi::install(runtime.context, runtime);
        }

        struct TaskBudget {
            Impl& owner;

            explicit TaskBudget(Impl& value, const char* name, int milliseconds = 0) : owner(value) {
                if (owner.ownerThread != std::this_thread::get_id()) throw std::runtime_error("QuickJS 只能由创建它的 UI Owner 线程执行");
                if (owner.taskDepth++ == 0) {
                    owner.taskName = name;
                    owner.taskMillis = milliseconds ? milliseconds : owner.limits.semanticMillis;
                    owner.started = std::chrono::steady_clock::now();
                    owner.deadline = owner.started + std::chrono::milliseconds(owner.taskMillis);
                    owner.interrupted = false;
                    owner.initialAllocations = owner.heap.stats().allocations;
                    owner.runtime.remainingFrameJobs = QuickJsRuntimeContext::maxJobsPerFrame;
                }
            }

            ~TaskBudget() {
                if (--owner.taskDepth == 0 && std::getenv("CANTON_TRACE_SCRIPT_TASKS")) core::Log::d("QuickJS任务", owner.taskName, "预算", owner.taskMillis, "耗时", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - owner.started).count(), "中断", owner.interrupted, "分配", owner.heap.stats().allocations - owner.initialAllocations);
            }
        };

        const std::thread::id ownerThread = std::this_thread::get_id();
        unsigned taskDepth = 0;
        std::chrono::steady_clock::time_point deadline;
        std::chrono::steady_clock::time_point started;
        const char* taskName = "";
        int taskMillis = 0;
        bool interrupted = false;
        std::uint64_t initialAllocations = 0;

        std::string exceptionText(JSContext* context) const {
            auto error = quickJsExceptionText(context);
            if (interrupted) error += "\nQuickJS 任务 " + std::string(taskName) + " 超过 " + std::to_string(taskMillis) + "ms 预算（耗时 " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()) + "ms）";
            return error;
        }

        DrainJobsResult drainJobs() {
            // 完成视觉回执后重新建立语义边界，嵌套派发仍继承当前任务预算
            TaskBudget budget(*this, "drainJobs");
            JSContext* jobContext = nullptr;
            while (JS_IsJobPending(runtime.runtime)) {
                if (runtime.remainingFrameJobs == 0) return {false, "JavaScript 语义任务超过检查点数量预算"};
                --runtime.remainingFrameJobs;
                if (JS_ExecutePendingJob(runtime.runtime, &jobContext) < 0) return {false, exceptionText(jobContext != nullptr ? jobContext : runtime.context)};
            }
            if (!runtime.unhandledRejections.empty()) {
                JS_Throw(runtime.context, JS_DupValue(runtime.context, runtime.unhandledRejections.front().second));
                return {false, exceptionText(runtime.context)};
            }
            return {};
        }

        CallbackInvokeResult callDriver(JSValueConst callback, int argc, JSValueConst* argv, const char* name) {
            TaskBudget budget(*this, name);
            if (!runtime.context || !JS_IsFunction(runtime.context, callback)) return {true, {}};
            ScopedValue fn(runtime.context, JS_DupValue(runtime.context, callback));
            ScopedValue result(runtime.context, JS_Call(runtime.context, fn.get(), JS_UNDEFINED, argc, argv));
            if (JS_IsException(result.get())) return {false, exceptionText(runtime.context)};
            const auto drained = drainJobs();
            return {drained.ok, drained.error};
        }
    };

    QuickJsScriptHost::QuickJsScriptHost(ScriptExecutionLimits limits) : impl_(std::make_unique<Impl>(limits)) {}

    void QuickJsScriptHost::setAssetLoader(core::AssetLoader loader) {
        impl_->runtime.assetLoader = std::move(loader);
    }

    void QuickJsScriptHost::setAssetExistsChecker(core::AssetExistsChecker checker) {
        impl_->runtime.assetExistsChecker = std::move(checker);
    }

    void QuickJsScriptHost::setPlatformBridge(std::shared_ptr<PlatformBridge> bridge) {
        impl_->runtime.platform = std::move(bridge);
    }

    QuickJsScriptHost::~QuickJsScriptHost() {
        // 先销毁 QuickJS 上下文（disposeApp / nativeUnmount 会引用 pendingTransactions_）
        impl_.reset();
    }

    void QuickJsScriptHost::setPainterLoader(canton::core::PainterLoader loader) {
        impl_->runtime.painterLoader = std::move(loader);
    }

    const canton::core::PainterLoader& QuickJsScriptHost::painterLoader() const noexcept {
        return impl_->runtime.painterLoader;
    }

    void QuickJsScriptHost::setFontRegistry(std::shared_ptr<core::FontRegistry> fonts) {
        if (impl_->runtime.context) throw std::logic_error("字体注册表须在加载脚本前设置");
        if (!fonts) throw std::invalid_argument("字体注册表不能为空");
        impl_->runtime.fonts = std::move(fonts);
    }

    const std::shared_ptr<core::FontRegistry>& QuickJsScriptHost::fontRegistry() const noexcept {
        return impl_->runtime.fonts;
    }

    void QuickJsScriptHost::setNativePainterRegistry(std::shared_ptr<const core::NativePainterRegistry> painters) {
        if (impl_->runtime.context) throw std::logic_error("绘制注册表须在加载脚本前设置");
        impl_->runtime.nativePainters = std::move(painters);
    }

    void QuickJsScriptHost::setShaderCompiler(std::shared_ptr<const core::ShaderCompiler> compiler) {
        impl_->runtime.shaderCompiler = std::move(compiler);
    }

    std::size_t QuickJsScriptHost::eventSlotCount() const noexcept {
        return impl_ ? impl_->runtime.events.size() : 0;
    }

    std::size_t QuickJsScriptHost::bindingCount() const noexcept {
        return impl_ ? impl_->runtime.bindings.size() : 0;
    }

    std::size_t QuickJsScriptHost::modifierInstanceCount() const noexcept {
        return impl_ ? impl_->runtime.publishedModifiers.size() : 0;
    }

    std::uint64_t QuickJsScriptHost::rejectedBindingUpdates() const noexcept {
        return impl_ ? impl_->runtime.rejectedBindingUpdates : 0;
    }

    ScriptMemoryStats QuickJsScriptHost::memoryStats() const noexcept {
        return impl_ ? impl_->heap.stats() : ScriptMemoryStats{};
    }

    bool QuickJsScriptHost::hasMountedApp() const noexcept {
        if (!impl_ || !impl_->runtime.context) return false;
        const auto& pending = pendingTransactions_.pending();
        return !JS_IsUndefined(impl_->runtime.prepareFrame) || (pending && pending->hasTreeMutations()) || !impl_->runtime.windowMutations.empty() || !impl_->runtime.windowTransactions.empty();
    }

    std::optional<core::AppConfigSpec> QuickJsScriptHost::takeAppConfig() {
        if (!impl_) return std::nullopt;
        return std::exchange(impl_->runtime.pendingAppConfig, std::nullopt);
    }

    bool QuickJsScriptHost::consumeAppExitRequest() noexcept {
        return impl_ && std::exchange(impl_->runtime.appExitRequested, false);
    }

    std::vector<WindowMutation> QuickJsScriptHost::takeWindowMutations() {
        if (!impl_) return {};
        return std::exchange(impl_->runtime.windowMutations, {});
    }

    std::vector<WindowCommand> QuickJsScriptHost::takeWindowCommands() {
        if (!impl_) return {};
        return std::exchange(impl_->runtime.windowCommands, {});
    }

    bool QuickJsScriptHost::hasPendingWindowWork() const noexcept {
        if (!impl_) return false;
        const auto& state = impl_->runtime;
        if (state.pendingAppConfig || state.appExitRequested || !state.windowMutations.empty() || !state.windowCommands.empty() || !state.scopedFocusCommands.empty()) return true;
        for (const auto& [_, queue] : state.windowTransactions)
            if (queue.hasPending()) return true;
        return false;
    }

    std::vector<core::WindowId> QuickJsScriptHost::pendingTransactionWindows() const {
        std::vector<core::WindowId> ids;
        if (!impl_) return ids;
        for (const auto& [windowId, queue] : impl_->runtime.windowTransactions) {
            if (queue.hasPending()) ids.push_back(windowId);
        }
        std::sort(ids.begin(), ids.end());
        return ids;
    }

    bool QuickJsScriptHost::hasWindowTransaction(core::WindowId windowId) const noexcept {
        if (!impl_) return false;
        const auto it = impl_->runtime.windowTransactions.find(windowId);
        return it != impl_->runtime.windowTransactions.end() && it->second.hasPending();
    }

    std::optional<core::MutationTransaction> QuickJsScriptHost::takeWindowTransaction(core::WindowId windowId) noexcept {
        if (!impl_) return std::nullopt;
        const auto it = impl_->runtime.windowTransactions.find(windowId);
        if (it == impl_->runtime.windowTransactions.end()) return std::nullopt;
        auto tx = it->second.take();
        if (!it->second.hasPending()) impl_->runtime.windowTransactions.erase(it);
        return tx;
    }

    void QuickJsScriptHost::clearWindowTransactions() noexcept {
        if (impl_) impl_->runtime.windowTransactions.clear();
    }

    std::shared_ptr<core::RearrangeSubmission> QuickJsScriptHost::pendingRearrangeSubmission() const noexcept {
        return impl_ ? impl_->runtime.submittedRearrange : nullptr;
    }

    CallbackInvokeResult QuickJsScriptHost::invokeWindowCloseRequest(core::WindowId windowId) {
        if (!impl_ || !impl_->runtime.context) return {false, "QuickJS 运行时尚未初始化"};
        ScopedValue idVal(impl_->runtime.context, JS_NewUint32(impl_->runtime.context, windowId));
        JSValueConst argv[] = {idVal.get()};
        return impl_->callDriver(impl_->runtime.onWindowCloseRequest, 1, argv, __func__);
    }

    CallbackInvokeResult QuickJsScriptHost::invokeWindowGeometryChanged(core::WindowId windowId, const core::WindowGeometryEvent& event) {
        if (!impl_ || !impl_->runtime.context) return {false, "QuickJS 运行时尚未初始化"};
        auto* ctx = impl_->runtime.context;
        ScopedValue idVal(ctx, JS_NewUint32(ctx, windowId));
        ScopedValue payload(ctx, JS_NewObject(ctx));
        JS_SetPropertyStr(ctx, payload.get(), "widthPx", JS_NewInt32(ctx, event.widthPx));
        JS_SetPropertyStr(ctx, payload.get(), "heightPx", JS_NewInt32(ctx, event.heightPx));
        JS_SetPropertyStr(ctx, payload.get(), "hasPosition", JS_NewBool(ctx, event.hasPosition));
        if (event.hasPosition) {
            JS_SetPropertyStr(ctx, payload.get(), "x", JS_NewInt32(ctx, event.x));
            JS_SetPropertyStr(ctx, payload.get(), "y", JS_NewInt32(ctx, event.y));
        }
        JS_SetPropertyStr(ctx, payload.get(), "placement", JS_NewString(ctx, placementLabel(event.placement)));
        JSValueConst argv[] = {idVal.get(), payload.get()};
        return impl_->callDriver(impl_->runtime.onWindowGeometryChanged, 2, argv, __func__);
    }

    CallbackInvokeResult QuickJsScriptHost::invokeWindowFocusChanged(core::WindowId windowId, bool focused) {
        if (!impl_ || !impl_->runtime.context) return {false, "QuickJS 运行时尚未初始化"};
        auto* ctx = impl_->runtime.context;
        ScopedValue idVal(ctx, JS_NewUint32(ctx, windowId));
        ScopedValue focusedVal(ctx, JS_NewBool(ctx, focused));
        JSValueConst argv[] = {idVal.get(), focusedVal.get()};
        return impl_->callDriver(impl_->runtime.onWindowFocusChanged, 2, argv, __func__);
    }

    CallbackInvokeResult QuickJsScriptHost::invokeWindowDpiChanged(core::WindowId windowId, double dpiScale) {
        if (!impl_ || !impl_->runtime.context) return {false, "QuickJS 运行时尚未初始化"};
        auto* ctx = impl_->runtime.context;
        ScopedValue idVal(ctx, JS_NewUint32(ctx, windowId));
        ScopedValue scaleVal(ctx, JS_NewFloat64(ctx, dpiScale));
        JSValueConst argv[] = {idVal.get(), scaleVal.get()};
        return impl_->callDriver(impl_->runtime.onWindowDpiChanged, 2, argv, __func__);
    }

    CallbackInvokeResult QuickJsScriptHost::invokeTimer(std::uint64_t timerId) {
        if (!impl_ || !impl_->runtime.context) return {true, {}};
        auto& state = impl_->runtime;
        auto* ctx = state.context;
        const auto it = state.timers.find(timerId);
        if (it == state.timers.end()) return {true, {}};

        const bool repeating = it->second.repeating;
        ScopedValue fn(ctx, JS_DupValue(ctx, it->second.callback));
        std::vector<JSValue> callArgs;
        callArgs.reserve(it->second.args.size());
        for (const auto& a : it->second.args) callArgs.push_back(JS_DupValue(ctx, a));

        if (!repeating) {
            JS_FreeValue(ctx, it->second.callback);
            for (auto& a : it->second.args) JS_FreeValue(ctx, a);
            state.timers.erase(it);
            state.fallbackTimerDue.erase(timerId);
        }

        CallbackInvokeResult res{true, {}};
        {
            Impl::TaskBudget budget(*impl_, __func__);
            ScopedValue ret(ctx, JS_Call(ctx, fn.get(), JS_UNDEFINED, static_cast<int>(callArgs.size()), callArgs.empty() ? nullptr : callArgs.data()));
            for (auto& a : callArgs) JS_FreeValue(ctx, a);
            if (JS_IsException(ret.get())) {
                res = {false, impl_->exceptionText(ctx)};
            } else {
                const auto drained = impl_->drainJobs();
                res = {drained.ok, drained.error};
            }
        }

        // 周期定时器在回调返回且未被 clearInterval 时安排下一拍
        if (repeating && state.timers.contains(timerId)) {
            if (state.platform && state.platform->rescheduleRepeatingTimer) {
                state.platform->rescheduleRepeatingTimer(timerId);
            } else {
                state.fallbackTimerDue[timerId] = core::monotonicMillis() + state.timers.at(timerId).intervalMs;
            }
        }
        return res;
    }

    CallbackInvokeResult QuickJsScriptHost::notifyGraphicsEnvironmentChanged() {
        if (!impl_ || !impl_->runtime.context) return {true, {}};
        auto* ctx = impl_->runtime.context;
        if (!JS_IsFunction(ctx, impl_->runtime.onGraphicsEnvChanged)) return {true, {}};
        ScopedValue payload(ctx, QuickJsPlatformApi::makeGraphicsEnvObject(ctx, impl_->runtime));
        JSValueConst argv[] = {payload.get()};
        return impl_->callDriver(impl_->runtime.onGraphicsEnvChanged, 1, argv, __func__);
    }

    CallbackInvokeResult QuickJsScriptHost::notifySecondInstance(const core::LaunchRequest& request) {
        if (!impl_ || !impl_->runtime.context) return {true, {}};
        auto* ctx = impl_->runtime.context;
        if (!JS_IsFunction(ctx, impl_->runtime.onSecondInstance)) {
            impl_->runtime.bufferedSecondInstances.push_back(request);
            return {true, {}};
        }
        ScopedValue payload(ctx, QuickJsPlatformApi::makeLaunchRequestObject(ctx, request));
        JSValueConst argv[] = {payload.get()};
        return impl_->callDriver(impl_->runtime.onSecondInstance, 1, argv, __func__);
    }

    CallbackInvokeResult QuickJsScriptHost::notifyTrayEvent(const core::TrayEvent& event) {
        if (!impl_ || !impl_->runtime.context) return {true, {}};
        auto* ctx = impl_->runtime.context;
        if (!JS_IsFunction(ctx, impl_->runtime.onTrayEvent)) return {true, {}};
        ScopedValue payload(ctx, JS_NewObject(ctx));
        JS_SetPropertyStr(ctx, payload.get(), "trayId", JS_NewStringLen(ctx, event.trayId.data(), event.trayId.size()));
        const char* kind = "click";
        if (event.kind == core::TrayEventKind::DoubleClick)
            kind = "doubleClick";
        else if (event.kind == core::TrayEventKind::ContextMenu)
            kind = "contextMenu";
        JS_SetPropertyStr(ctx, payload.get(), "kind", JS_NewString(ctx, kind));
        JS_SetPropertyStr(ctx, payload.get(), "anchor", QuickJsPlatformApi::makeTrayAnchorObject(ctx, event.anchor));
        JSValueConst argv[] = {payload.get()};
        return impl_->callDriver(impl_->runtime.onTrayEvent, 1, argv, __func__);
    }

    CallbackInvokeResult QuickJsScriptHost::notifyTaskbarActionClicked(core::WindowId windowId, const std::string& actionId) {
        if (!impl_ || !impl_->runtime.context) return {true, {}};
        auto* ctx = impl_->runtime.context;
        if (!JS_IsFunction(ctx, impl_->runtime.onTaskbarAction)) return {true, {}};
        ScopedValue idVal(ctx, JS_NewUint32(ctx, windowId));
        ScopedValue actVal(ctx, JS_NewStringLen(ctx, actionId.data(), actionId.size()));
        JSValueConst argv[] = {idVal.get(), actVal.get()};
        return impl_->callDriver(impl_->runtime.onTaskbarAction, 2, argv, __func__);
    }

    CallbackInvokeResult QuickJsScriptHost::notifyMediaButtonPressed(core::MediaButtonKind button) {
        if (!impl_ || !impl_->runtime.context) return {true, {}};
        auto* ctx = impl_->runtime.context;
        if (!JS_IsFunction(ctx, impl_->runtime.onMediaButton)) return {true, {}};
        const char* name = "play";
        switch (button) {
            case core::MediaButtonKind::Play: name = "play"; break;
            case core::MediaButtonKind::Pause: name = "pause"; break;
            case core::MediaButtonKind::Stop: name = "stop"; break;
            case core::MediaButtonKind::Next: name = "next"; break;
            case core::MediaButtonKind::Previous: name = "previous"; break;
        }
        ScopedValue btnVal(ctx, JS_NewString(ctx, name));
        JSValueConst argv[] = {btnVal.get()};
        return impl_->callDriver(impl_->runtime.onMediaButton, 1, argv, __func__);
    }

    CallbackInvokeResult QuickJsScriptHost::completeRearrange(const std::shared_ptr<canton::core::RearrangeSubmission>& submission, const std::string& error) {
        if (!impl_) return {true, {}};
        auto& state = impl_->runtime;
        if (!submission || submission->cancelled || submission != state.rearrangeSubmission || JS_IsUndefined(state.rearrangeCompletion)) return {true, {}};
        {
            // 回执包含结构退休与生命周期通知，属于当前视觉候选的完成阶段
            Impl::TaskBudget budget(*impl_, __func__, impl_->limits.visualMillis);
            const auto callback = state.rearrangeCompletion;
            state.rearrangeCompletion = JS_UNDEFINED;
            if (error.empty())
                state.commitRearrange();
            else
                state.abortRearrange();

            auto argument = error.empty() ? JS_UNDEFINED : JS_NewStringLen(state.context, error.data(), error.size());
            ScopedValue result(state.context, JS_Call(state.context, callback, JS_UNDEFINED, 1, &argument));
            JS_FreeValue(state.context, argument);
            JS_FreeValue(state.context, callback);
            if (JS_IsException(result.get())) return {false, impl_->exceptionText(state.context)};
        }
        const auto drained = impl_->drainJobs();
        return {drained.ok, drained.error};
    }

    void QuickJsScriptHost::publishScene(const canton::core::NativeScene& scene) {
        publishWindowScene(0, scene);
    }

    void QuickJsScriptHost::publishWindowScene(core::WindowId windowId, const canton::core::NativeScene& scene) {
        if (!impl_) return;
        auto& state = impl_->runtime;
        if (windowId == 0)
            state.events.publish(scene.activeEventSlots());
        else
            state.events.publishWindow(windowId, scene.activeEventSlots(), state.nodeWindows);

        std::erase_if(state.publishedModifiers, [&](const auto& entry) {
            return state.windowOfNode(entry.second.node.id) == windowId || !state.nodeGenerations.contains(entry.second.node.id);
        });
        for (auto it = state.modifierInputs.begin(); it != state.modifierInputs.end();) {
            if (state.windowOfNode(it->first) == windowId || !state.nodeGenerations.contains(it->first))
                it = state.modifierInputs.erase(it);
            else
                ++it;
        }
        for (const auto& [id, generation] : state.nodeGenerations) {
            if (state.windowOfNode(id) != windowId) continue;
            if (!scene.contains(id) || scene.node(id).generation != generation) continue;
            std::size_t position = 0;
            for (const auto& instance : scene.node(id).modifier.elements()) {
                const QuickJsRuntimeContext::PublishedModifier input{{id, generation}, instance.handle, instance.descriptor, position++};
                state.publishedModifiers.emplace(instance.handle.identity, input);
                state.modifierInputs[id].push_back(input);
            }
        }
        std::erase_if(state.bindings, [&](const auto& entry) {
            const auto* target = std::get_if<canton::core::ModifierInputTarget>(&entry.second.target);
            if (!target || state.windowOfNode(target->node.id) != windowId) return false;
            const auto found = state.publishedModifiers.find(target->modifier.identity);
            return found == state.publishedModifiers.end() || found->second.handle != target->modifier;
        });
    }

    void QuickJsScriptHost::unpublishWindowScene(core::WindowId windowId) {
        if (!impl_) return;
        auto& state = impl_->runtime;
        state.events.unpublishWindow(windowId, state.nodeWindows);
        std::erase_if(state.publishedModifiers, [&](const auto& entry) {
            return state.windowOfNode(entry.second.node.id) == windowId || !state.nodeGenerations.contains(entry.second.node.id);
        });
        for (auto it = state.modifierInputs.begin(); it != state.modifierInputs.end();) {
            if (state.windowOfNode(it->first) == windowId || !state.nodeGenerations.contains(it->first))
                it = state.modifierInputs.erase(it);
            else
                ++it;
        }
        state.windowTransactions.erase(windowId);
    }

    bool QuickJsScriptHost::hasPendingDiagnostics() const noexcept {
        return !impl_->runtime.diagnosticToasts.empty() || !impl_->runtime.diagnosticActions.empty();
    }

    std::vector<QuickJsToastRequest> QuickJsScriptHost::takeDiagnosticToasts() {
        if (!impl_) return {};
        auto toasts = std::move(impl_->runtime.diagnosticToasts);
        impl_->runtime.diagnosticToasts.clear();
        return toasts;
    }

    std::vector<QuickJsDiagnosticAction> QuickJsScriptHost::takeDiagnosticActions() {
        if (!impl_) return {};
        auto actions = std::move(impl_->runtime.diagnosticActions);
        impl_->runtime.diagnosticActions.clear();
        return actions;
    }

    std::optional<canton::core::MutationTransaction> QuickJsScriptHost::takePendingTransaction() noexcept {
        return pendingTransactions_.take();
    }

    void QuickJsScriptHost::clearPendingTransactions() noexcept {
        pendingTransactions_.clear();
    }

    void QuickJsScriptHost::setFrameTimeMillis(double nowMillis) noexcept {
        if (!impl_) return;
        auto& runtime = impl_->runtime;
        const auto timestamp = std::max(0.0, nowMillis);
        if (timestamp != runtime.frameTimeMillis) runtime.remainingFrameJobs = QuickJsRuntimeContext::maxJobsPerFrame;
        runtime.frameTimeMillis = timestamp;
    }

    void QuickJsScriptHost::setOwnerWake(std::function<void()> wake) {
        impl_->runtime.wakeOwner = std::move(wake);
    }

    CallbackInvokeResult QuickJsScriptHost::semanticCheckpoint(double nowMillis) {
        Impl::TaskBudget budget(*impl_, __func__);
        auto& state = impl_->runtime;
        if (!state.context) return {true, {}};
        if (state.framePrepared) return {false, "语义检查点不能重入视觉提交"};
        setFrameTimeMillis(nowMillis);
        state.remainingFrameJobs = QuickJsRuntimeContext::maxJobsPerFrame;
        if (state.painters && !state.painters->pump()) return {false, impl_->exceptionText(state.context)};
        if (!state.pumpAssets()) return {false, impl_->exceptionText(state.context)};
        if (!state.pumpPlatformAsync()) return {false, impl_->exceptionText(state.context)};
        // 无外部宿主定时队列（如纯单测）时由内置后备队列推进到期定时器
        if (!state.fallbackTimerDue.empty()) {
            const double now = core::monotonicMillis();
            std::vector<std::uint64_t> due;
            for (const auto& [id, dueAt] : state.fallbackTimerDue) {
                if (dueAt <= now) due.push_back(id);
            }
            std::sort(due.begin(), due.end());
            for (const auto id : due) {
                const auto res = invokeTimer(id);
                if (!res.ok) return res;
            }
        }
        const auto drained = impl_->drainJobs();
        return {drained.ok, drained.error};
    }

    bool QuickJsScriptHost::hasPendingSemanticWork() const noexcept {
        if (!impl_ || !impl_->runtime.context) return false;
        const auto& s = impl_->runtime;
        if (JS_IsJobPending(s.runtime) || !s.assetRequests.empty() || !s.assetExistsRequests.empty() || !s.fsRequests.empty() || !s.netRequests.empty() || (s.painters && s.painters->hasPending())) return true;
        if (!s.bufferedSecondInstances.empty() && JS_IsFunction(s.context, s.onSecondInstance)) return true;
        if (!s.fallbackTimerDue.empty()) return true;
        return false;
    }

    bool QuickJsScriptHost::hasPendingVisualWork() const noexcept {
        return impl_ && impl_->runtime.frameRequested;
    }

    CallbackInvokeResult QuickJsScriptHost::prepareVisualFrame(double nowMillis) {
        Impl::TaskBudget budget(*impl_, __func__, impl_->limits.visualMillis);
        auto& state = impl_->runtime;
        if (!state.context) return {false, "QuickJS 运行时尚未初始化"};
        if (state.framePrepared) return {false, "视觉帧不能重入"};
        setFrameTimeMillis(nowMillis);
        if (JS_IsUndefined(state.prepareFrame)) return {true, {}};

        state.frameRequested = false;
        state.framePrepared = true;
        ScopedValue timestamp(state.context, JS_NewFloat64(state.context, state.frameTimeMillis));
        JSValueConst argv[] = {timestamp.get()};
        ScopedValue result(state.context, JS_Call(state.context, state.prepareFrame, JS_UNDEFINED, 1, argv));
        if (JS_IsException(result.get())) {
            const auto error = impl_->exceptionText(state.context);
            state.abortRearrange();
            (void)completeVisualFrame(false);
            return {false, error};
        }
        return {true, {}};
    }

    CallbackInvokeResult QuickJsScriptHost::dispatchPositioned(core::WindowId windowId, const core::NativeScene& candidateScene, const std::vector<core::PositionedUpdate>& updates) {
        if (!impl_ || !impl_->runtime.context) return {false, "QuickJS 运行时尚未初始化"};
        if (updates.empty()) return {true, {}};
        Impl::TaskBudget budget(*impl_, __func__, impl_->limits.visualMillis);
        auto& state = impl_->runtime;
        auto* ctx = state.context;
        try {
            for (const auto& update : updates) {
                if (!update.callback.valid() || !candidateScene.hasEventSlot(update.callback)) continue;
                ScopedValue callback(ctx, JS_DupValue(ctx, state.events.candidateCallback(update.callback)));
                if (!JS_IsFunction(ctx, callback.get())) continue;
                ScopedValue payload(ctx, JS_NewObject(ctx));
                JS_SetPropertyStr(ctx, payload.get(), "windowId", JS_NewUint32(ctx, windowId));
                JS_SetPropertyStr(ctx, payload.get(), "width", JS_NewFloat64(ctx, update.geometry.size.width));
                JS_SetPropertyStr(ctx, payload.get(), "height", JS_NewFloat64(ctx, update.geometry.size.height));
                JS_SetPropertyStr(ctx, payload.get(), "parentX", JS_NewFloat64(ctx, update.geometry.positionInParent.x));
                JS_SetPropertyStr(ctx, payload.get(), "parentY", JS_NewFloat64(ctx, update.geometry.positionInParent.y));
                JS_SetPropertyStr(ctx, payload.get(), "a", JS_NewFloat64(ctx, update.geometry.localToRoot.a));
                JS_SetPropertyStr(ctx, payload.get(), "b", JS_NewFloat64(ctx, update.geometry.localToRoot.b));
                JS_SetPropertyStr(ctx, payload.get(), "c", JS_NewFloat64(ctx, update.geometry.localToRoot.c));
                JS_SetPropertyStr(ctx, payload.get(), "d", JS_NewFloat64(ctx, update.geometry.localToRoot.d));
                JS_SetPropertyStr(ctx, payload.get(), "tx", JS_NewFloat64(ctx, update.geometry.localToRoot.tx));
                JS_SetPropertyStr(ctx, payload.get(), "ty", JS_NewFloat64(ctx, update.geometry.localToRoot.ty));
                JS_SetPropertyStr(ctx, payload.get(), "clipX", JS_NewFloat64(ctx, update.geometry.clippedBoundsInWindow.x));
                JS_SetPropertyStr(ctx, payload.get(), "clipY", JS_NewFloat64(ctx, update.geometry.clippedBoundsInWindow.y));
                JS_SetPropertyStr(ctx, payload.get(), "clipWidth", JS_NewFloat64(ctx, update.geometry.clippedBoundsInWindow.width));
                JS_SetPropertyStr(ctx, payload.get(), "clipHeight", JS_NewFloat64(ctx, update.geometry.clippedBoundsInWindow.height));
                JS_SetPropertyStr(ctx, payload.get(), "attached", JS_TRUE);
                JSValueConst argv[]{payload.get()};
                ScopedValue res(ctx, JS_Call(ctx, callback.get(), JS_UNDEFINED, 1, argv));
                if (JS_IsException(res.get())) return {false, impl_->exceptionText(ctx)};
            }
            return {true, {}};
        } catch (const std::exception& error) {
            if (JS_HasException(ctx)) return {false, std::string(error.what()) + "：" + impl_->exceptionText(ctx)};
            return {false, error.what()};
        }
    }

    CallbackInvokeResult QuickJsScriptHost::prepareDrawModifiers(canton::core::NativeScene& candidateScene) {
        if (!impl_ || !impl_->runtime.context) return {false, "QuickJS 运行时尚未初始化"};
        Impl::TaskBudget budget(*impl_, __func__, impl_->limits.visualMillis);
        auto& state = impl_->runtime;
        try {
            for (const auto id : candidateScene.tree().nodeIds()) {
                const auto& node = candidateScene.node(id);
                for (std::size_t index = 0; index < node.modifier.elements().size(); ++index) {
                    const auto& current = node.modifier.elements()[index];
                    const auto* input = std::get_if<canton::core::DrawModifier>(&current.descriptor.value);
                    if (!input) continue;
                    const canton::core::Size size{current.bounds.width, current.bounds.height};
                    if (!candidateScene.hasEventSlot(input->prepare)) throw std::runtime_error("绘制准备受体已经退休");
                    ScopedValue callback(state.context, JS_DupValue(state.context, state.events.candidateCallback(input->prepare)));
                    if (!JS_IsFunction(state.context, callback.get())) throw std::runtime_error("绘制准备函数已经退休");
                    std::uint64_t effectiveRevision = input->revision;
                    ScopedValue revProp(state.context, JS_GetPropertyStr(state.context, callback.get(), "revision"));
                    if (JS_IsNumber(revProp.get())) {
                        double revNum = 0;
                        if (JS_ToFloat64(state.context, &revNum, revProp.get()) == 0 && std::isfinite(revNum) && revNum >= 0) {
                            effectiveRevision = std::max(effectiveRevision, static_cast<std::uint64_t>(revNum));
                        }
                    }
                    if (current.preparedDraw && current.preparedDrawSize == size && current.preparedDrawRevision == effectiveRevision && current.preparedDrawCallback == input->prepare) continue;
                    ScopedValue width(state.context, JS_NewFloat64(state.context, size.width));
                    ScopedValue height(state.context, JS_NewFloat64(state.context, size.height));
                    ScopedValue phase(state.context, JS_NewString(state.context, "prepare"));
                    if (input->transactional) state.preparedDrawCallbacks.push_back({JS_DupValue(state.context, callback.get()), size.width, size.height});
                    JSValueConst arguments[]{width.get(), height.get(), phase.get()};
                    ScopedValue prepared(state.context, JS_Call(state.context, callback.get(), JS_UNDEFINED, input->transactional ? 3 : 2, arguments));
                    if (JS_IsException(prepared.get())) throw std::runtime_error(impl_->exceptionText(state.context));
                    auto commands = QuickJsDrawReader(state.context, state.nativePainters.get(), &state.runtimeEffects, &state.renderLayers).read(prepared.get());
                    auto& instance = candidateScene.tree().mutableNode(id).modifier.elements()[index];
                    if (!instance.preparedDraw || *instance.preparedDraw != commands) instance.preparedDraw = std::make_shared<const canton::core::DrawCommands>(std::move(commands));
                    instance.preparedDrawSize = size;
                    instance.preparedDrawRevision = effectiveRevision;
                    instance.preparedDrawCallback = input->prepare;
                    candidateScene.noteDrawPrepared();
                }
            }
            return {true, {}};
        } catch (const std::exception& error) {
            if (JS_HasException(state.context)) return {false, std::string(error.what()) + "：" + impl_->exceptionText(state.context)};
            return {false, error.what()};
        }
    }

    CallbackInvokeResult QuickJsScriptHost::materializeLayout(canton::core::NodeId id, const std::vector<int>& indices) {
        Impl::TaskBudget budget(*impl_, __func__, impl_->limits.visualMillis);
        auto& state = impl_->runtime;
        if (!state.framePrepared || state.materializingLayout || !JS_IsFunction(state.context, state.materializeLayout)) return {false, "Layout 材料化缺少当前视觉候选或驱动"};
        if (indices.size() > 4096) return {false, "单次 Layout 材料化超过 4096 项保护上限"};
        ScopedValue values(state.context, JS_NewArray(state.context));
        for (std::uint32_t index = 0; index < indices.size(); ++index) JS_SetPropertyUint32(state.context, values.get(), index, JS_NewInt32(state.context, indices[index]));
        JSValue argv[] = {JS_NewUint32(state.context, id), values.get()};
        state.materializingLayout = true;
        ScopedValue result(state.context, JS_Call(state.context, state.materializeLayout, JS_UNDEFINED, 2, argv));
        state.materializingLayout = false;
        JS_FreeValue(state.context, argv[0]);
        if (JS_IsException(result.get())) return {false, impl_->exceptionText(state.context)};
        return {true, {}};
    }

    CallbackInvokeResult QuickJsScriptHost::completeVisualFrame(bool success) {
        auto& state = impl_->runtime;
        std::string drawError;
        {
            Impl::TaskBudget budget(*impl_, __func__, impl_->limits.visualMillis);
            const auto drawings = std::move(state.preparedDrawCallbacks);
            state.preparedDrawCallbacks.clear();
            for (const auto& drawing : drawings) {
                ScopedValue callback(state.context, drawing.callback);
                ScopedValue width(state.context, JS_NewFloat64(state.context, drawing.width));
                ScopedValue height(state.context, JS_NewFloat64(state.context, drawing.height));
                ScopedValue phase(state.context, JS_NewString(state.context, success ? "commit" : "rollback"));
                JSValueConst arguments[]{width.get(), height.get(), phase.get()};
                ScopedValue finished(state.context, JS_Call(state.context, callback.get(), JS_UNDEFINED, 3, arguments));
                if (JS_IsException(finished.get())) {
                    const auto error = impl_->exceptionText(state.context);
                    if (drawError.empty()) drawError = error;
                }
            }
            if (!state.framePrepared) return {drawError.empty(), drawError};
            state.framePrepared = false;
            ScopedValue argument(state.context, JS_NewBool(state.context, success));
            JSValueConst argv[] = {argument.get()};
            ScopedValue result(state.context, JS_Call(state.context, state.completeFrame, JS_UNDEFINED, 1, argv));
            if (JS_IsException(result.get())) {
                const auto error = impl_->exceptionText(state.context);
                return {false, drawError.empty() ? error : drawError + "\n" + error};
            }
        }
        const auto drained = impl_->drainJobs();
        if (!drawError.empty()) return {false, drawError};
        return {drained.ok, drained.error};
    }

    ScriptExecutionResult QuickJsScriptHost::executeModule(const std::filesystem::path& modulePath, std::string_view source) {
        pendingTransactions_.clear();
        const auto normalizedModulePath = std::filesystem::absolute(modulePath).lexically_normal();
        impl_->initialise(this);
        Impl::TaskBudget budget(*impl_, __func__, impl_->limits.moduleMillis);
        auto& state = impl_->runtime;
        ScopedValue compiled(state.context, JS_Eval(state.context, source.data(), source.size(), normalizedModulePath.generic_string().c_str(), JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY));
        if (JS_IsException(compiled.get())) return {false, impl_->exceptionText(state.context)};
        if (JS_VALUE_GET_TAG(compiled.get()) != JS_TAG_MODULE) return {false, "模块载荷不是有效的 ESM"};
        auto* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(compiled.get()));
        if (JS_ResolveModule(state.context, compiled.get()) < 0) return {false, impl_->exceptionText(state.context)};
        ScopedValue result(state.context, JS_EvalFunction(state.context, JS_DupValue(state.context, compiled.get())));
        if (JS_IsException(result.get())) return {false, impl_->exceptionText(state.context)};
        auto drained = impl_->drainJobs();
        if (!drained.ok) return {false, drained.error};
        std::string launchError;
        if (!launchModuleDefaultExport(state, module, launchError)) return {false, launchError.empty() ? impl_->exceptionText(state.context) : launchError};
        drained = impl_->drainJobs();
        if (!drained.ok) return {false, drained.error};
        if (!hasMountedApp()) return {false, "入口模块未启动应用：需要默认导出 appBuilder(Root).build() 的应用定义"};
        return {true, {}};
    }

    ScriptExecutionResult QuickJsScriptHost::executePackage(std::shared_ptr<const core::UiPackage> package) {
        pendingTransactions_.clear();
        impl_->initialise(this);
        Impl::TaskBudget budget(*impl_, __func__, impl_->limits.moduleMillis);
        auto& state = impl_->runtime;
        const std::string entry(package->entry());
        state.uiPackage = package;
        state.moduleLoader.install(std::move(package));
        auto* module = QuickJsModuleLoader::load(state.context, entry.c_str(), &state.moduleLoader);
        if (!module) return {false, impl_->exceptionText(state.context)};
        const auto value = JS_MKPTR(JS_TAG_MODULE, module);
        if (JS_ResolveModule(state.context, value) < 0) return {false, impl_->exceptionText(state.context)};
        ScopedValue result(state.context, JS_EvalFunction(state.context, JS_DupValue(state.context, value)));
        if (JS_IsException(result.get())) return {false, impl_->exceptionText(state.context)};
        auto drained = impl_->drainJobs();
        if (!drained.ok) return {false, drained.error};
        std::string launchError;
        if (!launchModuleDefaultExport(state, module, launchError)) return {false, launchError.empty() ? impl_->exceptionText(state.context) : launchError};
        drained = impl_->drainJobs();
        if (!drained.ok) return {false, drained.error};
        if (!hasMountedApp()) return {false, "入口模块未启动应用：需要默认导出 appBuilder(Root).build() 的应用定义"};
        return {true, {}};
    }

    QuickJsRuntimeContext* QuickJsScriptHost::runtimeContext() noexcept {
        return impl_ ? &impl_->runtime : nullptr;
    }

    ScriptExecutionResult QuickJsScriptHost::runModuleTask(const char* name, bool resetFirst, const std::function<bool(QuickJsRuntimeContext&, std::string&)>& step) {
        if (resetFirst) {
            pendingTransactions_.clear();
            impl_->initialise(this);
        }
        Impl::TaskBudget budget(*impl_, name, impl_->limits.moduleMillis);
        std::string customError;
        if (!step(impl_->runtime, customError)) return {false, customError.empty() ? impl_->exceptionText(impl_->runtime.context) : std::move(customError)};
        const auto drained = impl_->drainJobs();
        return {drained.ok, drained.error};
    }

    std::vector<canton::core::FocusCommand> QuickJsScriptHost::takeFocusCommands() {
        impl_->runtime.scopedFocusCommands.clear();
        return std::exchange(impl_->runtime.focusCommands, {});
    }

    std::vector<ScopedFocusCommand> QuickJsScriptHost::takeScopedFocusCommands() {
        impl_->runtime.focusCommands.clear();
        return std::exchange(impl_->runtime.scopedFocusCommands, {});
    }

    bool QuickJsScriptHost::hasPendingFocusCommands() const noexcept {
        return !impl_->runtime.focusCommands.empty() || !impl_->runtime.scopedFocusCommands.empty();
    }

    CallbackInvokeResult QuickJsScriptHost::invokeEventSlot(const canton::core::EventSlotId& slot, const CallbackInvokeOptions& options) {
        Impl::TaskBudget budget(*impl_, __func__);
        if (impl_->runtime.context == nullptr) return {false, "QuickJS runtime is not initialised"};
        if (!slot.valid()) return {false, "Canton event slot is invalid"};
        if (slot.kind == canton::core::EventSlotKind::DrawPrepare) return {false, "绘制 callback 只允许候选准备阶段调用"};
        const auto callbackValue = impl_->runtime.events.callback(slot);
        if (JS_IsUndefined(callbackValue)) return {false, "Canton event slot is not registered in QuickJS"};

        ScopedValue callback(impl_->runtime.context, JS_DupValue(impl_->runtime.context, callbackValue));
        JSValueConst* argv = nullptr;
        int argc = 0;
        JSValue argument = JS_UNDEFINED;
        if (options.hasStringArgument && !options.hasFocusArgument) {
            argument = JS_NewStringLen(impl_->runtime.context, options.stringArgument.data(), options.stringArgument.size());
            argv = &argument;
            argc = 1;
        }
        if (options.hasFocusArgument) {
            argument = JS_NewObject(impl_->runtime.context);
            JS_SetPropertyStr(impl_->runtime.context, argument, "isFocused", JS_NewBool(impl_->runtime.context, options.focused));
            JS_SetPropertyStr(impl_->runtime.context, argument, "hasFocus", JS_NewBool(impl_->runtime.context, options.hasFocus));
            argv = &argument;
            argc = 1;
        }
        ScopedValue result(impl_->runtime.context, JS_Call(impl_->runtime.context, callback.get(), JS_UNDEFINED, argc, argv));
        if (options.hasStringArgument || options.hasFocusArgument) JS_FreeValue(impl_->runtime.context, argument);
        if (JS_IsException(result.get())) return {false, impl_->exceptionText(impl_->runtime.context)};
        const auto drained = impl_->drainJobs();
        if (!drained.ok) return {false, drained.error};
        return {true, {}};
    }

    CallbackInvokeResult QuickJsScriptHost::invokeEventSlot(const canton::core::EventSlotId& slot, const canton::core::ScrollResult& scroll) {
        Impl::TaskBudget budget(*impl_, __func__);
        if (impl_->runtime.context == nullptr) return {false, "QuickJS runtime is not initialised"};
        if (!slot.valid()) return {false, "Canton event slot is invalid"};
        if (slot.kind == canton::core::EventSlotKind::DrawPrepare) return {false, "绘制 callback 只允许候选准备阶段调用"};
        const auto callbackValue = impl_->runtime.events.callback(slot);
        if (JS_IsUndefined(callbackValue)) return {false, "Canton event slot is not registered in QuickJS"};

        ScopedValue argument(impl_->runtime.context, JS_NewObject(impl_->runtime.context));
        JS_SetPropertyStr(impl_->runtime.context, argument.get(), "value", JS_NewFloat64(impl_->runtime.context, scroll.value));
        JS_SetPropertyStr(impl_->runtime.context, argument.get(), "maxValue", JS_NewFloat64(impl_->runtime.context, scroll.maxValue));
        JS_SetPropertyStr(impl_->runtime.context, argument.get(), "viewportSize", JS_NewFloat64(impl_->runtime.context, scroll.viewportSize));
        JS_SetPropertyStr(impl_->runtime.context, argument.get(), "contentSize", JS_NewFloat64(impl_->runtime.context, scroll.contentSize));
        JS_SetPropertyStr(impl_->runtime.context, argument.get(), "isScrollInProgress", JS_NewBool(impl_->runtime.context, false));
        if (scroll.lazy) {
            auto* context = impl_->runtime.context;
            const auto& lazy = *scroll.lazy;
            JS_SetPropertyStr(context, argument.get(), "needsMoreItems", JS_NewBool(context, lazy.needsMoreItems));
            JS_SetPropertyStr(context, argument.get(), "workGeneration", JS_NewFloat64(context, static_cast<double>(lazy.workGeneration)));
            JS_SetPropertyStr(context, argument.get(), "firstVisibleItemIndex", JS_NewInt32(context, lazy.firstVisibleItemIndex));
            JS_SetPropertyStr(context, argument.get(), "firstVisibleItemScrollOffset", JS_NewFloat64(context, lazy.firstVisibleItemScrollOffset));
            JS_SetPropertyStr(context, argument.get(), "totalItemsCount", JS_NewInt32(context, lazy.totalItemsCount));
            JSValue visible = JS_NewArray(context);
            for (std::uint32_t index = 0; index < lazy.visibleItems.size(); ++index) {
                const auto& item = lazy.visibleItems[index];
                JSValue entry = JS_NewObject(context);
                JS_SetPropertyStr(context, entry, "index", JS_NewInt32(context, item.index));
                JS_SetPropertyStr(context, entry, "offset", JS_NewFloat64(context, item.offset));
                JS_SetPropertyStr(context, entry, "size", JS_NewFloat64(context, item.size));
                JS_SetPropertyStr(context, entry, "span", JS_NewInt32(context, item.span));
                JS_SetPropertyUint32(context, visible, index, entry);
            }
            JS_SetPropertyStr(context, argument.get(), "visibleItemsInfo", visible);
        }
        ScopedValue callback(impl_->runtime.context, JS_DupValue(impl_->runtime.context, callbackValue));
        JSValueConst argv[1] = {argument.get()};
        ScopedValue result(impl_->runtime.context, JS_Call(impl_->runtime.context, callback.get(), JS_UNDEFINED, 1, argv));
        if (JS_IsException(result.get())) return {false, impl_->exceptionText(impl_->runtime.context)};
        const auto drained = impl_->drainJobs();
        if (!drained.ok) return {false, drained.error};
        return {true, {}};
    }

    CallbackInvokeResult QuickJsScriptHost::invokeEventSlot(const canton::core::EventSlotId& slot, const canton::core::InputEventPayload& payload) {
        Impl::TaskBudget budget(*impl_, __func__);
        auto* context = impl_->runtime.context;
        if (context == nullptr) return {false, "QuickJS runtime is not initialised"};
        if (!slot.valid()) return {false, "Canton event slot is invalid"};
        if (slot.kind == canton::core::EventSlotKind::DrawPrepare) return {false, "绘制 callback 只允许候选准备阶段调用"};
        const auto callbackValue = impl_->runtime.events.callback(slot);
        if (JS_IsUndefined(callbackValue)) return {false, "Canton event slot is not registered in QuickJS"};

        ScopedValue argument(context, JS_NewObject(context));
        const auto number = [&](const char* name, double value) {
            JS_SetPropertyStr(context, argument.get(), name, JS_NewFloat64(context, value));
        };
        const auto flag = [&](const char* name, bool value) {
            JS_SetPropertyStr(context, argument.get(), name, JS_NewBool(context, value));
        };
        const auto modifiers = [&](const canton::core::KeyModifierState& state) {
            JSValue object = JS_NewObject(context);
            JS_SetPropertyStr(context, object, "shift", JS_NewBool(context, state.shift));
            JS_SetPropertyStr(context, object, "alt", JS_NewBool(context, state.alt));
            JS_SetPropertyStr(context, object, "ctrl", JS_NewBool(context, state.ctrl));
            JS_SetPropertyStr(context, object, "meta", JS_NewBool(context, state.meta));
            JS_SetPropertyStr(context, object, "command", JS_NewBool(context, state.command));
            JS_SetPropertyStr(context, argument.get(), "modifiers", object);
        };
        std::visit(
            [&](const auto& event) {
                using T = std::decay_t<decltype(event)>;
                if constexpr (std::is_same_v<T, canton::core::PointerEventPayload>) {
                    number("x", event.x);
                    number("y", event.y);
                    modifiers(event.modifiers);
                } else if constexpr (std::is_same_v<T, canton::core::KeyEventPayload>) {
                    JS_SetPropertyStr(context, argument.get(), "type", JS_NewString(context, event.up ? "up" : "down"));
                    JS_SetPropertyStr(context, argument.get(), "key", JS_NewStringLen(context, event.key.data(), event.key.size()));
                    JS_SetPropertyStr(context, argument.get(), "text", JS_NewStringLen(context, event.text.data(), event.text.size()));
                    flag("repeat", event.repeat);
                    modifiers(event.modifiers);
                } else if constexpr (std::is_same_v<T, canton::core::WheelEventPayload>) {
                    number("deltaX", event.deltaX);
                    number("deltaY", event.deltaY);
                    modifiers(event.modifiers);
                } else if constexpr (std::is_same_v<T, canton::core::DragStartPayload>) {
                    number("x", event.x);
                    number("y", event.y);
                    number("width", event.width);
                    number("height", event.height);
                    modifiers(event.modifiers);
                } else if constexpr (std::is_same_v<T, canton::core::DragDeltaPayload>) {
                    number("deltaX", event.deltaX);
                    number("deltaY", event.deltaY);
                    modifiers(event.modifiers);
                } else if constexpr (std::is_same_v<T, canton::core::DragStopPayload>) {
                    number("velocity", event.velocity);
                    flag("cancelled", event.cancelled);
                } else if constexpr (std::is_same_v<T, canton::core::PointerInputPayload>) {
                    constexpr const char* types[]{"down", "move", "up", "cancel", "enter", "exit"};
                    JS_SetPropertyStr(context, argument.get(), "type", JS_NewString(context, types[static_cast<std::size_t>(event.type)]));
                    number("x", event.x);
                    number("y", event.y);
                    modifiers(event.modifiers);
                } else if constexpr (std::is_same_v<T, canton::core::InteractionPayload>) {
                    flag("hovered", event.hovered);
                    flag("pressed", event.pressed);
                    flag("focused", event.focused);
                    flag("dragged", event.dragged);
                }
            },
            payload);
        ScopedValue callback(context, JS_DupValue(context, callbackValue));
        JSValueConst argv[1] = {argument.get()};
        ScopedValue result(context, JS_Call(context, callback.get(), JS_UNDEFINED, 1, argv));
        if (JS_IsException(result.get())) return {false, impl_->exceptionText(context)};
        const auto consumed = JS_ToBool(context, result.get()) > 0;
        const auto drained = impl_->drainJobs();
        if (!drained.ok) return {false, drained.error};
        return {true, {}, consumed};
    }
}
