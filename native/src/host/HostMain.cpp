#include <host/HostAssembly.h>

#include <core/Clock.h>
#include <core/Log.h>
#include <d3d11/DWriteText.h>
#include <d3d11/Renderer.h>
#include <runtime/VBlankSource.h>
#include <win32/ConsoleLog.h>
#include <win32/Window.h>
#include <win32/WindowManager.h>

#include <objbase.h>

#include <atomic>
#include <fstream>
#include <memory>
#include <unordered_map>

namespace canton::host {
    namespace {
        constexpr std::string_view TAG = "Host";
        constexpr std::uint32_t kDefaultBackground = 0xff1f232au;
        constexpr std::uint32_t kTransparentBackground = 0x00000000u;

        class WindowVBlankSource final : public runtime::VBlankSource {
           public:
            explicit WindowVBlankSource(std::function<void()> post) : post_(std::move(post)) {}

            void start(Callback callback) override {
                callback_ = std::move(callback);
                thread_.start([this] {
                    pending_.store(true, std::memory_order_release);
                    post_();
                });
            }

            void stop() noexcept override {
                thread_.stop();
                pending_.store(false, std::memory_order_release);
                callback_ = {};
            }

            void deliver(double nowMillis) {
                if (!pending_.exchange(false, std::memory_order_acq_rel)) return;
                const auto callback = callback_;
                if (callback) callback(nowMillis);
            }

           private:
            std::function<void()> post_;
            d3d11::VBlankThread thread_;
            std::atomic<bool> pending_{false};
            Callback callback_;
        };

        void writeReadyFile(const std::filesystem::path& file, const std::string& token) {
            auto staged = file;
            staged += L".tmp";
            {
                std::ofstream out(staged, std::ios::binary | std::ios::trunc);
                out << token;
                if (!out.flush()) {
                    core::Log::e(TAG, "无法写入首帧就绪文件");
                    return;
                }
            }
            if (!MoveFileExW(staged.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) core::Log::e(TAG, "无法写入首帧就绪文件");
        }

        struct HostWindowRenderBinding {
            std::unique_ptr<d3d11::RenderTarget> target;
            std::unique_ptr<DevWindowController> dev;
            bool failed = false;
        };
    }

