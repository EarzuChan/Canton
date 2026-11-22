#include <d3d11/Com.h>
#include <d3d11/Renderer.h>

#include <dwmapi.h>
#include <dxgi1_2.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace canton::d3d11 {
    struct VBlankThread::Impl {
        std::thread thread;
        std::atomic<bool> running{false};
        std::function<void()> tick;
        ComPtr<IDXGIOutput> output;

        void run() {
            auto previous = std::chrono::steady_clock::now();
            while (running.load(std::memory_order_acquire)) {
                // 优先等显示器的垂直消隐；没有输出（无显卡、远程会话）时跟随桌面合成器的节拍
                if (!output || FAILED(output->WaitForVBlank())) {
                    if (FAILED(DwmFlush())) std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
                // 显示器休眠时等待会立刻返回，别让线程空转
                const auto now = std::chrono::steady_clock::now();
                if (now - previous < std::chrono::milliseconds(1)) std::this_thread::sleep_for(std::chrono::milliseconds(8));
                previous = std::chrono::steady_clock::now();
                if (running.load(std::memory_order_acquire) && tick) tick();
            }
        }
    };

    VBlankThread::VBlankThread() : impl_(std::make_unique<Impl>()) {}

    VBlankThread::~VBlankThread() {
        stop();
    }

    void VBlankThread::start(std::function<void()> tick) {
        stop();
        impl_->tick = std::move(tick);
        impl_->output.Reset();
        ComPtr<IDXGIFactory1> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) {
            ComPtr<IDXGIAdapter1> adapter;
            for (UINT index = 0; !impl_->output && factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++index) (void)adapter->EnumOutputs(0, impl_->output.ReleaseAndGetAddressOf());
        }
        impl_->running.store(true, std::memory_order_release);
        impl_->thread = std::thread([this] { impl_->run(); });
    }

    void VBlankThread::stop() noexcept {
        impl_->running.store(false, std::memory_order_release);
        if (impl_->thread.joinable()) impl_->thread.join();
        impl_->tick = {};
    }
}
