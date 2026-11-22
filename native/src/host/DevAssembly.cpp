#include <host/HostAssembly.h>

#if !CANTON_ENABLE_DEV
#error DevAssembly.cpp 只编进 Dev 宿主
#endif

#include <core/DiagnosticsOverlay.h>
#include <core/Log.h>
#include <runtime/DiagnosticsScene.h>
#include <runtime/LiveClient.h>
#include <runtime/LiveResources.h>
#include <win32/Window.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef DrawText
#undef DrawText
#endif

#include <optional>
#include <stdexcept>
#include <vector>

namespace canton::host {
    namespace {
        constexpr std::string_view DEV_TAG = "DevDebug";

        std::optional<std::wstring> environment(const wchar_t* name) {
            const auto size = GetEnvironmentVariableW(name, nullptr, 0);
            if (size == 0) return std::nullopt;
            std::wstring value(size, L'\0');
            value.resize(GetEnvironmentVariableW(name, value.data(), size));
            return value.empty() ? std::nullopt : std::optional(std::move(value));
        }

        std::string narrow(const std::wstring& text) {
            if (text.empty()) return {};
            const auto size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
            return result;
        }

        class DevWindowControllerImpl final : public DevWindowController {
           public:
            bool handleKeyDown(int virtualKey, core::KeyModifierState modifiers, runtime::AppSession& app, core::WindowId windowId) override {
                if (const auto* err = app.globalDiagnostics().error()) {
                    if (modifiers.ctrl && (virtualKey == 'C' || virtualKey == 'c')) {
                        win32::writeClipboardText(err->diagnosticText());
                        core::Log::i(DEV_TAG, "已复制诊断信息到剪贴板");
                        return true;
                    }
                }
                if (virtualKey == VK_F5) {
                    core::Log::i(DEV_TAG, "快捷键请求重新加载 (F5)");
                    app.requestReload();
                    return true;
                }
                if (virtualKey == VK_F8 && windowId != 0) {
                    const bool next = !app.windowFpsBadgeEnabled(windowId);
                    app.setWindowFpsBadgeEnabled(windowId, next);
                    core::Log::i(DEV_TAG, "FPS 徽标", next ? "已开启" : "已关闭");
                    return true;
                }
                if (virtualKey == VK_F9 && windowId != 0) {
                    if (auto* win = app.window(windowId)) {
                        if (modifiers.shift) {
                            auto layers = overlay_.layers();
                            const bool nextGeometry = !(layers.bounds && layers.hit);
                            layers.bounds = nextGeometry;
                            layers.hit = nextGeometry;
                            overlay_.setLayers(layers);
                            overlay_.setEnabled(true);
                            win->pipeline.timeline().setEnabled(true);
                            core::Log::i(DEV_TAG, "诊断叠层几何与命中高亮", nextGeometry ? "已开启" : "已关闭");
                        } else {
                            const bool next = !overlay_.enabled();
                            overlay_.setEnabled(next);
                            win->pipeline.timeline().setEnabled(next);
                            core::Log::i(DEV_TAG, "诊断叠层", next ? "已开启" : "已关闭");
                        }
                        win->pipeline.enqueueIntent(core::InputIntent::diagnostics());
                    }
                    return true;
                }
                return false;
            }

            bool handleErrorClick(runtime::AppSession& app) override {
                if (app.globalDiagnostics().errorRetryAvailable()) {
                    core::Log::i(DEV_TAG, "错误屏点击重试");
                    app.requestReload();
                    return true;
                }
                return false;
            }

            void setPointer(std::optional<core::Point> point) noexcept override {
                overlay_.setPointer(point);
            }

            void updateOverlay(const core::FrameTimeline& timeline, const core::PublishedFrame& frame, const core::LayoutTree& tree, core::NodeId root, const core::TextLayoutService& textLayout, double nowMillis, float dpiScale) override {
                overlay_.setDpiScale(dpiScale);
                (void)overlay_.update(timeline, frame, tree, root, nowMillis);
                preparedOps_ = overlay_.ops();
                for (auto& op : preparedOps_) {
                    if (op.type == core::DrawOpType::DrawText) {
                        core::DrawOpsBuilder::prepareText(op, textLayout);
                    }
                }
            }

            std::span<const core::DrawOp> overlayOps() const noexcept override {
                return preparedOps_;
            }

            bool animating() const noexcept override {
                return overlay_.enabled() && overlay_.animating();
            }

           private:
            core::DiagnosticsOverlay overlay_;
            std::vector<core::DrawOp> preparedOps_;
        };
    }

    // Dev 宿主：加载规矩经 CANTON_DEV_* 环境变量注入，不占用命令行
    HostAssembly assembleHost(const std::filesystem::path& bundleRoot) {
        HostAssembly assembly;
        assembly.source.bundleRoot = bundleRoot;
        assembly.source.policy = runtime::LoadPolicy::LiveThenDist;
        if (const auto source = environment(L"CANTON_DEV_SOURCE")) {
            const auto policy = runtime::parseLoadPolicy(narrow(*source));
            if (!policy) throw std::runtime_error("CANTON_DEV_SOURCE 只接受 lfd、live 或 dist，收到的是 " + narrow(*source));
            assembly.source.policy = *policy;
        }
        assembly.source.devServerUrl = narrow(environment(L"CANTON_DEV_SERVER").value_or(L"http://127.0.0.1:9178"));
        if (const auto dist = environment(L"CANTON_DEV_DIST")) assembly.source.distOverride = *dist;
        if (const auto file = environment(L"CANTON_DEV_READY_FILE")) {
            assembly.readyFile = *file;
            assembly.readyToken = narrow(environment(L"CANTON_DEV_READY_TOKEN").value_or(L""));
        }
        assembly.hudFactory = [] {
            runtime::DiagnosticsConfig diagnostics;
            diagnostics.badge = runtime::DiagnosticVisibility::Always;
            diagnostics.toasts = runtime::DiagnosticVisibility::Always;
            diagnostics.overlay = runtime::DiagnosticVisibility::Always;
            return runtime::createDiagnosticsHud(diagnostics);
        };
        assembly.devControllerFactory = [] {
            return std::make_unique<DevWindowControllerImpl>();
        };
        assembly.detailedErrorScreen = true;
        assembly.live = [workers = runtime::createResourceWorkers()](std::function<void()> wake) {
            return runtime::makeLiveSessionFactory(std::move(wake), [workers](std::string_view devServerUrl) {
                return runtime::RuntimeLoadResources{runtime::livePainterLoader(std::string(devServerUrl), {}, workers), runtime::liveAssetLoader(std::string(devServerUrl), workers)};
            });
        };
        return assembly;
    }
}
