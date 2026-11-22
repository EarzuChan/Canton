#include <runtime/PainterResources.h>

#include <runtime/UiPackage.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace canton::runtime {
    namespace {
        using Microsoft::WRL::ComPtr;

        int hexDigit(char value) {
            if (value >= '0' && value <= '9') return value - '0';
            if (value >= 'a' && value <= 'f') return value - 'a' + 10;
            if (value >= 'A' && value <= 'F') return value - 'A' + 10;
            return -1;
        }

        std::string percentDecode(std::string_view value) {
            std::string result;
            result.reserve(value.size());
            for (std::size_t index = 0; index < value.size(); ++index) {
                if (value[index] != '%')
                    result += value[index];
                else {
                    if (index + 2 >= value.size() || hexDigit(value[index + 1]) < 0 || hexDigit(value[index + 2]) < 0) throw std::runtime_error("Painter 资源含有无效的百分号编码");
                    result += static_cast<char>((hexDigit(value[index + 1]) << 4) | hexDigit(value[index + 2]));
                    index += 2;
                }
            }
            return result;
        }

        bool base64Decode(std::string_view text, std::string& out) {
            std::uint32_t buffer = 0;
            int bits = 0;
            std::size_t padding = 0;
            for (const auto c : text) {
                int value;
                if (c >= 'A' && c <= 'Z')
                    value = c - 'A';
                else if (c >= 'a' && c <= 'z')
                    value = c - 'a' + 26;
                else if (c >= '0' && c <= '9')
                    value = c - '0' + 52;
                else if (c == '+' || c == '-')
                    value = 62;
                else if (c == '/' || c == '_')
                    value = 63;
                else if (c == '=') {
                    ++padding;
                    continue;
                } else if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
                    continue;
                else
                    return false;
                if (padding) return false;
                buffer = (buffer << 6) | static_cast<std::uint32_t>(value);
                bits += 6;
                if (bits >= 8) {
                    bits -= 8;
                    out += static_cast<char>((buffer >> bits) & 0xff);
                }
            }
            return padding <= 2;
        }
    }

    std::string asciiLower(std::string_view text) {
        std::string result(text);
        for (auto& c : result)
            if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        return result;
    }

    bool isFontMime(std::string_view mime) {
        return mime.starts_with("font/") || mime.starts_with("application/font-") || mime.starts_with("application/x-font-");
    }

    bool isFontPath(std::string_view path) {
        return path.ends_with(".ttf") || path.ends_with(".otf") || path.ends_with(".ttc") || path.ends_with(".otc");
    }

    ResourcePayload dataUrlPayload(std::string_view resource, const PainterResourceLimits& limits) {
        const auto comma = resource.find(',');
        if (!resource.starts_with("data:") || comma == std::string_view::npos) throw std::runtime_error("Painter data URL 缺少内容");
        auto type = asciiLower(resource.substr(5, comma - 5));
        const auto base64 = type.ends_with(";base64");
        if (base64) type.resize(type.size() - 7);
        const auto font = isFontMime(type) || type == "application/octet-stream";
        if (!font && type != "image/svg+xml" && type != "image/png" && type != "image/jpeg" && type != "image/gif") throw std::runtime_error("Painter data URL 只支持 SVG、PNG、JPEG、GIF 图片或 TrueType、OpenType 字体");
        const auto body = resource.substr(comma + 1);
        if (body.size() > limits.maxBytes * (base64 ? 2 : 3)) throw std::runtime_error("Painter data URL 超出资源字节预算");
        ResourcePayload result;
        result.svg = type == "image/svg+xml";
        result.font = font;
        if (base64) {
            if (!base64Decode(body, result.bytes)) throw std::runtime_error("Painter data URL 的 base64 内容无效");
        } else
            result.bytes = percentDecode(body);
        if (result.bytes.size() > limits.maxBytes) throw std::runtime_error("Painter data URL 超出资源字节预算");
        return result;
    }

    std::shared_ptr<const core::FontContent> decodeFontResource(std::span<const std::uint8_t> payload, std::string_view source) {
        auto bytes = std::make_shared<std::vector<std::uint8_t>>(payload.begin(), payload.end());
        const auto info = canton::core::readFontFaceInfo({bytes->data(), bytes->size()});
        if (!info) throw std::runtime_error("字体资源不是 TrueType 或 OpenType（不支持 WOFF）：" + std::string(source));
        auto content = std::make_shared<canton::core::FontContent>();
        content->bytes = std::move(bytes);
        content->info = *info;
        return content;
    }

    namespace {
        // 解码线程各自初始化 COM；线程本来已在别的套间里时不重复收尾
        struct ComScope {
            ComScope() : owned(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}

            ~ComScope() {
                if (owned) CoUninitialize();
            }

            bool owned;
        };
    }

    std::shared_ptr<const core::BitmapContent> decodeBitmapResource(std::span<const std::uint8_t> payload, std::string_view source) {
        const auto fail = [&](const char* reason) -> std::runtime_error {
            return std::runtime_error(std::string("Painter 图片无法解码（") + reason + "）：" + std::string(source));
        };
        if (payload.empty() || payload.size() > 0xffffffffu) throw fail("内容为空或过大");
        const ComScope com;
        ComPtr<IWICImagingFactory> factory;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf())))) throw fail("无法创建 WIC 工厂");
        ComPtr<IWICStream> stream;
        if (FAILED(factory->CreateStream(stream.GetAddressOf())) || FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(payload.data()), static_cast<DWORD>(payload.size())))) throw fail("无法读取内容");
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.GetAddressOf()))) throw fail("格式不支持");
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) throw fail("没有图像帧");
        ComPtr<IWICFormatConverter> converter;
        // 直接转成预乘 BGRA，与渲染器的纹理格式一致
        if (FAILED(factory->CreateFormatConverter(converter.GetAddressOf())) || FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) throw fail("像素格式无法转换");
        UINT width = 0, height = 0;
        if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0 || width > 16384 || height > 16384) throw fail("尺寸无效");
        auto content = std::make_shared<core::BitmapContent>();
        content->width = static_cast<int>(width);
        content->height = static_cast<int>(height);
        content->pixels.resize(static_cast<std::size_t>(width) * height);
        if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(content->pixels.size() * 4), reinterpret_cast<BYTE*>(content->pixels.data())))) throw fail("像素读取失败");
        content->intrinsicSize = core::Size{static_cast<float>(width), static_cast<float>(height)};
        return content;
    }

    std::shared_ptr<const core::PainterContent> decodePainterResource(const ResourcePayload& payload, std::string_view resource) {
        const std::string_view source = payload.source.empty() ? resource : std::string_view(payload.source);
        const std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(payload.bytes.data()), payload.bytes.size());
        if (payload.font) return decodeFontResource(bytes, source);
        if (payload.svg) return decodeSvgResource(payload.bytes, source);
        return decodeBitmapResource(bytes, source);
    }

    class ResourceWorkers {
       public:
        explicit ResourceWorkers(std::size_t threads) {
            for (std::size_t i = 0; i < std::max<std::size_t>(threads, 1); ++i) threads_.emplace_back([this] { run(); });
        }

        ~ResourceWorkers() {
            {
                const std::scoped_lock lock(mutex_);
                stopping_ = true;
            }
            ready_.notify_all();
            for (auto& thread : threads_) thread.join();
        }

        void post(std::function<void()> job) {
            {
                const std::scoped_lock lock(mutex_);
                jobs_.push_back(std::move(job));
            }
            ready_.notify_one();
        }

       private:
        void run() {
            while (true) {
                std::function<void()> job;
                {
                    std::unique_lock lock(mutex_);
                    ready_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
                    // 关闭时把已排队的任务做完，等着结果的 future 才不会悬空
                    if (jobs_.empty()) return;
                    job = std::move(jobs_.front());
                    jobs_.pop_front();
                }
                job();
            }
        }

        std::mutex mutex_;
        std::condition_variable ready_;
        std::deque<std::function<void()>> jobs_;
        std::vector<std::thread> threads_;
        bool stopping_ = false;
    };

    std::shared_ptr<ResourceWorkers> createResourceWorkers(std::size_t threads) {
        return std::make_shared<ResourceWorkers>(threads);
    }

    core::PainterLoader asynchronousPainterLoader(std::shared_ptr<ResourceWorkers> workers, std::function<ResourcePayload(const std::string&)> read) {
        return [workers = std::move(workers), read = std::move(read)](const std::string& resource, std::function<void()> wake) {
            // 工作线程只持有不可变的来源配置和完成结果
            auto result = std::make_shared<std::promise<core::PainterLoadResult>>();
            auto future = result->get_future();
            workers->post([result, read, resource, wake = std::move(wake)] {
                core::PainterLoadResult loaded;
                try {
                    loaded.content = decodePainterResource(read(resource), std::string_view(resource).substr(0, 512));
                } catch (const std::exception& error) {
                    loaded.error = error.what();
                }
                result->set_value(std::move(loaded));
                if (wake) wake();
            });
            return future;
        };
    }

    core::AssetLoader asynchronousAssetLoader(std::shared_ptr<ResourceWorkers> workers, std::function<std::string(const std::string&)> read) {
        return [workers = std::move(workers), read = std::move(read)](std::string name, std::function<void()> wake) {
            auto task = std::make_shared<std::packaged_task<std::string()>>([read, name = std::move(name)] { return read(name); });
            auto future = task->get_future();
            workers->post([task, wake = std::move(wake)] {
                (*task)();
                if (wake) wake();
            });
            return future;
        };
    }

    core::PainterLoader packagePainterLoader(std::shared_ptr<const core::UiPackage> package, std::shared_ptr<ResourceWorkers> workers) {
        return asynchronousPainterLoader(std::move(workers), [package = std::move(package)](const std::string& resource) {
            if (resource.starts_with("data:")) return dataUrlPayload(resource, PainterResourceLimits{});
            const auto name = packageAssetName(resource);
            ResourcePayload result;
            result.bytes = package->read(name, core::UiEntryKind::Asset, PainterResourceLimits::MaxResourceBytes);
            const auto lower = asciiLower(name);
            result.svg = lower.ends_with(".svg");
            result.font = isFontPath(lower);
            result.source = "包内资产 " + name;
            return result;
        });
    }

    core::AssetLoader packageAssetLoader(std::shared_ptr<const core::UiPackage> package, std::shared_ptr<ResourceWorkers> workers) {
        return asynchronousAssetLoader(std::move(workers), [package = std::move(package)](const std::string& reference) { return package->read(packageAssetName(reference), core::UiEntryKind::Asset, PainterResourceLimits::MaxResourceBytes); });
    }
}
