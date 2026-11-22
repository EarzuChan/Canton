#pragma once

#include <core/Font.h>
#include <core/Painter.h>
#include <core/UiPackage.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace canton::runtime {
    struct PainterResourceLimits {
        static constexpr std::size_t MaxResourceBytes = 16 * 1024 * 1024;
        static constexpr int MaxTimeoutMs = 5000;
        std::size_t maxBytes = MaxResourceBytes;
        int timeoutMs = MaxTimeoutMs;
    };

    // 取回来还没解码的资源字节
    struct ResourcePayload {
        std::string bytes;
        bool svg = false;
        // 字体字节：由 defineFontFamily 经同一条资源获取链路取得
        bool font = false;
        // 出现在诊断里的来源描述，空则用资源地址
        std::string source;
    };

    [[nodiscard]] bool isFontMime(std::string_view mime);
    [[nodiscard]] bool isFontPath(std::string_view lowerCasePath);
    [[nodiscard]] std::string asciiLower(std::string_view text);

    std::shared_ptr<const core::FontContent> decodeFontResource(std::span<const std::uint8_t> payload, std::string_view source);
    // PNG、JPEG 等位图经 WIC 解成预乘位图
    std::shared_ptr<const core::BitmapContent> decodeBitmapResource(std::span<const std::uint8_t> payload, std::string_view source);
    // SVG：禁止 DOCTYPE、实体与外部引用，视口尺寸必须有效
    std::shared_ptr<const core::VectorContent> decodeSvgResource(std::string_view text, std::string_view source);
    std::shared_ptr<const core::PainterContent> decodePainterResource(const ResourcePayload& payload, std::string_view resource);

    // data: URL，只接受 SVG、PNG、JPEG、GIF 图片与 TrueType、OpenType 字体
    [[nodiscard]] ResourcePayload dataUrlPayload(std::string_view resource, const PainterResourceLimits& limits);

    // 后台线程池：读取与解码在这里进行，完成后经 wake 通知所有者线程。工作线程不碰脚本上下文和场景
    class ResourceWorkers;
    [[nodiscard]] std::shared_ptr<ResourceWorkers> createResourceWorkers(std::size_t threads = 2);

    [[nodiscard]] core::PainterLoader asynchronousPainterLoader(std::shared_ptr<ResourceWorkers> workers, std::function<ResourcePayload(const std::string&)> read);
    [[nodiscard]] core::AssetLoader asynchronousAssetLoader(std::shared_ptr<ResourceWorkers> workers, std::function<std::string(const std::string&)> read);

    // Dist：从包里读
    [[nodiscard]] core::PainterLoader packagePainterLoader(std::shared_ptr<const core::UiPackage> package, std::shared_ptr<ResourceWorkers> workers = createResourceWorkers());
    [[nodiscard]] core::AssetLoader packageAssetLoader(std::shared_ptr<const core::UiPackage> package, std::shared_ptr<ResourceWorkers> workers = createResourceWorkers());
}
