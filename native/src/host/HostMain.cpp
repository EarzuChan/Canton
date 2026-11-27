#include <host/HostAssembly.h>

#include <core/Clock.h>
#include <core/Log.h>
#include <d3d11/DWriteText.h>
#include <d3d11/Renderer.h>
#include <d3d11/ShaderCompiler.h>
#include <runtime/AppPaths.h>
#include <runtime/AppResolver.h>
#include <runtime/SingleInstance.h>
#include <runtime/UiPackage.h>
#include <runtime/VBlankSource.h>
#include <win32/ConsoleLog.h>
#include <win32/JumpList.h>
#include <win32/MediaSession.h>
#include <win32/ShellTray.h>
#include <win32/Window.h>
#include <win32/WindowManager.h>

#include <objbase.h>
#include <shellapi.h>

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
            bool failed = false;
        };

        core::LaunchRequest captureLaunchRequest() {
            core::LaunchRequest req;
            int argc = 0;
            if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
                for (int i = 1; i < argc; ++i) {
                    req.args.push_back(runtime::AppPaths::wideToUtf8(argv[i]));
                }
                LocalFree(argv);
            }
            const DWORD cwdLen = GetCurrentDirectoryW(0, nullptr);
            if (cwdLen > 0) {
                std::wstring cwd(cwdLen, L'\0');
                const DWORD written = GetCurrentDirectoryW(cwdLen, cwd.data());
                cwd.resize(written);
                req.workingDirectory = runtime::AppPaths::wideToUtf8(cwd);
            }
            return req;
        }

        core::GraphicsEnvironmentInfo buildGraphicsEnv(const d3d11::Renderer& renderer) {
            const auto adapter = renderer.adapter();
            const auto prefs = win32::querySystemGraphicsPreferences();
            core::GraphicsEnvironmentInfo env;
            env.adapterName = adapter.description;
            env.isSoftwareRenderer = adapter.software;
            env.isRemoteSession = prefs.isRemoteSession;
            env.systemTransparency = prefs.systemTransparency;
            env.systemAnimations = prefs.systemAnimations;
            return env;
        }
    }

    int runHost(HostAssembly assembly, const std::string& startupError) {
        // 启动前轻量探测随包清单元信息（appId / singleInstance / portable），在初始化 QuickJS/D3D11 之前完成单实例拦截
        std::string resolvedAppId = assembly.appIdOverride;
        bool singleInstance = assembly.singleInstanceOverride.value_or(false);
        bool portableManifest = false;
        if (startupError.empty()) {
            try {
                runtime::AppResolver resolver(runtime::mountUiPackage);
                const auto resolved = resolver.resolveDist(assembly.source);
                if (resolved.ok && resolved.package) {
                    if (resolvedAppId.empty() && !resolved.package->appId().empty()) {
                        resolvedAppId = std::string(resolved.package->appId());
                    }
                    if (!assembly.singleInstanceOverride.has_value()) {
                        singleInstance = resolved.package->singleInstance();
                    }
                    portableManifest = resolved.package->portable();
                }
            } catch (...) {
                // 随包探测失败不阻断正常启动与错误屏展示
            }
        }
        if (!resolvedAppId.empty()) win32::setProcessAppId(resolvedAppId);

        runtime::AppSession* appPtr = nullptr;
        std::mutex preMountSecondMutex;
        std::vector<core::LaunchRequest> preMountSecondInstances;

        runtime::SingleInstanceGuard singleInstanceGuard;
        if (singleInstance) {
            const std::string singleKey = !resolvedAppId.empty() ? resolvedAppId : runtime::AppPaths::wideToUtf8(assembly.source.bundleRoot.wstring());
            const bool acquired = singleInstanceGuard.acquireOrForward(
                singleKey,
                assembly.launchRequest,
                [&](core::LaunchRequest incoming) {
                    if (appPtr) {
                        appPtr->notifySecondInstance(std::move(incoming));
                    } else {
                        const std::scoped_lock lock(preMountSecondMutex);
                        preMountSecondInstances.push_back(std::move(incoming));
                    }
                });
            if (!acquired) {
                return 0;
            }
        }

        const auto fonts = std::make_shared<core::FontRegistry>();
        const d3d11::DWriteTextMeasurer measurer(fonts.get());
        d3d11::Renderer renderer;
        auto lastDeviceGen = renderer.deviceGeneration();
        core::Log::i(TAG, "显示适配器", renderer.adapter().description, renderer.adapter().software ? "（软件渲染）" : "");

        std::function<void()> onWakeCallback, onFontsChangedCallback, onSettingsChangedCallback, onTaskbarCreatedCallback, onEndSessionCallback;
        std::function<void(std::uint32_t, std::uint32_t, int, int)> onTrayMessageCallback;
        win32::MessageDispatcher dispatcher({
            [&] { if (onWakeCallback) onWakeCallback(); },
            [&] { if (onFontsChangedCallback) onFontsChangedCallback(); },
            [&] { if (onSettingsChangedCallback) onSettingsChangedCallback(); },
            [&] { if (onTaskbarCreatedCallback) onTaskbarCreatedCallback(); },
            [&](std::uint32_t iconId, std::uint32_t mouseMsg, int x, int y) {
                if (onTrayMessageCallback) onTrayMessageCallback(iconId, mouseMsg, x, y);
            },
            [&] { if (onEndSessionCallback) onEndSessionCallback(); },
        });
        const auto wake = [&dispatcher] { dispatcher.postWake(); };

        win32::ShellTrayManager trayManager(dispatcher.hwnd(), [&](const core::TrayEvent& ev) {
            if (appPtr) appPtr->notifyTrayEvent(ev);
        });
        if (!resolvedAppId.empty()) trayManager.setAppId(resolvedAppId);

        win32::MediaSessionController mediaSession(dispatcher.hwnd(), [&](core::MediaButtonKind btn) {
            if (appPtr) appPtr->notifyMediaButtonPressed(btn);
        });

        std::unordered_map<std::string, win32::IconHandle> trayIconHandles;
        std::unordered_map<core::WindowId, win32::IconHandle> windowBadgeIcons;
        std::unordered_map<core::WindowId, std::vector<win32::IconHandle>> windowActionIcons;

        win32::WindowManager windowManager;
        std::unordered_map<core::WindowId, HostWindowRenderBinding> windowBindings;

        std::unique_ptr<win32::Window> fallbackWindow;
        std::unique_ptr<d3d11::RenderTarget> fallbackTarget;
        auto devInput = std::move(assembly.devInput);

        std::function<void(double)> runFrameTick;
        std::function<void()> scheduleIfNeeded;
        bool inFrameTick = false;
        bool exiting = false;
        bool firstFrameShown = false;

        const auto syncGraphicsEnvIfDeviceRebuilt = [&] {
            const auto gen = renderer.deviceGeneration();
            if (gen != lastDeviceGen) {
                lastDeviceGen = gen;
                if (appPtr) appPtr->updateGraphicsEnvironment(buildGraphicsEnv(renderer));
            }
        };

        const auto renderWindow = [&](core::WindowId windowId) {
            if (!appPtr) return;
            auto* win = windowManager.find(windowId);
            auto* surface = appPtr->window(windowId);
            auto bindingIt = windowBindings.find(windowId);
            if (!win || !surface || bindingIt == windowBindings.end() || bindingIt->second.failed) return;
            auto& binding = bindingIt->second;
            win->applyPointerEffects(appPtr->takePointerEffects(windowId));
            try {
                const d3d11::FrameInput input{
                    &surface->pipeline.publishedFrame().content,
                    appPtr->windowOverlayOps(windowId),
                    (surface->config.transparent || win->backdropActive()) ? kTransparentBackground : kDefaultBackground,
                };
                renderer.render(*binding.target, input);
                syncGraphicsEnvIfDeviceRebuilt();
            } catch (const std::exception& error) {
                binding.failed = true;
                core::Log::e(TAG, "窗口帧渲染失败，已停止渲染该窗", error.what());
            }
        };

        const auto renderFallback = [&] {
            if (!appPtr || !fallbackWindow || !fallbackTarget) return;
            try {
                renderer.render(*fallbackTarget, {&appPtr->fallbackPublishedFrame().content, {}, kDefaultBackground});
                syncGraphicsEnvIfDeviceRebuilt();
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
                        if (devInput && appPtr && devInput->handleErrorClick(*appPtr)) scheduleIfNeeded();
                    };
                    cb.keyDown = [&](int vk, const core::KeyInput& key) {
                        if (devInput && appPtr && devInput->handleKeyDown(vk, key.modifiers, *appPtr, 0)) {
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
                appPtr->pointerMove(windowId, sample);
                scheduleIfNeeded();
            };
            cb.pointerExit = [&, windowId] {
                if (!appPtr) return;
                appPtr->pointerExit(windowId);
                scheduleIfNeeded();
            };
            cb.pointerDown = [&, windowId](const core::PointerSample& sample, core::PointerDownOptions opts) {
                if (!appPtr) return;
                if (appPtr->globalDiagnostics().hasError()) {
                    if (devInput && devInput->handleErrorClick(*appPtr)) {
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
                if (devInput && devInput->handleKeyDown(vk, key.modifiers, *appPtr, windowId)) {
                    scheduleIfNeeded();
                    return true;
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
            cb.taskbarActionClicked = [&, windowId](const std::string& actionId) {
                if (!appPtr) return;
                appPtr->notifyTaskbarActionClicked(windowId, actionId);
                scheduleIfNeeded();
            };
            return cb;
        };

        runtime::AppSessionOptions sessionOptions;
        sessionOptions.hudFactory = assembly.hudFactory;
        sessionOptions.detailedErrorScreen = assembly.detailedErrorScreen;
        if (assembly.live) sessionOptions.live = assembly.live(wake);
        sessionOptions.painters = std::make_shared<core::NativePainterRegistry>();
        sessionOptions.shaderCompiler = std::make_shared<d3d11::D3DShaderCompiler>();
        sessionOptions.initialGraphicsEnv = buildGraphicsEnv(renderer);
        sessionOptions.launchRequest = assembly.launchRequest;
        sessionOptions.pathsConfig = {
            assembly.source.bundleRoot,
            resolvedAppId,
            assembly.dataRootOverride,
            portableManifest,
        };

        sessionOptions.platform.onAppConfigChanged = [&](const core::AppConfigSpec& spec) {
            if (!spec.appId.empty()) {
                win32::setProcessAppId(spec.appId);
                trayManager.setAppId(spec.appId);
            }
        };
        sessionOptions.platform.onWindowCreated = [&](core::WindowId windowId, const core::WindowConfigSpec& spec) {
            auto& win = windowManager.createWindow(windowId, spec, makeWindowCallbacks(windowId));
            HostWindowRenderBinding binding;
            binding.target = renderer.createWindowTarget(win.handle());
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
            windowBadgeIcons.erase(windowId);
            windowActionIcons.erase(windowId);
            windowManager.destroyWindow(windowId);
        };
        sessionOptions.platform.onWindowCommand = [&](core::WindowId windowId, core::WindowCommandKind kind) {
            auto* win = windowManager.find(windowId);
            if (!win) return;
            switch (kind) {
                case core::WindowCommandKind::Activate:
                    win->activate();
                    break;
                case core::WindowCommandKind::RequestAttentionInformational:
                    win->requestAttention(core::WindowAttentionMode::Informational);
                    break;
                case core::WindowCommandKind::RequestAttentionCritical:
                    win->requestAttention(core::WindowAttentionMode::Critical);
                    break;
                case core::WindowCommandKind::StopAttention:
                    win->requestAttention(core::WindowAttentionMode::Stop);
                    break;
                case core::WindowCommandKind::RequestClose:
                    break;
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
        sessionOptions.platform.onTrayUpsert = [&](const core::TrayIconSpec& spec, const runtime::IconBitmap* icon) {
            void* hicon = nullptr;
            if (icon) {
                auto handle = win32::IconHandle::fromPbgra(icon->pixels.data(), icon->width, icon->height);
                hicon = handle.get();
                trayIconHandles[spec.id] = std::move(handle);
            } else {
                trayIconHandles.erase(spec.id);
            }
            trayManager.upsertTray(spec, hicon);
        };
        sessionOptions.platform.onTrayRemove = [&](const std::string& trayId) {
            trayManager.removeTray(trayId);
            trayIconHandles.erase(trayId);
        };
        sessionOptions.platform.onTrayClearAll = [&] {
            trayManager.clearAll();
            trayIconHandles.clear();
        };
        sessionOptions.platform.onTrayShowSystemMenu = [&](const std::vector<core::TrayMenuItemSpec>& items, int screenX, int screenY) -> std::uint32_t {
            return trayManager.showSystemMenu(items, screenX, screenY);
        };
        sessionOptions.platform.onTrayQueryAnchor = [&](const std::string& trayId) -> core::TrayAnchorRect {
            return trayManager.queryAnchor(trayId, 0, 0);
        };
        sessionOptions.platform.onWindowTaskbarProgress = [&](core::WindowId windowId, const core::TaskbarProgressSpec& spec) {
            if (auto* win = windowManager.find(windowId)) win->setTaskbarProgress(spec);
        };
        sessionOptions.platform.onWindowTaskbarBadge = [&](core::WindowId windowId, const runtime::IconBitmap* icon, const std::string& label) {
            auto* win = windowManager.find(windowId);
            if (!win) return;
            if (icon) {
                auto handle = win32::IconHandle::fromPbgra(icon->pixels.data(), icon->width, icon->height);
                void* hicon = handle.get();
                windowBadgeIcons[windowId] = std::move(handle);
                win->setTaskbarBadge(hicon, label);
            } else {
                windowBadgeIcons.erase(windowId);
                win->setTaskbarBadge(nullptr, {});
            }
        };
        sessionOptions.platform.onWindowTaskbarActions = [&](core::WindowId windowId, const std::vector<core::TaskbarActionSpec>& actions, const std::vector<const runtime::IconBitmap*>& icons) {
            auto* win = windowManager.find(windowId);
            if (!win) return;
            std::vector<win32::IconHandle> handles;
            std::vector<void*> rawIcons;
            handles.reserve(icons.size());
            rawIcons.reserve(icons.size());
            for (const auto* bmp : icons) {
                if (bmp) {
                    handles.push_back(win32::IconHandle::fromPbgra(bmp->pixels.data(), bmp->width, bmp->height));
                    rawIcons.push_back(handles.back().get());
                } else {
                    handles.emplace_back();
                    rawIcons.push_back(nullptr);
                }
            }
            windowActionIcons[windowId] = std::move(handles);
            win->setTaskbarActions(actions, rawIcons);
        };
        sessionOptions.platform.onMediaSessionUpdated = [&](const core::MediaSessionSpec& spec, const std::string& thumbPath) {
            mediaSession.update(spec, thumbPath);
        };
        sessionOptions.platform.onSetJumpList = [](const std::string& appId, const std::vector<core::JumpListCategorySpec>& categories) {
            win32::setJumpList(appId, categories);
        };
        sessionOptions.platform.onClearJumpList = [](const std::string& appId) {
            win32::clearJumpList(appId);
        };

        runtime::AppSession app(measurer, fonts, std::move(sessionOptions));
        appPtr = &app;
        app.setWorkAvailable(wake);
        {
            std::vector<core::LaunchRequest> queued;
            {
                const std::scoped_lock lock(preMountSecondMutex);
                queued = std::exchange(preMountSecondInstances, {});
            }
            for (auto& req : queued) app.notifySecondInstance(std::move(req));
        }

        WindowVBlankSource vblank(wake);
        runtime::VBlankFrameDriver frames(vblank);

        const auto anyExtraTickWork = [&] {
            for (const auto id : windowManager.windowIds()) {
                if (const auto* win = windowManager.find(id); win && win->needsPrecisionWheelTicks()) return true;
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
                if (std::find(changed.begin(), changed.end(), id) != changed.end()) {
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
        onSettingsChangedCallback = [&] {
            app.updateGraphicsEnvironment(buildGraphicsEnv(renderer));
            scheduleIfNeeded();
        };
        onTaskbarCreatedCallback = [&] {
            trayManager.onTaskbarCreated();
        };
        onTrayMessageCallback = [&](std::uint32_t iconId, std::uint32_t mouseMsg, int x, int y) {
            trayManager.handleTrayMessage(iconId, mouseMsg, x, y);
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

        const auto exitCode = win32::MessageDispatcher::runMessageLoop(
            [&]() -> std::optional<std::uint32_t> {
                if (exiting) return std::nullopt;
                return app.nextTimerWaitMillis(core::monotonicMillis());
            },
            [&] {
                if (exiting) return;
                runFrameTick(core::monotonicMillis());
                scheduleIfNeeded();
            });
        exiting = true;
        singleInstanceGuard.stop();
        frames.stop();
        app.setWorkAvailable({});
        app.shutdownSession();
        mediaSession.deactivate();
        trayManager.clearAll();
        trayIconHandles.clear();
        windowBadgeIcons.clear();
        windowActionIcons.clear();
        fallbackTarget.reset();
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
        assembly.launchRequest = host::captureLaunchRequest();
        code = host::runHost(std::move(assembly), startupError);
    } catch (const std::exception& error) {
        core::Log::e("Host", "宿主异常退出", error.what());
    }
    if (comReady) CoUninitialize();
    return code;
}
