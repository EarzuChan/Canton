#pragma once

#include "RenderSupport.h"

#include <core/TextLayoutService.h>
#include <d3d11/Renderer.h>

#include <functional>
#include <span>
#include <string>
#include <vector>

// 无头回归：每个用例离屏画一张图，与进 Git 的基线比对
namespace render_regression {
    using canton::core::DrawOp;
    using test_support::Image;

    struct CaseContext {
        canton::d3d11::Renderer& renderer;
        canton::core::FontRegistry& fonts;
        const canton::core::TextLayoutService& text;
        // 构建好的 .cta 夹具所在目录
        std::filesystem::path fixtures;
        std::filesystem::path sources;
        int width = 0;
        int height = 0;

        [[nodiscard]] Image render(const canton::core::PublishedFrameContent& content, std::span<const DrawOp> overlay = {}) const {
            auto target = renderer.createOffscreenTarget(width, height);
            renderer.render(*target, {&content, overlay});
            return {width, height, renderer.readPixels(*target)};
        }

        // 直接喂绘制指令；文字指令在这里排版
        [[nodiscard]] Image renderOps(std::vector<DrawOp> ops) const {
            for (auto& op : ops) canton::core::DrawOpsBuilder::prepareText(op, text);
            canton::core::PublishedFrameContent content;
            content.scenePaint = test_support::fragmentOf(std::move(ops));
            content.viewport = {static_cast<float>(width), static_cast<float>(height)};
            return render(content);
        }
    };

    struct Case {
        std::string name;
        int width = 0;
        int height = 0;
        std::function<Image(CaseContext&)> run;
        bool compareBaseline = true;
    };

    void addDrawCases(std::vector<Case>& cases);
    void addDiagnosticsCases(std::vector<Case>& cases);
    void addFixtureCases(std::vector<Case>& cases);
}
