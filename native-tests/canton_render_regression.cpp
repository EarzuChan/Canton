#include "RenderRegression.h"

#include <d3d11/DWriteText.h>

#include <iostream>
#include <optional>

// 无头回归程序。每个用例跑两档适配器：WARP 与基线逐像素比对（各机器结果一致），
// 硬件显卡按容差与同一张基线比对；没有硬件显卡时跳过这一档并写明。
// 基线只在带 --update 时写入，普通运行绝不覆盖。
namespace {
    using namespace render_regression;

    // 容差按实测定，见 docs/proj/m3.md 的施工笔记
    constexpr int warpTolerance = 0;
    constexpr int hardwareTolerance = 2;
    // 硬件档允许超出容差的像素占比
    constexpr double hardwareExceedingRatio = 0.0005;

    struct Options {
        std::filesystem::path baselines;
        std::filesystem::path output;
        std::filesystem::path fixtures;
        std::filesystem::path sources = CANTON_SOURCE_DIR;
        std::string only;
        bool update = false;
        bool list = false;
    };

    Options parse(int argc, wchar_t** argv) {
        Options options;
        for (int i = 1; i < argc; ++i) {
            const std::wstring argument = argv[i];
            const auto value = [&]() -> std::filesystem::path {
                if (i + 1 >= argc) throw std::runtime_error("参数缺少取值");
                return argv[++i];
            };
            if (argument == L"--baselines")
                options.baselines = value();
            else if (argument == L"--output")
                options.output = value();
            else if (argument == L"--fixtures")
                options.fixtures = value();
            else if (argument == L"--only")
                options.only = value().string();
            else if (argument == L"--update")
                options.update = true;
            else if (argument == L"--list")
                options.list = true;
            else
                throw std::runtime_error("未知参数");
        }
        if (!options.list && (options.baselines.empty() || options.output.empty())) throw std::runtime_error("使用方法：canton_render_regression --baselines <目录> --output <目录> [--fixtures <目录>] [--only <用例>] [--update] [--list]");
        return options;
    }

    Image runCase(const Case& item, canton::d3d11::Renderer& renderer, const Options& options) {
        // 每个用例一套独立的字体环境，互不影响
        canton::core::FontRegistry fonts;
        test_support::defineTestFonts(fonts, options.sources / "tests/fixtures/fonts");
        const canton::d3d11::DWriteTextMeasurer measurer(&fonts);
        const canton::core::TextLayoutService text(measurer, 256, 8 * 1024 * 1024, &fonts);
        CaseContext context{renderer, fonts, text, options.fixtures, options.sources, item.width, item.height};
        return item.run(context);
    }

    int run(int argc, wchar_t** argv) {
        const auto options = parse(argc, argv);
        std::vector<Case> cases;
        addDrawCases(cases);
        addDiagnosticsCases(cases);
        // 夹具用例要先构建夹具；没给目录时只跑直接喂指令的用例
        if (!options.fixtures.empty()) addFixtureCases(cases);
        if (options.list) {
            for (const auto& item : cases) std::cout << item.name << '\n';
            return 0;
        }
        const test_support::ComApartment apartment;
        canton::d3d11::Renderer warp({canton::d3d11::AdapterPreference::Warp});
        std::optional<canton::d3d11::Renderer> hardware;
        try {
            hardware.emplace(canton::d3d11::RendererOptions{canton::d3d11::AdapterPreference::Hardware});
            std::cout << "[硬件档] " << hardware->adapter().description << '\n';
        } catch (const canton::d3d11::AdapterUnavailable& error) {
            std::cout << "[硬件档] 跳过：" << error.what() << '\n';
        }
        std::cout << "[WARP 档] " << warp.adapter().description << '\n';

        int failures = 0, ran = 0;
        for (const auto& item : cases) {
            if (!options.only.empty() && item.name != options.only) continue;
            ++ran;
            if (!item.compareBaseline) {
                try {
                    (void)runCase(item, warp, options);
                    std::cout << "PASS " << item.name << " [warp]\n";
                } catch (const std::exception& error) {
                    std::cout << "FAIL " << item.name << " " << error.what() << '\n';
                    ++failures;
                }
                continue;
            }
            const auto baselinePath = options.baselines / (item.name + ".png");
            const auto report = [&](const char* tier, const Image& actual, int tolerance, double ratio) {
                test_support::writePng(options.output / (item.name + "." + tier + ".png"), actual);
                if (!std::filesystem::exists(baselinePath)) {
                    std::cout << "FAIL " << item.name << " [" << tier << "] 缺少基线，先显式更新基线\n";
                    return false;
                }
                Image diff;
                const auto difference = test_support::compareImages(test_support::readPng(baselinePath), actual, tolerance, &diff);
                const auto allowed = static_cast<std::size_t>(ratio * static_cast<double>(actual.pixels.size()));
                if (difference.sizeMismatch || difference.exceeding > allowed) {
                    if (!difference.sizeMismatch) test_support::writePng(options.output / (item.name + "." + tier + ".diff.png"), diff);
                    std::cout << "FAIL " << item.name << " [" << tier << "] " << (difference.sizeMismatch ? "尺寸与基线不符" : "超出容差的像素 " + std::to_string(difference.exceeding) + "，最大通道差 " + std::to_string(difference.largest)) << '\n';
                    return false;
                }
                std::cout << "PASS " << item.name << " [" << tier << "] 最大通道差 " << difference.largest << "，超出容差的像素 " << difference.exceeding << '\n';
                return true;
            };
            try {
                const auto image = runCase(item, warp, options);
                if (options.update) {
                    const auto changed = !std::filesystem::exists(baselinePath) || test_support::compareImages(test_support::readPng(baselinePath), image, 0).exceeding > 0;
                    if (changed) test_support::writePng(baselinePath, image);
                    std::cout << (changed ? "UPDATED " : "UNCHANGED ") << item.name << '\n';
                } else if (!report("warp", image, warpTolerance, 0))
                    ++failures;
                if (hardware && !report("hardware", runCase(item, *hardware, options), hardwareTolerance, hardwareExceedingRatio)) {
                    if (!options.update) ++failures;
                }
            } catch (const std::exception& error) {
                std::cout << "FAIL " << item.name << " 抛出异常：" << error.what() << '\n';
                ++failures;
            }
        }
        if (ran == 0) {
            std::cout << "没有匹配的用例\n";
            return 2;
        }
        std::cout << (failures ? "无头回归失败 " : "无头回归通过 ") << ran - failures << "/" << ran << (hardware ? "" : "（硬件档已跳过）") << '\n';
        return failures ? 1 : 0;
    }
}

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}
