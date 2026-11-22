#include <runtime/AppSession.h>

#include <core/Clock.h>
#include <core/InputIntent.h>
#include <core/Layout.h>
#include <runtime/ErrorScreenModel.h>
#include <runtime/PainterResources.h>
#include <runtime/UiPackage.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace canton::runtime {
    namespace {
        constexpr std::string_view TAG = "AppSession";

        AppResolver::MountPackage makeMounter(const AppSessionOptions& options) {
            return options.mountPackage ? options.mountPackage : AppResolver::MountPackage(mountUiPackage);
        }

        IconBitmap scaleBitmapBilinear(const core::BitmapContent& src, int dstW, int dstH) {
            IconBitmap out{dstW, dstH, std::vector<std::uint32_t>(static_cast<std::size_t>(dstW * dstH), 0)};
            if (src.width <= 0 || src.height <= 0 || src.pixels.empty()) return out;
            for (int y = 0; y < dstH; ++y) {
                const float sy = (static_cast<float>(y) + 0.5f) * static_cast<float>(src.height) / static_cast<float>(dstH) - 0.5f;
                const int y0 = std::clamp(static_cast<int>(std::floor(sy)), 0, src.height - 1);
                const int y1 = std::clamp(y0 + 1, 0, src.height - 1);
                const float fy = std::clamp(sy - static_cast<float>(y0), 0.0f, 1.0f);
                for (int x = 0; x < dstW; ++x) {
                    const float sx = (static_cast<float>(x) + 0.5f) * static_cast<float>(src.width) / static_cast<float>(dstW) - 0.5f;
                    const int x0 = std::clamp(static_cast<int>(std::floor(sx)), 0, src.width - 1);
                    const int x1 = std::clamp(x0 + 1, 0, src.width - 1);
                    const float fx = std::clamp(sx - static_cast<float>(x0), 0.0f, 1.0f);

                    const auto p00 = src.pixels[static_cast<std::size_t>(y0 * src.width + x0)];
                    const auto p10 = src.pixels[static_cast<std::size_t>(y0 * src.width + x1)];
                    const auto p01 = src.pixels[static_cast<std::size_t>(y1 * src.width + x0)];
                    const auto p11 = src.pixels[static_cast<std::size_t>(y1 * src.width + x1)];

                    const auto channel = [&](unsigned shift) -> std::uint32_t {
                        const float c00 = static_cast<float>((p00 >> shift) & 0xffu);
                        const float c10 = static_cast<float>((p10 >> shift) & 0xffu);
                        const float c01 = static_cast<float>((p01 >> shift) & 0xffu);
                        const float c11 = static_cast<float>((p11 >> shift) & 0xffu);
                        const float top = c00 + (c10 - c00) * fx;
                        const float bot = c01 + (c11 - c01) * fx;
                        return static_cast<std::uint32_t>(std::clamp(std::lround(top + (bot - top) * fy), 0l, 255l));
                    };
                    out.pixels[static_cast<std::size_t>(y * dstW + x)] = (channel(24) << 24u) | (channel(16) << 16u) | (channel(8) << 8u) | channel(0);
                }
            }
            return out;
        }

        std::optional<IconBitmap> renderPainterToIcon(const core::PainterContent& content, int size) {
            if (const auto* bmp = dynamic_cast<const core::BitmapContent*>(&content)) {
                return scaleBitmapBilinear(*bmp, size, size);
            }
            if (const auto* vec = dynamic_cast<const core::VectorContent*>(&content)) {
                const auto whole = vec->intrinsicSize.value_or(core::Size{static_cast<float>(size), static_cast<float>(size)});
                IconBitmap out{size, size, vec->rasterize({0, 0, whole.width, whole.height}, size, size)};
                if (out.pixels.size() == static_cast<std::size_t>(size * size)) return out;
            }
            return std::nullopt;
        }
    }

    AppSession::AppSession(const core::TextMeasurer& measurer, std::shared_ptr<core::FontRegistry> fonts, AppSessionOptions options)
        : fonts_(std::move(fonts)),
          textLayout_(measurer, 256, 8 * 1024 * 1024, fonts_.get()),
          options_(std::move(options)),
          resourceWorkers_(createResourceWorkers()),
          controlRuntime_(core::SceneFramePipeline(core::LayoutEngine(textLayout_))),
          source_(
              AppResolver(makeMounter(options_)),
              RuntimePackageLoader(
                  options_.distResources ? options_.distResources : [workers = resourceWorkers_](std::shared_ptr<const core::UiPackage> package) {
                      return RuntimeLoadResources{packagePainterLoader(package, workers), packageAssetLoader(package, workers)};
                  },
                  fonts_,
                  options_.painters),
              options_.live),
          fallbackPipeline_(core::SceneFramePipeline(core::LayoutEngine(textLayout_))) {
        if (options_.hudFactory) globalDiagnostics_.setHud(options_.hudFactory());
        controlRuntime_.setExternalSurfaceHost(true);
        controlRuntime_.setEventSlotValidator([this](const core::EventSlotId& slot) {
            for (const auto& [_, win] : windows_) {
                if (win->pipeline.scene().hasEventSlot(slot)) return true;
            }
            return false;
        });
    }

    AppSession::~AppSession() {
        controlRuntime_.setWorkAvailable({});
        teardownAllWindows();
    }

    void AppSession::setWorkAvailable(std::function<void()> callback) {
        controlRuntime_.setWorkAvailable(std::move(callback));
    }

    void AppSession::configure(AppSourceConfig config) {
        reloadRequested_ = false;
        exitRequested_ = false;
        applyLoadOutcome(source_.configure(std::move(config)));
    }

    void AppSession::setStartupError(const std::string& message) {
        sessionLoaded_ = false;
        globalDiagnostics_.setError(makeErrorScreenModel(ErrorSource::AppPackage, message, {}, {}, false));
        fallbackPipeline_.enqueueIntent(core::InputIntent::diagnostics());
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::requestReload() noexcept {
        reloadRequested_ = true;
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::shutdownSession() {
        exitRequested_ = true;
        sessionLoaded_ = false;
        teardownAllWindows();
        controlRuntime_.reset();
    }

    std::vector<core::WindowId> AppSession::sortDestroyOrder(
        const std::vector<core::WindowId>& toDestroy,
        const std::unordered_map<core::WindowId, std::optional<core::WindowId>>& owners) {
        const std::unordered_set<core::WindowId> targetSet(toDestroy.begin(), toDestroy.end());
        const auto depthOf = [&](core::WindowId id) {
            int depth = 0;
            auto cur = id;
            std::unordered_set<core::WindowId> seen{id};
            while (true) {
                const auto it = owners.find(cur);
                if (it == owners.end() || !it->second || !seen.insert(*it->second).second) break;
                ++depth;
                cur = *it->second;
            }
            return depth;
        };
        auto sorted = toDestroy;
        std::stable_sort(sorted.begin(), sorted.end(), [&](core::WindowId a, core::WindowId b) {
            return depthOf(a) > depthOf(b);
        });
        return sorted;
    }

    void AppSession::teardownAllWindows() {
        if (windows_.empty()) return;
        std::unordered_map<core::WindowId, std::optional<core::WindowId>> owners;
        for (const auto& [id, win] : windows_) owners[id] = win->config.ownerWindowId;
        const auto ordered = sortDestroyOrder(windowOrder_, owners);
        for (const auto id : ordered) {
            if (options_.platform.onWindowDestroyed) options_.platform.onWindowDestroyed(id);
            windows_.erase(id);
        }
        windowOrder_.clear();
    }

    void AppSession::applyLoadOutcome(PackageLoadOutcome outcome) {
        if (outcome.pending) return;
        teardownAllWindows();
        sessionLoaded_ = false;
        everOpenedWindow_ = false;
        pendingWindowEvents_.clear();
        iconCache_.clear();
        pendingIcons_.clear();
        appConfig_ = {};
        controlRuntime_.reset();
        globalDiagnostics_.clearError();
        fallbackPipeline_.reset();

        if (outcome.error) globalDiagnostics_.setError(std::move(*outcome.error));
        for (const auto& log : outcome.logs) core::Log::write(log.level, TAG, log.message);
        for (const auto& toast : outcome.toasts) {
            (void)globalDiagnostics_.notify(toast.level, TAG, toast.title, toast.message, toast.coalesce);
        }

        if (!outcome.loaded) {
            fallbackPipeline_.enqueueIntent(core::InputIntent::diagnostics());
            return;
        }

        activePainterLoader_ = outcome.scriptHost ? outcome.scriptHost->painterLoader() : core::PainterLoader{};
        fontGeneration_ = fonts_ ? fonts_->generation() : 0;
        controlRuntime_.setScriptHost(std::move(outcome.scriptHost));
        sessionLoaded_ = true;
    }

    void AppSession::applyRuntimeFailure(const std::string& message, RuntimeFrameErrorPhase phase) {
        const auto src = phase == RuntimeFrameErrorPhase::Pipeline ? ErrorSource::NativeTransaction : ErrorSource::ScriptRuntime;
        const auto related = source_.activeSource() == PackageSource::Dist ? source_.config().distOverride : std::filesystem::path{};
        controlRuntime_.suspend();
        sessionLoaded_ = false;
        globalDiagnostics_.setError(makeErrorScreenModel(src, message, {}, related));
        (void)globalDiagnostics_.notify(core::LogLevel::Error, TAG, "运行时执行失败", message);
        for (auto& [_, win] : windows_) {
            win->diagnostics.setError(*globalDiagnostics_.error());
            win->pipeline.enqueueIntent(core::InputIntent::diagnostics());
        }
        fallbackPipeline_.enqueueIntent(core::InputIntent::diagnostics());
    }

    void AppSession::syncAppAndWindowMutations(bool& hadWindowChanges) {
        auto* host = controlRuntime_.scriptHost();
        if (!host) return;

        if (auto appSpec = host->takeAppConfig()) {
            const auto iconChanged = appSpec->icon != appConfig_.icon;
            appConfig_ = std::move(*appSpec);
            if (!appConfig_.appId.empty() && appConfig_.appId != appliedAppId_) {
                appliedAppId_ = appConfig_.appId;
            }
            if (options_.platform.onAppConfigChanged) options_.platform.onAppConfigChanged(appConfig_);
            if (iconChanged) {
                for (auto& [_, win] : windows_) requestIconIfNeeded(*win);
            }
        }

        if (host->consumeAppExitRequest()) {
            exitRequested_ = true;
            if (options_.platform.onRequestExit) options_.platform.onRequestExit();
            return;
        }

        const auto mutations = host->takeWindowMutations();
        if (!mutations.empty()) {
            hadWindowChanges = true;
            std::unordered_map<core::WindowId, std::optional<core::WindowId>> owners;
            for (const auto& [id, win] : windows_) owners[id] = win->config.ownerWindowId;
            std::vector<core::WindowId> pendingDestroys;

            for (const auto& mutation : mutations) {
                if (mutation.kind == quickjs::WindowMutationKind::Create) {
                    std::erase(pendingDestroys, mutation.windowId);
                    auto surface = std::make_unique<WindowSurfaceState>(textLayout_);
                    surface->windowId = mutation.windowId;
                    surface->rootNodeId = mutation.rootNodeId;
                    surface->config = mutation.config;
                    surface->widthPx = std::max(1, static_cast<int>(std::lround(mutation.config.sizeDp.width)));
                    surface->heightPx = std::max(1, static_cast<int>(std::lround(mutation.config.sizeDp.height)));
                    if (options_.hudFactory) surface->diagnostics.setHud(options_.hudFactory());
                    const auto windowId = mutation.windowId;
                    surface->text.setCancelCompositionHook([this, windowId] {
                        if (options_.platform.onCancelPlatformIme) options_.platform.onCancelPlatformIme(windowId);
                    });
                    owners[windowId] = mutation.config.ownerWindowId;
                    auto* raw = surface.get();
                    windows_[windowId] = std::move(surface);
                    if (std::find(windowOrder_.begin(), windowOrder_.end(), windowId) == windowOrder_.end()) {
                        windowOrder_.push_back(windowId);
                    }
                    everOpenedWindow_ = true;
                    if (options_.platform.onWindowCreated) options_.platform.onWindowCreated(windowId, raw->config);
                    requestIconIfNeeded(*raw);
                } else if (mutation.kind == quickjs::WindowMutationKind::Update) {
                    if (auto it = windows_.find(mutation.windowId); it != windows_.end()) {
                        it->second->config = mutation.config;
                        owners[mutation.windowId] = mutation.config.ownerWindowId;
                        if (options_.platform.onWindowUpdated) options_.platform.onWindowUpdated(mutation.windowId, it->second->config);
                        requestIconIfNeeded(*it->second);
                    }
                } else if (mutation.kind == quickjs::WindowMutationKind::Destroy) {
                    if (windows_.contains(mutation.windowId) && std::find(pendingDestroys.begin(), pendingDestroys.end(), mutation.windowId) == pendingDestroys.end()) {
                        pendingDestroys.push_back(mutation.windowId);
                    }
                }
            }

            if (!pendingDestroys.empty()) {
                const auto ordered = sortDestroyOrder(pendingDestroys, owners);
                for (const auto id : ordered) {
                    host->unpublishWindowScene(id);
                    if (options_.platform.onWindowDestroyed) options_.platform.onWindowDestroyed(id);
                    windows_.erase(id);
                    std::erase(windowOrder_, id);
                }
            }
        }

        for (const auto& cmd : host->takeWindowCommands()) {
            if (!windows_.contains(cmd.windowId)) continue;
            if (cmd.kind == core::WindowCommandKind::RequestClose) {
                notifyWindowCloseRequest(cmd.windowId);
            } else if (options_.platform.onWindowCommand) {
                options_.platform.onWindowCommand(cmd.windowId, cmd.kind);
            }
        }
    }

    void AppSession::requestIconIfNeeded(WindowSurfaceState& win) {
        const auto effective = !win.config.icon.empty() ? win.config.icon : appConfig_.icon;
        if (effective == win.appliedIconPath) return;
        win.appliedIconPath = effective;
        if (effective.empty()) {
            if (options_.platform.onWindowIconChanged) options_.platform.onWindowIconChanged(win.windowId, nullptr, nullptr);
            return;
        }
        if (const auto cached = iconCache_.find(effective); cached != iconCache_.end()) {
            if (options_.platform.onWindowIconChanged) options_.platform.onWindowIconChanged(win.windowId, &cached->second.bigIcon, &cached->second.smallIcon);
            return;
        }
        if (!activePainterLoader_) return;
        if (std::any_of(pendingIcons_.begin(), pendingIcons_.end(), [&](const PendingIcon& item) { return item.path == effective; })) return;
        pendingIcons_.push_back({effective, activePainterLoader_(effective, [this] { controlRuntime_.notifyWorkAvailable(); })});
    }

    void AppSession::pumpPendingIcons() {
        for (auto it = pendingIcons_.begin(); it != pendingIcons_.end();) {
            if (it->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
                ++it;
                continue;
            }
            const auto path = it->path;
            const auto loaded = it->future.get();
            it = pendingIcons_.erase(it);
            if (!loaded.content) {
                core::Log::w(TAG, "窗口图标解码失败", path, loaded.error);
                continue;
            }
            const auto bigIcon = renderPainterToIcon(*loaded.content, 32);
            const auto smallIcon = renderPainterToIcon(*loaded.content, 16);
            if (!bigIcon || !smallIcon) continue;
            auto& entry = iconCache_[path] = DecodedIcons{*bigIcon, *smallIcon};
            for (auto& [_, win] : windows_) {
                const auto effective = !win->config.icon.empty() ? win->config.icon : appConfig_.icon;
                if (effective == path && options_.platform.onWindowIconChanged) {
                    options_.platform.onWindowIconChanged(win->windowId, &entry.bigIcon, &entry.smallIcon);
                }
            }
        }
    }

    void AppSession::prepareDiagnosticsTextOps(core::PublishedFrame& frame) const {
        auto& content = frame.content;
        for (auto* ops : {&content.overlayDrawOps, &content.diagnosticsErrorDrawOps, &content.diagnosticsBadgeDrawOps, &content.diagnosticsFpsDrawOps, &content.diagnosticsToastDrawOps}) {
            for (auto& op : *ops) {
                if (op.type != core::DrawOpType::DrawText) continue;
                if (ops != &content.overlayDrawOps && op.textStyle.fontFamily.empty() && !options_.diagnosticsFontFamily.empty()) {
                    op.textStyle.fontFamily = options_.diagnosticsFontFamily;
                    op.textLayout.reset();
                }
                core::DrawOpsBuilder::prepareText(op, textLayout_);
            }
        }
    }

    void AppSession::pumpFallbackFrame(double nowMillis) {
        (void)globalDiagnostics_.tick(controlRuntime_, nowMillis);
        const core::Rect bounds{0.0f, 0.0f, static_cast<float>(fallbackWidthPx_), static_cast<float>(fallbackHeightPx_)};
        const float scale = static_cast<float>(fallbackDpiScale_ > 0.0 ? fallbackDpiScale_ : 1.0);
        (void)fallbackPipeline_.publishRetained([&](const core::NativeScene&, core::PublishedFrame& frame) {
            frame.content.viewport = {bounds.width, bounds.height};
            globalDiagnostics_.prepareFrame(bounds, options_.detailedErrorScreen, source_.activeSource(), source_.liveUnavailable(), textLayout_, frame.content, scale);
            frame.content.errorFrame = globalDiagnostics_.hasError() ? std::optional<std::string>(globalDiagnostics_.error()->summary) : std::nullopt;
            prepareDiagnosticsTextOps(frame);
        });
    }

    void AppSession::drainPendingWindowEvents() {
        if (pendingWindowEvents_.empty() || !sessionLoaded_ || globalDiagnostics_.hasError()) return;
        auto* host = controlRuntime_.scriptHost();
        if (!host) return;
        auto batch = std::move(pendingWindowEvents_);
        pendingWindowEvents_.clear();
        for (const auto& ev : batch) {
            if (!windows_.contains(ev.windowId)) continue;
            quickjs::CallbackInvokeResult res{true, {}};
            switch (ev.kind) {
                case PendingWindowEvent::Kind::Geometry:
                    res = host->invokeWindowGeometryChanged(ev.windowId, ev.geometry);
                    break;
                case PendingWindowEvent::Kind::Focus:
                    res = host->invokeWindowFocusChanged(ev.windowId, ev.focused);
                    break;
                case PendingWindowEvent::Kind::Dpi:
                    res = host->invokeWindowDpiChanged(ev.windowId, ev.dpiScale);
                    break;
                case PendingWindowEvent::Kind::CloseRequest:
                    res = host->invokeWindowCloseRequest(ev.windowId);
                    break;
            }
            if (!res.ok) {
                applyRuntimeFailure(res.error, RuntimeFrameErrorPhase::Event);
                return;
            }
        }
    }

    std::vector<core::WindowId> AppSession::pumpFrame(double nowMillis) {
        std::vector<core::WindowId> changedWindows;
        if (exitRequested_ || pumpingFrame_) return changedWindows;
        struct FrameReentrancyGuard {
            bool& flag;
            explicit FrameReentrancyGuard(bool& f) : flag(f) { flag = true; }
            ~FrameReentrancyGuard() { flag = false; }
        } guard(pumpingFrame_);

        source_.pumpLive(controlRuntime_, globalDiagnostics_, [this](PackageLoadOutcome outcome) { applyLoadOutcome(std::move(outcome)); });
        if (controlRuntime_.consumeReloadRequest() || std::exchange(reloadRequested_, false)) {
            applyLoadOutcome(source_.reload());
        }
        pumpPendingIcons();

        if (!sessionLoaded_ || globalDiagnostics_.hasError()) {
            if (!windows_.empty()) {
                for (const auto windowId : windowOrder_) {
                    auto& win = *windows_.at(windowId);
                    if (globalDiagnostics_.hasError()) win.diagnostics.setError(*globalDiagnostics_.error());
                    const auto prevRev = win.pipeline.publishedFrame().revision;
                    const core::Rect bounds{0.0f, 0.0f, static_cast<float>(win.widthPx), static_cast<float>(win.heightPx)};
                    const float scale = static_cast<float>(win.dpiScale > 0.0 ? win.dpiScale : 1.0);
                    (void)win.diagnostics.tick(controlRuntime_, nowMillis);
                    (void)win.pipeline.publishRetained([&](const core::NativeScene&, core::PublishedFrame& frame) {
                        frame.content.viewport = {bounds.width, bounds.height};
                        win.diagnostics.prepareFrame(bounds, options_.detailedErrorScreen, source_.activeSource(), source_.liveUnavailable(), textLayout_, frame.content, scale);
                        frame.content.errorFrame = win.diagnostics.hasError() ? std::optional<std::string>(win.diagnostics.error()->summary) : std::nullopt;
                        prepareDiagnosticsTextOps(frame);
                    });
                    if (win.pipeline.publishedFrame().revision != prevRev) changedWindows.push_back(windowId);
                }
            }
            if (needsFallbackWindow()) {
                const auto prevRev = fallbackPipeline_.publishedFrame().revision;
                pumpFallbackFrame(nowMillis);
                if (fallbackPipeline_.publishedFrame().revision != prevRev) changedWindows.push_back(0);
            }
            return changedWindows;
        }

        drainPendingWindowEvents();
        if (!sessionLoaded_ || globalDiagnostics_.hasError()) return changedWindows;

        const auto semantic = controlRuntime_.semanticCheckpoint(nowMillis);
        if (!semantic.ok) {
            applyRuntimeFailure(semantic.error, RuntimeFrameErrorPhase::Event);
            if (needsFallbackWindow()) {
                pumpFallbackFrame(nowMillis);
                changedWindows.push_back(0);
            }
            return changedWindows;
        }

        if (controlRuntime_.checkFontPreloadHold()) return changedWindows;

        auto* host = controlRuntime_.scriptHost();
        if (!host) return changedWindows;

        bool fontChanged = false;
        if (const auto gen = fonts_ ? fonts_->generation() : 0; gen != fontGeneration_) {
            fontGeneration_ = gen;
            fontChanged = true;
        }

        bool hadWindowChanges = false;
        syncAppAndWindowMutations(hadWindowChanges);
        if (exitRequested_) return changedWindows;

        bool needsScriptVisual = host->hasPendingVisualWork() || host->hasPendingFocusCommands() || host->hasPendingWindowWork();
        for (const auto& [_, win] : windows_) {
            if (win->pipeline.scene().tree().activeAnimationCount() > 0) needsScriptVisual = true;
        }

        std::shared_ptr<core::RearrangeSubmission> rearrangeSubmission;
        if (needsScriptVisual) {
            const auto prepared = host->prepareVisualFrame(nowMillis);
            if (!prepared.ok) {
                applyRuntimeFailure(prepared.error, RuntimeFrameErrorPhase::Animation);
                if (needsFallbackWindow()) {
                    pumpFallbackFrame(nowMillis);
                    changedWindows.push_back(0);
                }
                return changedWindows;
            }
            rearrangeSubmission = host->pendingRearrangeSubmission();
            syncAppAndWindowMutations(hadWindowChanges);
            if (exitRequested_) return changedWindows;
        }

        // 分发焦点命令到所属窗口（未带窗口身份的命令按请求器匹配各窗口）
        const auto focusCommands = host->takeScopedFocusCommands();
        std::unordered_map<core::WindowId, std::vector<core::FocusCommand>> focusByWindow;
        for (const auto& scoped : focusCommands) {
            if (scoped.windowId != 0) {
                focusByWindow[scoped.windowId].push_back(scoped.command);
            } else {
                for (const auto id : windowOrder_) focusByWindow[id].push_back(scoped.command);
            }
        }

        // 将候选事务入队至对应窗口
        for (const auto id : host->pendingTransactionWindows()) {
            auto tx = host->takeWindowTransaction(id);
            if (tx && windows_.contains(id)) windows_.at(id)->pipeline.enqueue(std::move(*tx));
        }

        // 分发诊断气泡与动作
        const auto toasts = controlRuntime_.takeDiagnosticToasts();
        const auto actions = controlRuntime_.takeDiagnosticActions();
        for (const auto& action : actions) {
            if (action.kind == quickjs::QuickJsDiagnosticActionKind::RequestReload) reloadRequested_ = true;
        }
        if (!toasts.empty() && !windowOrder_.empty()) {
            auto& primary = *windows_.at(windowOrder_.front());
            for (const auto& toast : toasts) {
                if (primary.diagnostics.notify(toast.level, toast.tag, toast.title, toast.content, toast.coalesce)) {
                    primary.pipeline.enqueueIntent(core::InputIntent::diagnostics());
                }
            }
        }

        std::optional<std::string> pipelineFailure;
        std::vector<std::pair<core::EventSlotId, core::ScrollResult>> scrollFeedbacks;
        std::vector<core::WindowId> publishedWindows;

        for (const auto windowId : windowOrder_) {
            auto& win = *windows_.at(windowId);
            win.pipeline.setFrameTime(nowMillis);

            // 推进滚轮平滑器
            while (const auto step = win.wheelSmoother.tick(nowMillis)) {
                dispatchWheelSample(win, step->first, step->second);
            }

            if (fontChanged || win.pipeline.scene().tree().activeAnimationCount() > 0 || !focusByWindow[windowId].empty()) {
                win.pipeline.enqueueIntent(core::InputIntent::animationFrame());
            }
            (void)win.diagnostics.tick(controlRuntime_, nowMillis);

            const auto prevRevision = win.pipeline.publishedFrame().revision;
            const core::Constraints constraints{0.0f, static_cast<float>(win.widthPx), 0.0f, static_cast<float>(win.heightPx)};
            const float scale = static_cast<float>(win.dpiScale > 0.0 ? win.dpiScale : 1.0);
            const float topSafe = (!win.diagnostics.hasError() && win.config.chrome == core::WindowChrome::CustomTitleBar) ? 32.0f * scale : 0.0f;
            const core::Rect hudBounds{0.0f, topSafe, static_cast<float>(win.widthPx), std::max(0.0f, static_cast<float>(win.heightPx) - topSafe)};

            core::InputSession candidateInput = win.input;
            InputTextSession candidateText = win.text;
            bool candidatePrepared = false;

            const auto finalize = [&](const core::NativeScene& scene, core::PublishedFrame& frame) {
                candidateInput.synchronizePublishedInput(scene.tree(), true, candidateText);
                candidateInput.synchronizePublishedFocus(scene.tree(), true, candidateText, focusByWindow[windowId]);
                candidateText.populatePublishedFrame(scene.tree(), true, frame.content);
                candidatePrepared = true;

                win.diagnostics.prepareFrame(hudBounds, options_.detailedErrorScreen, source_.activeSource(), source_.liveUnavailable(), textLayout_, frame.content, scale);
                frame.content.errorFrame = std::nullopt;
                prepareDiagnosticsTextOps(frame);
            };

            const core::FramePreparation preparation{
                [&](core::NodeId id, const std::vector<int>& indices) {
                    const auto materialized = host->materializeLayout(id, indices);
                    if (!materialized.ok) throw std::runtime_error(materialized.error);
                    auto tx = host->takeWindowTransaction(windowId);
                    if (!tx) throw std::runtime_error("Layout 材料化未提交原生候选");
                    return std::move(*tx);
                },
                [&](core::NativeScene& candidate) {
                    const auto prepared = host->prepareDrawModifiers(candidate);
                    if (!prepared.ok) throw std::runtime_error(prepared.error);
                },
                [&](core::LayoutTree& tree) {
                    win.input.prepareLazyInteraction(tree);
                },
            };

            const bool forceRun = fontChanged || !focusByWindow[windowId].empty();
            const auto result = win.pipeline.run(win.rootNodeId, constraints, forceRun, finalize, preparation);
            if (result.error) {
                pipelineFailure = *result.error;
                break;
            }

            if (result.ran) {
                if (candidatePrepared) {
                    win.input.commitState(std::move(candidateInput));
                    win.text.commitState(std::move(candidateText));
                    auto callbacks = makeInputCallbacks(windowId);
                    win.input.dispatchCommittedFocus(win.pipeline.scene().tree(), win.text, callbacks);
                    syncWindowImeState(win);
                }
                publishedWindows.push_back(windowId);
                for (const auto& snapshot : result.scrollUpdates) {
                    if (snapshot.eventSlot.valid()) scrollFeedbacks.emplace_back(snapshot.eventSlot, snapshot);
                }
            } else {
                // 仅叠层或诊断浮层变化时走保留发布
                (void)win.pipeline.publishRetained([&](const core::NativeScene& scene, core::PublishedFrame& frame) {
                    win.text.populatePublishedFrame(scene.tree(), true, frame.content);
                    win.diagnostics.prepareFrame(hudBounds, options_.detailedErrorScreen, source_.activeSource(), source_.liveUnavailable(), textLayout_, frame.content, scale);
                    frame.content.errorFrame = std::nullopt;
                    prepareDiagnosticsTextOps(frame);
                });
                syncWindowImeState(win);
            }

            const bool changed = win.pipeline.publishedFrame().revision != prevRevision;
            if (changed) changedWindows.push_back(windowId);
            win.diagnostics.recordFrame(changed, win.pipeline.publishedFrame().revision > 0, nowMillis, controlRuntime_);
        }

        if (pipelineFailure) {
            if (rearrangeSubmission) (void)host->completeRearrange(rearrangeSubmission, *pipelineFailure);
            (void)host->completeVisualFrame(false);
            applyRuntimeFailure(*pipelineFailure, RuntimeFrameErrorPhase::Pipeline);
            if (needsFallbackWindow()) {
                pumpFallbackFrame(nowMillis);
                changedWindows.push_back(0);
            }
            return changedWindows;
        }

        for (const auto windowId : publishedWindows) {
            if (windows_.contains(windowId)) host->publishWindowScene(windowId, windows_.at(windowId)->pipeline.scene());
        }

        for (const auto& [slot, snapshot] : scrollFeedbacks) {
            const auto feedback = host->invokeEventSlot(slot, snapshot);
            if (!feedback.ok) {
                if (rearrangeSubmission) (void)host->completeRearrange(rearrangeSubmission, feedback.error);
                (void)host->completeVisualFrame(false);
                applyRuntimeFailure(feedback.error, RuntimeFrameErrorPhase::Event);
                return changedWindows;
            }
        }

        if (rearrangeSubmission) {
            const auto completed = host->completeRearrange(rearrangeSubmission);
            if (!completed.ok) {
                (void)host->completeVisualFrame(false);
                applyRuntimeFailure(completed.error, RuntimeFrameErrorPhase::Pipeline);
                return changedWindows;
            }
        }
        const auto frameCompleted = host->completeVisualFrame(true);
        if (!frameCompleted.ok) {
            applyRuntimeFailure(frameCompleted.error, RuntimeFrameErrorPhase::Animation);
            return changedWindows;
        }

        // 末窗自动退出：仅在完整重排提交收尾后、本次会话曾成功打开过窗口且当前活跃窗口数降为 0 时触发
        if (hadWindowChanges && appConfig_.exitOnLastWindowClosed && everOpenedWindow_ && windows_.empty() && !globalDiagnostics_.hasError()) {
            exitRequested_ = true;
            if (options_.platform.onRequestExit) options_.platform.onRequestExit();
        }

        return changedWindows;
    }

    bool AppSession::wantsFrame() const {
        if (exitRequested_) return false;
        if (reloadRequested_ || globalDiagnostics_.hasActiveToasts() || !pendingWindowEvents_.empty()) return true;
        if (sessionLoaded_ && !globalDiagnostics_.hasError()) {
            if (controlRuntime_.hasPendingEvents() || controlRuntime_.awaitingFontPreload()) return true;
            if (const auto* host = const_cast<CantonRuntime&>(controlRuntime_).scriptHost()) {
                if (host->hasPendingVisualWork() || host->hasPendingFocusCommands() || host->hasPendingWindowWork() || host->hasPendingSemanticWork() || host->hasPendingDiagnostics()) return true;
            }
            if (fonts_ && fonts_->generation() != fontGeneration_) return true;
        }
        for (const auto& [_, win] : windows_) {
            if (win->pipeline.hasPendingIntents() || win->pipeline.hasPendingTransactions() || win->pipeline.scene().tree().activeAnimationCount() > 0 || win->wheelSmoother.active() || win->diagnostics.hasActiveToasts() || win->diagnostics.fpsEnabled()) {
                return true;
            }
        }
        return false;
    }

    bool AppSession::ready() const {
        if (!sessionLoaded_ || globalDiagnostics_.hasError() || windows_.empty()) return false;
        for (const auto& [_, win] : windows_) {
            if (win->config.visible && win->pipeline.publishedFrame().revision > 0) return true;
        }
        return false;
    }

    bool AppSession::needsFallbackWindow() const noexcept {
        if (!globalDiagnostics_.hasError()) return false;
        for (const auto& [_, win] : windows_) {
            if (win->config.visible) return false;
        }
        return true;
    }

    core::InputCallbacks AppSession::makeInputCallbacks(core::WindowId windowId) {
        core::InputCallbacks result;
        result.invokeStringEvent = [this](const core::EventSlotId& slot, const std::string& value) {
            if (slot.valid()) controlRuntime_.enqueueStringEvent(slot, value);
        };
        result.invokeFocusEvent = [this](const core::EventSlotId& slot, bool focused, bool hasFocus) {
            if (slot.valid()) controlRuntime_.enqueueFocusEvent(slot, focused, hasFocus);
        };
        result.invokeEvent = [this](const core::EventSlotId& slot) {
            if (slot.valid()) controlRuntime_.enqueueEvent(slot);
        };
        result.invokeScrollEvent = [this](const core::EventSlotId& slot, const core::ScrollResult& scroll) {
            if (slot.valid()) controlRuntime_.enqueueScrollSnapshotEvent(slot, scroll);
        };
        result.dispatchKeyEvent = [this](const core::EventSlotId& slot, const core::KeyEventPayload& payload) {
            return controlRuntime_.dispatchInputEventNow(slot, payload, core::monotonicMillis());
        };
        result.invalidateNativeState = [this, windowId](core::NodeId nodeId, core::DirtyFlag flag) {
            if (auto* win = window(windowId)) {
                core::MutationTransaction tx;
                tx.operations.emplace_back(core::NativeInvalidationMutation{nodeId, flag});
                win->pipeline.enqueueIntent(core::InputIntent::jsCommit(std::move(tx)));
            }
        };
        result.enqueueKeyIntent = [this, windowId](core::NodeId) {
            if (auto* win = window(windowId)) win->pipeline.enqueueIntent(core::InputIntent::key());
        };
        result.enqueueTextInputIntent = [this, windowId](core::NodeId) {
            if (auto* win = window(windowId)) win->pipeline.enqueueIntent(core::InputIntent::textInput());
        };
        result.enqueueImeCompositionIntent = [this, windowId](core::NodeId) {
            if (auto* win = window(windowId)) win->pipeline.enqueueIntent(core::InputIntent::imeComposition());
        };
        result.readClipboard = options_.platform.readClipboard;
        result.writeClipboard = options_.platform.writeClipboard;
        return result;
    }

    void AppSession::emitGestureOutput(WindowSurfaceState& win, const core::PointerGestureOutput& output) {
        win.input.notePointerEffects(output);
        for (const auto& ev : output.events) {
            if (!ev.slot.valid()) continue;
            if (std::holds_alternative<std::monostate>(ev.payload))
                controlRuntime_.enqueueEvent(ev.slot);
            else
                controlRuntime_.enqueueInputEvent(ev.slot, ev.payload);
        }
    }

    void AppSession::syncWindowImeState(WindowSurfaceState& win) {
        const auto& tree = win.pipeline.scene().tree();
        const bool interactive = sessionLoaded_ && !globalDiagnostics_.hasError();
        const bool editable = win.text.isEditableActive(tree, interactive);
        if (win.imeEnabled != editable) {
            win.imeEnabled = editable;
            if (options_.platform.onImeStateChanged) options_.platform.onImeStateChanged(win.windowId, editable);
        }
        if (editable) {
            const auto caret = win.text.caretBoundsInRoot(tree, interactive);
            if (caret && caret != win.lastCaretBounds) {
                win.lastCaretBounds = caret;
                if (options_.platform.onImeCaretChanged) options_.platform.onImeCaretChanged(win.windowId, *caret);
            }
        } else {
            win.lastCaretBounds.reset();
        }
    }

    void AppSession::notifyWindowResized(core::WindowId windowId, int widthPx, int heightPx) {
        auto* win = window(windowId);
        if (!win || widthPx <= 0 || heightPx <= 0) return;
        if (win->widthPx == widthPx && win->heightPx == heightPx) return;
        win->widthPx = widthPx;
        win->heightPx = heightPx;
        win->pipeline.enqueueIntent(core::InputIntent::resize());
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::notifyWindowGeometry(core::WindowId windowId, const core::WindowGeometryEvent& event) {
        auto* win = window(windowId);
        if (!win) return;
        if (event.placement != core::WindowPlacement::Minimized && event.widthPx > 0 && event.heightPx > 0) {
            if (win->widthPx != event.widthPx || win->heightPx != event.heightPx) {
                win->widthPx = event.widthPx;
                win->heightPx = event.heightPx;
                win->pipeline.enqueueIntent(core::InputIntent::resize());
            }
        }
        win->config.placement = event.placement;
        if (sessionLoaded_ && !globalDiagnostics_.hasError()) {
            if (pumpingFrame_) {
                PendingWindowEvent item;
                item.kind = PendingWindowEvent::Kind::Geometry;
                item.windowId = windowId;
                item.geometry = event;
                pendingWindowEvents_.push_back(item);
            } else if (auto* host = controlRuntime_.scriptHost()) {
                const auto res = host->invokeWindowGeometryChanged(windowId, event);
                if (!res.ok) applyRuntimeFailure(res.error, RuntimeFrameErrorPhase::Event);
            }
        }
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::notifyWindowFocus(core::WindowId windowId, bool focused) {
        auto* win = window(windowId);
        if (!win) return;
        win->focused = focused;
        if (!focused && sessionLoaded_ && !globalDiagnostics_.hasError()) {
            auto callbacks = makeInputCallbacks(windowId);
            auto& tree = win->pipeline.scene().tree();
            (void)win->input.keyStateChanged(tree, true, {}, callbacks);
            emitGestureOutput(*win, win->input.cancelPointer(tree, win->text));
            win->wheelSmoother.cancel();
            (void)win->text.cancelComposition(tree, true, callbacks, true);
        } else if (focused) {
            syncWindowImeState(*win);
        }
        if (sessionLoaded_ && !globalDiagnostics_.hasError()) {
            if (pumpingFrame_) {
                PendingWindowEvent item;
                item.kind = PendingWindowEvent::Kind::Focus;
                item.windowId = windowId;
                item.focused = focused;
                pendingWindowEvents_.push_back(item);
            } else if (auto* host = controlRuntime_.scriptHost()) {
                const auto res = host->invokeWindowFocusChanged(windowId, focused);
                if (!res.ok) applyRuntimeFailure(res.error, RuntimeFrameErrorPhase::Event);
            }
        }
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::notifyWindowDpi(core::WindowId windowId, double dpiScale) {
        auto* win = window(windowId);
        if (!win || !std::isfinite(dpiScale) || dpiScale <= 0) return;
        win->dpiScale = dpiScale;
        win->input.setTouchSlop(static_cast<float>(4.0 * dpiScale));
        win->lastCaretBounds.reset();
        if (sessionLoaded_ && !globalDiagnostics_.hasError()) {
            if (pumpingFrame_) {
                PendingWindowEvent item;
                item.kind = PendingWindowEvent::Kind::Dpi;
                item.windowId = windowId;
                item.dpiScale = dpiScale;
                pendingWindowEvents_.push_back(item);
            } else if (auto* host = controlRuntime_.scriptHost()) {
                const auto res = host->invokeWindowDpiChanged(windowId, dpiScale);
                if (!res.ok) applyRuntimeFailure(res.error, RuntimeFrameErrorPhase::Event);
            }
        }
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::notifyWindowCloseRequest(core::WindowId windowId) {
        if (!windows_.contains(windowId) || !sessionLoaded_ || globalDiagnostics_.hasError()) return;
        if (pumpingFrame_) {
            PendingWindowEvent item;
            item.kind = PendingWindowEvent::Kind::CloseRequest;
            item.windowId = windowId;
            pendingWindowEvents_.push_back(item);
        } else if (auto* host = controlRuntime_.scriptHost()) {
            const auto res = host->invokeWindowCloseRequest(windowId);
            if (!res.ok) applyRuntimeFailure(res.error, RuntimeFrameErrorPhase::Event);
        }
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::notifyFallbackResized(int widthPx, int heightPx, double dpiScale) {
        if (widthPx <= 0 || heightPx <= 0) return;
        const double validScale = dpiScale > 0.0 ? dpiScale : fallbackDpiScale_;
        if (fallbackWidthPx_ == widthPx && fallbackHeightPx_ == heightPx && std::abs(fallbackDpiScale_ - validScale) < 1e-4) return;
        fallbackWidthPx_ = widthPx;
        fallbackHeightPx_ = heightPx;
        fallbackDpiScale_ = validScale;
        fallbackPipeline_.enqueueIntent(core::InputIntent::diagnostics());
        controlRuntime_.notifyWorkAvailable();
    }

    core::WindowHitResult AppSession::hitTestWindowChrome(core::WindowId windowId, core::Point pointPx, float resizeBorderPx) const {
        const auto* win = window(windowId);
        if (!win) return core::WindowHitResult::Client;
        if (globalDiagnostics_.hasError() || win->diagnostics.hasError()) return core::WindowHitResult::Client;
        const auto& hitSnapshot = win->pipeline.publishedFrame().content.hitTest;
        if (!hitSnapshot) return core::WindowHitResult::Client;
        const core::WindowCapabilities caps{
            win->config.resizable,
            win->config.minimizable,
            win->config.maximizable,
            win->config.closable,
            win->config.placement,
            win->config.chrome,
            resizeBorderPx,
        };
        return core::resolveWindowHitTest(*hitSnapshot, {static_cast<float>(win->widthPx), static_cast<float>(win->heightPx)}, pointPx, caps);
    }

    core::PointerIcon AppSession::pointerIcon(core::WindowId windowId) const noexcept {
        const auto* win = window(windowId);
        return win ? win->input.pointerIcon() : core::PointerIcon::Default;
    }

    core::PointerPlatformEffects AppSession::takePointerEffects(core::WindowId windowId) noexcept {
        auto* win = window(windowId);
        return win ? win->input.takePointerEffects() : core::PointerPlatformEffects{};
    }

    void AppSession::pointerMove(core::WindowId windowId, const core::PointerSample& sample) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return;
        auto& tree = win->pipeline.scene().tree();
        const auto& hit = win->pipeline.publishedFrame().content.hitTest;
        if (!hit) return;
        if (win->input.focus().target() || win->input.pointerDragging() || win->text.isActive(tree, true)) {
            core::PointerGestureOutput dragOut;
            auto callbacks = makeInputCallbacks(windowId);
            if (win->input.pointerDrag(tree, true, sample, win->text, callbacks, &dragOut) || !dragOut.events.empty() || dragOut.consumed) {
                emitGestureOutput(*win, dragOut);
                syncWindowImeState(*win);
                controlRuntime_.notifyWorkAvailable();
                return;
            }
        }
        emitGestureOutput(*win, win->input.pointerMove(tree, *hit, sample));
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::pointerExit(core::WindowId windowId) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return;
        emitGestureOutput(*win, win->input.pointerExit(win->pipeline.scene().tree()));
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::pointerDown(core::WindowId windowId, const core::PointerSample& sample, core::PointerDownOptions options) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return;
        const auto& hit = win->pipeline.publishedFrame().content.hitTest;
        if (!hit) return;
        win->wheelSmoother.cancel();
        auto callbacks = makeInputCallbacks(windowId);
        const auto out = win->input.pointerDown(win->pipeline.scene().tree(), *hit, sample, win->text, callbacks, options);
        emitGestureOutput(*win, out);
        syncWindowImeState(*win);
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::pointerUp(core::WindowId windowId, const core::PointerSample& sample) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return;
        const auto& hit = win->pipeline.publishedFrame().content.hitTest;
        if (!hit) return;
        const auto result = win->input.pointerUp(win->pipeline.scene().tree(), *hit, sample, win->text);
        emitGestureOutput(*win, result.gesture);
        syncWindowImeState(*win);
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::contextClick(core::WindowId windowId, const core::PointerSample& sample) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return;
        const auto& hit = win->pipeline.publishedFrame().content.hitTest;
        if (!hit) return;
        emitGestureOutput(*win, win->input.contextClick(win->pipeline.scene().tree(), *hit, sample));
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::cancelPointer(core::WindowId windowId) {
        auto* win = window(windowId);
        if (!win) return;
        win->wheelSmoother.cancel();
        emitGestureOutput(*win, win->input.cancelPointer(win->pipeline.scene().tree(), win->text));
        controlRuntime_.notifyWorkAvailable();
    }

    void AppSession::dispatchWheelSample(WindowSurfaceState& win, core::Point pointPx, const core::WheelInput& input) {
        const auto& frame = win.pipeline.publishedFrame();
        const auto result = win.input.wheel(win.pipeline.scene().tree(), win.rootNodeId, pointPx, input, frame.revision, frame.content.hitTest.get());
        if (result.wheelEvent && result.wheelEvent->slot.valid()) {
            controlRuntime_.enqueueInputEvent(result.wheelEvent->slot, result.wheelEvent->payload);
        }
        if (result.scroll.consumed && result.scroll.eventSlot.valid()) {
            controlRuntime_.enqueueScrollSnapshotEvent(result.scroll.eventSlot, result.scroll);
        }
    }

    void AppSession::wheel(core::WindowId windowId, core::Point pointPx, const core::WheelInput& input) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return;
        if (win->wheelSmoother.push(pointPx, input, core::monotonicMillis())) {
            controlRuntime_.notifyWorkAvailable();
            return;
        }
        dispatchWheelSample(*win, pointPx, input);
        controlRuntime_.notifyWorkAvailable();
    }

    bool AppSession::keyDown(core::WindowId windowId, const core::KeyInput& key) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return false;
        auto callbacks = makeInputCallbacks(windowId);
        const bool consumed = win->input.keyDown(win->pipeline.scene().tree(), true, key, win->text, callbacks);
        syncWindowImeState(*win);
        controlRuntime_.notifyWorkAvailable();
        return consumed;
    }

    bool AppSession::keyStateChanged(core::WindowId windowId, const std::function<bool(int)>& isDown) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return false;
        auto callbacks = makeInputCallbacks(windowId);
        const bool consumed = win->input.keyStateChanged(win->pipeline.scene().tree(), true, isDown, callbacks);
        controlRuntime_.notifyWorkAvailable();
        return consumed;
    }

    void AppSession::modifierKeysChanged(core::WindowId windowId, core::KeyModifierState modifiers) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return;
        auto callbacks = makeInputCallbacks(windowId);
        win->input.modifierKeysChanged(win->pipeline.scene().tree(), true, modifiers, callbacks);
        controlRuntime_.notifyWorkAvailable();
    }

    bool AppSession::charInput(core::WindowId windowId, std::string_view utf8Text) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return false;
        auto callbacks = makeInputCallbacks(windowId);
        const bool consumed = win->text.insertText(win->pipeline.scene().tree(), true, utf8Text, callbacks);
        if (consumed) {
            syncWindowImeState(*win);
            controlRuntime_.notifyWorkAvailable();
        }
        return consumed;
    }

    bool AppSession::imeComposition(core::WindowId windowId, std::string preedit, std::size_t cursorOffsetBytes, std::vector<core::TextCompositionClause> clauses) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return false;
        auto callbacks = makeInputCallbacks(windowId);
        const bool consumed = win->text.setComposition(win->pipeline.scene().tree(), true, std::move(preedit), cursorOffsetBytes, std::move(clauses), callbacks);
        if (consumed) {
            syncWindowImeState(*win);
            controlRuntime_.notifyWorkAvailable();
        }
        return consumed;
    }

    bool AppSession::imeCommit(core::WindowId windowId, std::string committed) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return false;
        auto callbacks = makeInputCallbacks(windowId);
        const bool consumed = win->text.commitComposition(win->pipeline.scene().tree(), true, std::move(committed), callbacks);
        if (consumed) {
            syncWindowImeState(*win);
            controlRuntime_.notifyWorkAvailable();
        }
        return consumed;
    }

    bool AppSession::imeEndComposition(core::WindowId windowId) {
        auto* win = window(windowId);
        if (!win || !sessionLoaded_ || globalDiagnostics_.hasError()) return false;
        if (!win->text.state().composing()) return false;
        auto callbacks = makeInputCallbacks(windowId);
        const bool consumed = win->text.cancelComposition(win->pipeline.scene().tree(), true, callbacks, false);
        if (consumed) {
            syncWindowImeState(*win);
            controlRuntime_.notifyWorkAvailable();
        }
        return consumed;
    }

    void AppSession::setWindowFpsBadgeEnabled(core::WindowId windowId, bool enabled) {
        auto* win = window(windowId);
        if (!win || !win->diagnostics.hud()) return;
        win->fpsEnabled = enabled;
        win->diagnostics.hud()->setFpsEnabled(enabled, controlRuntime_);
        win->pipeline.enqueueIntent(core::InputIntent::diagnostics());
        controlRuntime_.notifyWorkAvailable();
    }

    bool AppSession::windowFpsBadgeEnabled(core::WindowId windowId) const noexcept {
        const auto* win = window(windowId);
        return win && win->diagnostics.fpsEnabled();
    }

    std::vector<core::WindowId> AppSession::windowIds() const {
        return windowOrder_;
    }

    const WindowSurfaceState* AppSession::window(core::WindowId windowId) const noexcept {
        const auto it = windows_.find(windowId);
        return it == windows_.end() ? nullptr : it->second.get();
    }

    WindowSurfaceState* AppSession::window(core::WindowId windowId) noexcept {
        const auto it = windows_.find(windowId);
        return it == windows_.end() ? nullptr : it->second.get();
    }
}
