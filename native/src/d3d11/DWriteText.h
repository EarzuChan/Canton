#pragma once

#include <core/Font.h>
#include <core/TextLayoutService.h>
#include <d3d11/Com.h>

#include <dwrite_3.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace canton::d3d11 {
    // 进程内共用的 DirectWrite 工厂
    [[nodiscard]] IDWriteFactory5* dwriteFactory();

    // 一段同字面的字形，位置是相对排版原点的基线笔位置
    struct GlyphRun {
        ComPtr<IDWriteFontFace> face;
        float emSize = 0;
        std::size_t line = 0;
        // 所在行的基线，伪斜体绕它错切
        float baseline = 0;
        bool syntheticBold = false;
        bool syntheticItalic = false;
        std::vector<std::uint16_t> glyphs;
        std::vector<core::Point> positions;
    };

    // 排版结果里供渲染器取用的字形；渲染器按字面、字号和变换把它们栅格化进图集
    class DWriteTextResource final : public core::TextDrawResource {
       public:
        std::vector<GlyphRun> runs;
        [[nodiscard]] std::size_t estimatedBytes() const noexcept override;
    };

    // DirectWrite 排版。字面由 Canton 按 CSS 规则选定：先查字体注册表里的自带字体，再查系统字体；
    // 选中字面里没有的字符由 DirectWrite 的系统回落补上
    class DWriteTextMeasurer final : public core::TextMeasurer {
       public:
        // 注册表须与测量器同寿命或更长；为空时只用系统字体
        explicit DWriteTextMeasurer(const core::FontRegistry* fonts = nullptr);
        ~DWriteTextMeasurer() override;
        DWriteTextMeasurer(const DWriteTextMeasurer&) = delete;
        DWriteTextMeasurer& operator=(const DWriteTextMeasurer&) = delete;

        core::TextLayout createLayout(std::string_view text, core::TextStyle style, core::TextLayoutOptions options) const override;

        // 选中字面的族名与是否伪造，供测试断言回落次序
        struct ResolvedFace {
            std::string family;
            int weight = core::defaultFontWeight;
            bool italic = false;
            bool bundled = false;
            bool syntheticBold = false;
            bool syntheticItalic = false;
        };

        [[nodiscard]] ResolvedFace resolve(const core::TextStyle& style) const;

       private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
