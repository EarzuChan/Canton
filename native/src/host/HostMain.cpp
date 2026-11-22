#include <host/HostAssembly.h>

#include <core/Clock.h>
#include <core/Log.h>
#include <d3d11/DWriteText.h>
#include <d3d11/Renderer.h>
#include <runtime/VBlankSource.h>
#include <win32/ConsoleLog.h>
#include <win32/Window.h>

#include <objbase.h>

#include <atomic>
#include <fstream>
#include <optional>

namespace canton::host {
    namespace {
        constexpr std::string_view TAG = "Host";

        // 把渲染库的垂直同步线程接成 runtime 的 VBlankSource：节拍在后台线程产生，回调在窗口线程执行
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

            // 在窗口线程上调用：有节拍就交给回调
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
            // 先写临时文件再改名，等待方读不到写了一半的令牌
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
    }

    int runHost(HostAssembly assembly, const std::string& startupError) {
        const auto fonts = std::make_shared<core::FontRegistry>();
        const d3d11::DWriteTextMeasurer measurer(fonts.get());
        d3d11::Renderer renderer;
        core::Log::i(TAG, "显示适配器", renderer.adapter().description, renderer.adapter().software ? "（软件渲染）" : "");

        std::function<void()> onWake, onPaint, onFontsChanged;
        std::function<void(int, int)> onResize;
        win32::Window window(L"Canton", 960, 640,
                             {[&](int width, int height) {
                                  if (onResize) onResize(width, height);
                              },
                              [&] {
                                  if (onPaint) onPaint();
                              },
                              [&] {
                                  if (onWake) onWake();
                              },
                              [&] {
                                  if (onFontsChanged) onFontsChanged();
                              }});
        const auto wake = [&window] {
            window.postWake();
        };

        runtime::SceneHostOptions options;
        options.hud = std::move(assembly.hud);
        options.detailedErrorScreen = assembly.detailedErrorScreen;
        if (assembly.live) options.live = assembly.live(wake);
        options.painters = std::make_shared<core::NativePainterRegistry>();
        runtime::SceneHost scene(measurer, fonts, std::move(options));
        const auto target = renderer.createWindowTarget(window.handle());
        WindowVBlankSource vblank(wake);
        runtime::VBlankFrameDriver frames(vblank);
        bool firstFrameShown = false;
        bool failed = false;

        const auto render = [&] {
            renderer.render(*target, {&scene.publishedFrame().content});
        };
        const auto frame = [&](double nowMillis) {
            if (failed) return;
            try {
                (void)scene.pumpFrame(nowMillis);
                render();
                if (!firstFrameShown && scene.ready()) {
                    firstFrameShown = true;
                    core::Log::i(TAG, "首帧已呈现", runtime::packageSourceLabel(scene.source().activeSource()));
                    if (!assembly.readyFile.empty()) writeReadyFile(assembly.readyFile, assembly.readyToken);
                }
            } catch (const std::exception& error) {
                // 渲染本身坏了，错误屏也画不出来：记日志并停下，窗口保持可关闭
                failed = true;
                core::Log::e(TAG, "帧执行失败，已停止渲染", error.what());
            }
            // 没有待办就停掉节拍，空闲时不占 CPU；有新工作时由唤醒重新启动
            if (failed || !scene.wantsFrame()) frames.stop();
        };
        const auto schedule = [&] {
            if (!failed && !frames.active()) frames.start(frame);
        };
        onWake = [&] {
            const auto now = core::monotonicMillis();
            vblank.deliver(now);
            // 后台线程（资源、Live）交回的工作在下一拍处理
            schedule();
        };
        onResize = [&](int width, int height) {
            scene.resize(width, height);
            renderer.resize(*target, width, height);
            // 拖动边框时消息循环被系统接管，这里直接出一帧，画面跟手
            frame(core::monotonicMillis());
            schedule();
        };
        onPaint = [&] {
            if (failed) return;
            try {
                render();
            } catch (const std::exception& error) {
                core::Log::e(TAG, "重画失败", error.what());
            }
        };
        onFontsChanged = [&] {
            fonts->notifyChanged();
            schedule();
        };

        scene.setWorkAvailable(wake);
        int width = 0, height = 0;
        window.clientSize(width, height);
        scene.resize(width, height);
        if (startupError.empty())
            scene.configure(assembly.source);
        else {
            scene.diagnostics().setError(runtime::makeErrorScreenModel(runtime::ErrorSource::AppPackage, startupError, {}, {}, false));
            scene.runtime().enqueueIntent(core::InputIntent::diagnostics());
        }
        window.show();
        schedule();
        const auto code = win32::Window::runMessageLoop();
        frames.stop();
        scene.setWorkAvailable({});
        onWake = {}, onPaint = {}, onResize = {}, onFontsChanged = {};
        return code;
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    using namespace canton;
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
            // 装配不了也要开窗，把原因显示在错误屏上
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
