#include <d3d11/Com.h>
#include <d3d11/Renderer.h>

#include <dwmapi.h>
#include <dxgi1_2.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace canton::d3d11 {
    struct VBlankThread::Impl {
        std::mutex mutex;
        std::condition_variable cv;
        std::thread thread;
        bool active = false;
        bool shutdown = false;
        std::uint64_t generation = 0;
        std::function<void()> tick;
        ComPtr<IDXGIOutput> output;

        void ensureOutputLocked() {
            if (output) return;
            ComPtr<IDXGIFactory1> factory;
            if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) {
                ComPtr<IDXGIAdapter1> adapter;
                for (UINT index = 0; !output && factory->EnumAdapters1(index, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++index) (void)adapter->EnumOutputs(0, output.ReleaseAndGetAddressOf());
            }
        }

        void run() {
            auto previous = std::chrono::steady_clock::now();
            for (;;) {
                std::uint64_t currentGeneration = 0;
                ComPtr<IDXGIOutput> localOutput;
                {
                    std::unique_lock lock(mutex);
                    cv.wait(lock, [&] { return shutdown || active; });
                    if (shutdown) return;
                    currentGeneration = generation;
                    localOutput = output;
                }
                // 优先等显示器的垂直消隐；没有输出（无显卡、远程会话）时跟随桌面合成器的节拍
                if (!localOutput || FAILED(localOutput->WaitForVBlank())) {
                    if (localOutput) {
                        std::lock_guard lock(mutex);
                        output.Reset();
                    }
                    if (FAILED(DwmFlush())) std::this_thread::sleep_for(std::chrono::milliseconds(16));
                }
                // 显示器休眠时等待会立刻返回，别让线程空转
                const auto now = std::chrono::steady_clock::now();
                if (now - previous < std::chrono::milliseconds(1)) std::this_thread::sleep_for(std::chrono::milliseconds(8));
                previous = std::chrono::steady_clock::now();
                std::lock_guard lock(mutex);
                if (shutdown || !active || generation != currentGeneration) continue;
                if (tick) tick();
            }
        }
    };

    VBlankThread::VBlankThread() : impl_(std::make_unique<Impl>()) {}

    VBlankThread::~VBlankThread() {
        {
            std::lock_guard lock(impl_->mutex);
            impl_->active = false;
            impl_->shutdown = true;
            ++impl_->generation;
            impl_->tick = {};
        }
        impl_->cv.notify_one();
        if (impl_->thread.joinable()) impl_->thread.join();
    }

    void VBlankThread::start(std::function<void()> tick) {
        {
            std::lock_guard lock(impl_->mutex);
            impl_->ensureOutputLocked();
            impl_->tick = std::move(tick);
            impl_->active = true;
            ++impl_->generation;
            if (!impl_->thread.joinable()) {
                impl_->thread = std::thread([this] { impl_->run(); });
            }
        }
        impl_->cv.notify_one();
    }

    void VBlankThread::stop() noexcept {
        std::lock_guard lock(impl_->mutex);
        impl_->active = false;
        ++impl_->generation;
        impl_->tick = {};
    }
}
