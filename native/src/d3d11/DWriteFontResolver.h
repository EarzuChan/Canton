#pragma once

#include <d3d11/DWriteText.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace canton::d3d11 {
    struct ResolvedFont {
        // 自带字体是只含这一个字面的集合；系统字体为空，表示用系统集合
        ComPtr<IDWriteFontCollection> collection;
        std::wstring family;
        DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
        DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
        DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
        ComPtr<IDWriteFontFace> face;
        DWRITE_FONT_METRICS metrics{};
        core::FontFaceInfo info;
        bool bundled = false;
        bool syntheticBold = false;
        bool syntheticItalic = false;
    };

    // 按 Canton 的规则选字面：族表逐个查，自带字体优先于同名系统字体；字重与斜体由 core::selectFontFace 定
    class FontResolver {
       public:
        explicit FontResolver(const core::FontRegistry* fonts);
        ~FontResolver();

        // 调用方负责串行化
        [[nodiscard]] ResolvedFont resolve(const core::TextStyle& style);

       private:
        void syncGeneration();
        IDWriteFontCollection* systemCollection();
        static std::optional<ResolvedFont> faceOf(IDWriteFontCollection* collection, IDWriteFont* font, IDWriteFontFamily* family);
        std::optional<ResolvedFont> resolveFamily(const std::string& family, const core::TextStyle& style);
        const std::vector<ResolvedFont>& bundledFaces(const std::shared_ptr<const core::FontFamilyDefinition>& family);
        std::vector<ResolvedFont> systemFaces(const std::wstring& family);

        const core::FontRegistry* fonts_;
        std::uint64_t generation_ = ~std::uint64_t{0};
        ComPtr<IDWriteInMemoryFontFileLoader> loader_;
        ComPtr<IDWriteFontCollection> system_;
        std::unordered_map<std::string, ResolvedFont> resolved_;
        std::unordered_map<const core::FontFamilyDefinition*, std::pair<std::shared_ptr<const core::FontFamilyDefinition>, std::vector<ResolvedFont>>> bundled_;
    };

    // 两个字面对象是否指向同一个字体文件里的同一个字面
    [[nodiscard]] bool sameFontFace(IDWriteFontFace* left, IDWriteFontFace* right);
}
