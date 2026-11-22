#pragma once

#include <core/Font.h>
#include <core/Paint.h>
#include <core/SceneFramePipeline.h>

#include <wincodec.h>
#include <wrl/client.h>
// windows.h 把 DrawText 定义成宏，与 core 的 DrawOpType::DrawText 撞名
#undef DrawText

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

// 渲染测试共用：PNG 读写、图像比对、测试字体与手工拼的绘制片段
namespace test_support {
    struct Image {
        int width = 0;
        int height = 0;
        // 0xAARRGGBB，行优先
        std::vector<std::uint32_t> pixels;
    };

    struct ComApartment {
        ComApartment() : owned(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}

        ~ComApartment() {
            if (owned) CoUninitialize();
        }

        bool owned;
    };

    inline void require(HRESULT result, const char* what) {
        if (FAILED(result)) throw std::runtime_error(what);
    }

    inline void writePng(const std::filesystem::path& path, const Image& image) {
        using Microsoft::WRL::ComPtr;
        std::filesystem::create_directories(path.parent_path());
        ComPtr<IWICImagingFactory> factory;
        require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf())), "无法创建 WIC 工厂");
        ComPtr<IWICStream> stream;
        require(factory->CreateStream(stream.GetAddressOf()), "无法创建 WIC 流");
        require(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "无法写入 PNG 文件");
        ComPtr<IWICBitmapEncoder> encoder;
        require(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()), "无法创建 PNG 编码器");
        require(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "无法初始化 PNG 编码器");
        ComPtr<IWICBitmapFrameEncode> frame;
        require(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr), "无法创建 PNG 帧");
        require(frame->Initialize(nullptr), "无法初始化 PNG 帧");
        require(frame->SetSize(static_cast<UINT>(image.width), static_cast<UINT>(image.height)), "无法设置 PNG 尺寸");
        auto format = GUID_WICPixelFormat32bppBGRA;
        require(frame->SetPixelFormat(&format), "无法设置 PNG 像素格式");
        auto pixels = image.pixels;
        require(frame->WritePixels(static_cast<UINT>(image.height), static_cast<UINT>(image.width * 4), static_cast<UINT>(pixels.size() * 4), reinterpret_cast<BYTE*>(pixels.data())), "无法写入 PNG 像素");
        require(frame->Commit(), "无法提交 PNG 帧");
        require(encoder->Commit(), "无法提交 PNG");
    }

    inline Image readPng(const std::filesystem::path& path) {
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf())), "无法创建 WIC 工厂");
        ComPtr<IWICBitmapDecoder> decoder;
        require(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()), "无法读取 PNG 文件");
        ComPtr<IWICBitmapFrameDecode> frame;
        require(decoder->GetFrame(0, frame.GetAddressOf()), "无法读取 PNG 帧");
        ComPtr<IWICFormatConverter> converter;
        require(factory->CreateFormatConverter(converter.GetAddressOf()), "无法创建格式转换器");
        require(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom), "无法转换 PNG 像素格式");
        UINT width = 0, height = 0;
        require(converter->GetSize(&width, &height), "无法读取 PNG 尺寸");
        Image image{static_cast<int>(width), static_cast<int>(height), std::vector<std::uint32_t>(static_cast<std::size_t>(width) * height)};
        require(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(image.pixels.size() * 4), reinterpret_cast<BYTE*>(image.pixels.data())), "无法读取 PNG 像素");
        return image;
    }

    struct Difference {
        // 任一通道差值超过容差的像素数
        std::size_t exceeding = 0;
        int largest = 0;
        bool sizeMismatch = false;
    };

    // diff 里把超出容差的像素标成红色，其余按原图压暗
    inline Difference compareImages(const Image& expected, const Image& actual, int tolerance, Image* diff = nullptr) {
        Difference result;
        if (expected.width != actual.width || expected.height != actual.height) {
            result.sizeMismatch = true;
            result.exceeding = static_cast<std::size_t>(actual.width) * static_cast<std::size_t>(actual.height);
            return result;
        }
        if (diff) *diff = {actual.width, actual.height, std::vector<std::uint32_t>(actual.pixels.size())};
        for (std::size_t i = 0; i < actual.pixels.size(); ++i) {
            int delta = 0;
            for (int shift = 0; shift < 32; shift += 8) delta = std::max(delta, std::abs(static_cast<int>((expected.pixels[i] >> shift) & 0xff) - static_cast<int>((actual.pixels[i] >> shift) & 0xff)));
            result.largest = std::max(result.largest, delta);
            if (delta > tolerance) ++result.exceeding;
            if (diff) diff->pixels[i] = delta > tolerance ? 0xffff0000u : (0xff000000u | ((actual.pixels[i] >> 2) & 0x003f3f3fu));
        }
        return result;
    }

    inline std::vector<std::uint8_t> readFile(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("无法读取文件 " + path.string());
        return {std::istreambuf_iterator<char>(input), {}};
    }

    inline constexpr const char* testFontFamily = "Canton Test Sans";

    // 把自带测试字体的三个字面登记进注册表
    inline void defineTestFonts(canton::core::FontRegistry& fonts, const std::filesystem::path& directory, const void* owner = nullptr) {
        canton::core::FontFamilyDefinition family;
        family.name = testFontFamily;
        for (const auto* name : {"CantonTestSans-Regular.ttf", "CantonTestSans-Bold.ttf", "CantonTestSans-Italic.ttf"}) family.faces.push_back({std::make_shared<const std::vector<std::uint8_t>>(readFile(directory / name)), std::nullopt, std::nullopt});
        fonts.define(owner, std::move(family));
    }

    // 手工拼的绘制片段没有经过边界计算，标成未知以免被剔除
    inline canton::core::PlacedPaintFragment fragmentOf(std::vector<canton::core::DrawOp> ops) {
        canton::core::PaintFragment fragment;
        fragment.content = std::make_shared<const std::vector<canton::core::DrawOp>>(std::move(ops));
        fragment.bounds = {{}, false, false};
        return {std::make_shared<const canton::core::PaintFragment>(std::move(fragment)), {}};
    }
}