    int runHost(HostAssembly assembly, const std::string& startupError) {
        const auto fonts = std::make_shared<core::FontRegistry>();
        const d3d11::DWriteTextMeasurer measurer(fonts.get());
        d3d11::Renderer renderer;
        core::Log::i(TAG, "显示适配器", renderer.adapter().description, renderer.adapter().software ? "（软件渲染）" : "");

        std::function<void()> onWakeCallback, onFontsChangedCallback, onEndSessionCallback;
        win32::MessageDispatcher dispatcher({
            [&] { if (onWakeCallback) onWakeCallback(); },
            [&] { if (onFontsChangedCallback) onFontsChangedCallback(); },
            [&] { if (onEndSessionCallback) onEndSessionCallback(); },
        });
        const auto wake = [&dispatcher] { dispatcher.postWake(); };

        win32::WindowManager windowManager;
        std::unordered_map<core::WindowId, HostWindowRenderBinding> windowBindings;

        std::unique_ptr<win32::Window> fallbackWindow;
        std::unique_ptr<d3d11::RenderTarget> fallbackTarget;
        std::unique_ptr<DevWindowController> fallbackDev;

        runtime::AppSession* appPtr = nullptr;
        std::function<void(double)> runFrameTick;
        std::function<void()> scheduleIfNeeded;
        bool inFrameTick = false;
        bool exiting = false;
        bool firstFrameShown = false;

        const auto renderWindow = [&](core::WindowId windowId) {
            if (!appPtr) return;
            auto* win = windowManager.find(windowId);
            auto* surface = appPtr->window(windowId);
            auto bindingIt = windowBindings.find(windowId);
            if (!win || !surface || bindingIt == windowBindings.end() || bindingIt->second.failed) return;
            auto& binding = bindingIt->second;
            win->applyPointerEffects(appPtr->takePointerEffects(windowId));
            try {
                if (binding.dev) {
                    binding.dev->updateOverlay(surface->pipeline.timeline(), surface->pipeline.publishedFrame(), surface->pipeline.scene().tree(), surface->rootNodeId, appPtr->textLayout(), core::monotonicMillis(), static_cast<float>(surface->dpiScale));
                }
                const d3d11::FrameInput input{
                    &surface->pipeline.publishedFrame().content,
                    binding.dev ? binding.dev->overlayOps() : std::span<const core::DrawOp>{},
                    win->backdropActive() ? kTransparentBackground : kDefaultBackground,
                };
                renderer.render(*binding.target, input);
            } catch (const std::exception& error) {
                binding.failed = true;
                core::Log::e(TAG, "窗口帧渲染失败，已停止渲染该窗", error.what());
            }
        };

        const auto renderFallback = [&] {
            if (!appPtr || !fallbackWindow || !fallbackTarget) return;
            try {
                renderer.render(*fallbackTarget, {&appPtr->fallbackPublishedFrame().content, {}, kDefaultBackground});
            } catch (const std::exception& error) {
                core::Log::e(TAG, "兜底错误窗渲染失败", error.what());
            }
        };

        const auto syncFallbackWindow = [&] {
            if (!appPtr || exiting) return;
            if (appPtr->needsFallbackWindow()) {
                if (!fallbackWindow) {
                    core::WindowConfigSpec spec;
                    spec.title = "Canton";
                    spec.sizeDp = {960.0f, 640.0f};
                    spec.visible = true;
                    if (assembly.devControllerFactory) fallbackDev = assembly.devControllerFactory();
                    win32::WindowCallbacks cb;
                    cb.resized = [&](int w, int h) {
                        if (!appPtr || !fallbackTarget || !fallbackWindow) return;
                        appPtr->notifyFallbackResized(w, h, fallbackWindow->dpiScale());
                        renderer.resize(*fallbackTarget, w, h);
                        runFrameTick(core::monotonicMillis());
                        scheduleIfNeeded();
                    };
                    cb.dpiChanged = [&](double scale) {
                        if (!appPtr || !fallbackWindow) return;
                        int w = 0, h = 0;
                        fallbackWindow->clientSize(w, h);
                        appPtr->notifyFallbackResized(w, h, scale);
                        scheduleIfNeeded();
                    };
                    cb.paint = renderFallback;
                    cb.wake = wake;
                    cb.fontsChanged = [&] {
                        fonts->notifyChanged();
                        scheduleIfNeeded();
                    };
                    cb.closeRequested = [&] {
                        exiting = true;
                        win32::MessageDispatcher::quitMessageLoop(0);
                    };
                    cb.pointerDown = [&](const core::PointerSample&, core::PointerDownOptions) {
                        if (fallbackDev && appPtr && fallbackDev->handleErrorClick(*appPtr)) scheduleIfNeeded();
                    };
                    cb.keyDown = [&](int vk, const core::KeyInput& key) {
                        if (fallbackDev && appPtr && fallbackDev->handleKeyDown(vk, key.modifiers, *appPtr, 0)) {
                            scheduleIfNeeded();
                            return true;
                        }
                        return false;
                    };
                    fallbackWindow = std::make_unique<win32::Window>(spec, nullptr, std::move(cb));
                    fallbackTarget = renderer.createWindowTarget(fallbackWindow->handle());
                    int cw = 0, ch = 0;
                    fallbackWindow->clientSize(cw, ch);
                    appPtr->notifyFallbackResized(cw, ch, fallbackWindow->dpiScale());
                }
            } else if (fallbackWindow) {
                fallbackTarget.reset();
                fallbackDev.reset();
                fallbackWindow.reset();
            }
        };

        const auto makeWindowCallbacks = [&](core::WindowId windowId) {
            win32::WindowCallbacks cb;
            cb.wake = wake;
            cb.fontsChanged = [&] {
                fonts->notifyChanged();
                scheduleIfNeeded();
            };
            cb.endSession = [&] {
                if (appPtr && !exiting) {
                    exiting = true;
                    appPtr->shutdownSession();
                    win32::MessageDispatcher::quitMessageLoop(0);
                }
            };
            cb.resized = [&, windowId](int w, int h) {
                if (!appPtr) return;
                appPtr->notifyWindowResized(windowId, w, h);
                if (auto it = windowBindings.find(windowId); it != windowBindings.end() && it->second.target) {
                    renderer.resize(*it->second.target, w, h);
                }
                // 拖动边框期间消息循环被系统接管，同步推进一帧保持跟手
                runFrameTick(core::monotonicMillis());
                scheduleIfNeeded();
            };
            cb.geometryChanged = [&, windowId](const core::WindowGeometryEvent& ev) {
                if (!appPtr) return;
                appPtr->notifyWindowGeometry(windowId, ev);
                if (ev.widthPx > 0 && ev.heightPx > 0) {
                    if (auto it = windowBindings.find(windowId); it != windowBindings.end() && it->second.target) {
                        renderer.resize(*it->second.target, ev.widthPx, ev.heightPx);
                    }
                }
                scheduleIfNeeded();
            };
            cb.focusChanged = [&, windowId](bool focused) {
                if (!appPtr) return;
                appPtr->notifyWindowFocus(windowId, focused);
                scheduleIfNeeded();
            };
            cb.dpiChanged = [&, windowId](double scale) {
                if (!appPtr) return;
                appPtr->notifyWindowDpi(windowId, scale);
                scheduleIfNeeded();
            };
            cb.closeRequested = [&, windowId] {
                if (!appPtr) return;
                appPtr->notifyWindowCloseRequest(windowId);
                scheduleIfNeeded();
            };
            cb.paint = [&, windowId] { renderWindow(windowId); };
            cb.hitTest = [&, windowId](core::Point pt, float resizeBorderPx) {
                return appPtr ? appPtr->hitTestWindowChrome(windowId, pt, resizeBorderPx) : core::WindowHitResult::Client;
            };
            cb.pointerIcon = [&, windowId] {
                return appPtr ? appPtr->pointerIcon(windowId) : core::PointerIcon::Default;
            };
            cb.pointerMove = [&, windowId](const core::PointerSample& sample) {
                if (!appPtr) return;
                if (auto it = windowBindings.find(windowId); it != windowBindings.end() && it->second.dev) {
                    it->second.dev->setPointer(sample.point);
                }
                appPtr->pointerMove(windowId, sample);
                scheduleIfNeeded();
            };
            cb.pointerExit = [&, windowId] {
                if (!appPtr) return;
                if (auto it = windowBindings.find(windowId); it != windowBindings.end() && it->second.dev) {
                    it->second.dev->setPointer(std::nullopt);
                }
                appPtr->pointerExit(windowId);
                scheduleIfNeeded();
            };
            cb.pointerDown = [&, windowId](const core::PointerSample& sample, core::PointerDownOptions opts) {
                if (!appPtr) return;
                if (appPtr->globalDiagnostics().hasError()) {
                    if (auto it = windowBindings.find(windowId); it != windowBindings.end() && it->second.dev && it->second.dev->handleErrorClick(*appPtr)) {
                        scheduleIfNeeded();
                        return;
                    }
                }
                appPtr->pointerDown(windowId, sample, opts);
                scheduleIfNeeded();
            };
            cb.pointerUp = [&, windowId](const core::PointerSample& sample) {
                if (!appPtr) return;
                appPtr->pointerUp(windowId, sample);
                scheduleIfNeeded();
            };
            cb.contextClick = [&, windowId](const core::PointerSample& sample) {
                if (!appPtr) return;
                appPtr->contextClick(windowId, sample);
                scheduleIfNeeded();
            };
            cb.pointerCancelled = [&, windowId] {
                if (!appPtr) return;
                appPtr->cancelPointer(windowId);
                scheduleIfNeeded();
            };
            cb.wheel = [&, windowId](core::Point pt, const core::WheelInput& input) {
                if (!appPtr) return;
                appPtr->wheel(windowId, pt, input);
                scheduleIfNeeded();
            };
            cb.keyDown = [&, windowId](int vk, const core::KeyInput& key) {
                if (!appPtr) return false;
                if (auto it = windowBindings.find(windowId); it != windowBindings.end() && it->second.dev) {
                    if (it->second.dev->handleKeyDown(vk, key.modifiers, *appPtr, windowId)) {
                        scheduleIfNeeded();
                        return true;
                    }
                }
                const bool consumed = appPtr->keyDown(windowId, key);
                scheduleIfNeeded();
                return consumed;
            };
            cb.keyStateChanged = [&, windowId](const std::function<bool(int)>& isDown) {
                if (!appPtr) return false;
                const bool consumed = appPtr->keyStateChanged(windowId, isDown);
                scheduleIfNeeded();
                return consumed;
            };
            cb.modifiersChanged = [&, windowId](core::KeyModifierState mods) {
                if (!appPtr) return;
                appPtr->modifierKeysChanged(windowId, mods);
                scheduleIfNeeded();
            };
            cb.charInput = [&, windowId](std::string_view text) {
                if (!appPtr) return false;
                const bool consumed = appPtr->charInput(windowId, text);
                scheduleIfNeeded();
                return consumed;
            };
            cb.imeComposition = [&, windowId](std::string preedit, std::size_t cursorBytes, std::vector<core::TextCompositionClause> clauses) {
                if (!appPtr) return false;
                const bool consumed = appPtr->imeComposition(windowId, std::move(preedit), cursorBytes, std::move(clauses));
                scheduleIfNeeded();
                return consumed;
            };
            cb.imeCommit = [&, windowId](std::string committed) {
                if (!appPtr) return false;
                const bool consumed = appPtr->imeCommit(windowId, std::move(committed));
                scheduleIfNeeded();
                return consumed;
            };
            cb.imeEndComposition = [&, windowId] {
                if (!appPtr) return false;
                const bool consumed = appPtr->imeEndComposition(windowId);
                scheduleIfNeeded();
                return consumed;
            };
            return cb;
        };

        runtime::AppSessionOptions sessionOptions;
        sessionOptions.hudFactory = assembly.hudFactory;
        sessionOptions.detailedErrorScreen = assembly.detailedErrorScreen;
        if (assembly.live) sessionOptions.live = assembly.live(wake);
        sessionOptions.painters = std::make_shared<core::NativePainterRegistry>();

        sessionOptions.platform.onAppConfigChanged = [](const core::AppConfigSpec& spec) {
            if (!spec.appId.empty()) win32::setProcessAppId(spec.appId);
        };
        sessionOptions.platform.onWindowCreated = [&](core::WindowId windowId, const core::WindowConfigSpec& spec) {
            auto& win = windowManager.createWindow(windowId, spec, makeWindowCallbacks(windowId));
            HostWindowRenderBinding binding;
            binding.target = renderer.createWindowTarget(win.handle());
            if (assembly.devControllerFactory) binding.dev = assembly.devControllerFactory();
            int cw = 0, ch = 0;
            win.clientSize(cw, ch);
            renderer.resize(*binding.target, cw, ch);
            windowBindings.emplace(windowId, std::move(binding));
            if (appPtr) {
                appPtr->notifyWindowDpi(windowId, win.dpiScale());
                appPtr->notifyWindowResized(windowId, cw, ch);
                if (GetForegroundWindow() == win.handle() || GetFocus() == win.handle()) {
                    appPtr->notifyWindowFocus(windowId, true);
                }
            }
        };
        sessionOptions.platform.onWindowUpdated = [&](core::WindowId windowId, const core::WindowConfigSpec& spec) {
            windowManager.updateWindow(windowId, spec);
        };
        sessionOptions.platform.onWindowDestroyed = [&](core::WindowId windowId) {
            windowBindings.erase(windowId);
            windowManager.destroyWindow(windowId);
        };
        sessionOptions.platform.onWindowCommand = [&](core::WindowId windowId, core::WindowCommandKind kind) {
            if (kind == core::WindowCommandKind::Activate) {
                if (auto* win = windowManager.find(windowId)) win->activate();
            }
        };
        sessionOptions.platform.onWindowIconChanged = [&](core::WindowId windowId, const runtime::IconBitmap* bigIcon, const runtime::IconBitmap* smallIcon) {
            auto bigHandle = bigIcon ? win32::IconHandle::fromPbgra(bigIcon->pixels.data(), bigIcon->width, bigIcon->height) : win32::IconHandle{};
            auto smallHandle = smallIcon ? win32::IconHandle::fromPbgra(smallIcon->pixels.data(), smallIcon->width, smallIcon->height) : win32::IconHandle{};
            windowManager.setWindowIcons(windowId, std::move(bigHandle), std::move(smallHandle));
        };
        sessionOptions.platform.onImeStateChanged = [&](core::WindowId windowId, bool editable) {
            if (auto* win = windowManager.find(windowId)) win->setImeEnabled(editable);
        };
        sessionOptions.platform.onImeCaretChanged = [&](core::WindowId windowId, core::Rect caretPx) {
            if (auto* win = windowManager.find(windowId)) win->setImeCaretBounds(caretPx);
        };
        sessionOptions.platform.onCancelPlatformIme = [&](core::WindowId windowId) {
            if (auto* win = windowManager.find(windowId)) win->cancelImeComposition();
        };
        sessionOptions.platform.readClipboard = win32::readClipboardText;
        sessionOptions.platform.writeClipboard = [](const std::string& text) { win32::writeClipboardText(text); };
        sessionOptions.platform.onRequestExit = [&] {
            if (exiting) return;
            exiting = true;
            win32::MessageDispatcher::quitMessageLoop(0);
        };

        runtime::AppSession app(measurer, fonts, std::move(sessionOptions));
        appPtr = &app;
        app.setWorkAvailable(wake);

        WindowVBlankSource vblank(wake);
        runtime::VBlankFrameDriver frames(vblank);

        const auto anyExtraTickWork = [&] {
            for (const auto id : windowManager.windowIds()) {
                if (const auto* win = windowManager.find(id); win && win->needsPrecisionWheelTicks()) return true;
                if (const auto it = windowBindings.find(id); it != windowBindings.end() && it->second.dev && it->second.dev->animating()) return true;
            }
            return false;
        };

        runFrameTick = [&](double nowMillis) {
            if (exiting || inFrameTick) return;
            struct TickGuard {
                bool& active;
                explicit TickGuard(bool& a) : active(a) { active = true; }
                ~TickGuard() { active = false; }
            } guard(inFrameTick);
            for (const auto id : windowManager.windowIds()) {
                if (auto* win = windowManager.find(id)) win->tickPrecisionWheel();
            }

            const auto changed = app.pumpFrame(nowMillis);
            if (exiting) {
                frames.stop();
                return;
            }

            syncFallbackWindow();

            for (const auto id : windowManager.windowIds()) {
                const bool publishedChanged = std::find(changed.begin(), changed.end(), id) != changed.end();
                const auto bindIt = windowBindings.find(id);
                const bool overlayAnimating = bindIt != windowBindings.end() && bindIt->second.dev && bindIt->second.dev->animating();
                if (publishedChanged || overlayAnimating) {
                    renderWindow(id);
                }
            }
            if (fallbackWindow && (std::find(changed.begin(), changed.end(), 0u) != changed.end() || app.fallbackPublishedFrame().revision == 0)) {
                if (app.fallbackPublishedFrame().revision == 0) (void)app.pumpFrame(nowMillis);
                renderFallback();
            }

            if (!firstFrameShown && app.ready()) {
                firstFrameShown = true;
                core::Log::i(TAG, "首帧已呈现", runtime::packageSourceLabel(app.source().activeSource()));
                if (!assembly.readyFile.empty()) writeReadyFile(assembly.readyFile, assembly.readyToken);
            }

            if (exiting || (!app.wantsFrame() && !anyExtraTickWork())) {
                frames.stop();
            }
        };

        scheduleIfNeeded = [&] {
            if (!exiting && !frames.active() && (app.wantsFrame() || app.needsFallbackWindow() || anyExtraTickWork())) {
                frames.start(runFrameTick);
            }
        };

        onWakeCallback = [&] {
            const auto now = core::monotonicMillis();
            if (frames.active()) {
                vblank.deliver(now);
            } else {
                runFrameTick(now);
            }
            scheduleIfNeeded();
        };
        onFontsChangedCallback = [&] {
            fonts->notifyChanged();
            scheduleIfNeeded();
        };
        onEndSessionCallback = [&] {
            if (!exiting) {
                exiting = true;
                app.shutdownSession();
                win32::MessageDispatcher::quitMessageLoop(0);
            }
        };

        if (startupError.empty()) {
            app.configure(assembly.source);
        } else {
            app.setStartupError(startupError);
        }

        // 首轮同步推进一次，以便 Dist 模式下立即建窗或立起兜底错误窗
        runFrameTick(core::monotonicMillis());
        scheduleIfNeeded();

        const auto exitCode = win32::MessageDispatcher::runMessageLoop();
        exiting = true;
        frames.stop();
        app.setWorkAvailable({});
        app.shutdownSession();
        fallbackTarget.reset();
        fallbackDev.reset();
        fallbackWindow.reset();
        windowBindings.clear();
        windowManager.destroyAll();
        appPtr = nullptr;
        return exitCode;
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    using namespace canton;
    (void)SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    win32::enableConsoleLogColors();
    const auto comReady = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    const auto bundleRoot = std::filesystem::path(path).parent_path();
    int code = 1;
    try {
        host::HostAssembly assembly;
        std::string startupError;
        try {
            assembly = host::assembleHost(bundleRoot);
        } catch (const std::exception& error) {
            startupError = error.what();
            assembly.source.bundleRoot = bundleRoot;
            core::Log::e("Host", "宿主装配失败", startupError);
        }
        code = host::runHost(std::move(assembly), startupError);
    } catch (const std::exception& error) {
        core::Log::e("Host", "宿主异常退出", error.what());
    }
    if (comReady) CoUninitialize();
    return code;
}
