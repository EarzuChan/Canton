#include <d3d11/DWriteFontResolver.h>

#include <algorithm>
#include <cstring>

namespace canton::d3d11 {
    std::wstring widen(std::string_view utf8) {
        if (utf8.empty()) return {};
        const auto size = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        std::wstring result(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), size);
        return result;
    }

    std::string narrow(std::wstring_view utf16) {
        if (utf16.empty()) return {};
        const auto size = WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, utf16.data(), static_cast<int>(utf16.size()), result.data(), size, nullptr, nullptr);
        return result;
    }

    IDWriteFactory5* dwriteFactory() {
        static const ComPtr<IDWriteFactory5> factory = [] {
            ComPtr<IDWriteFactory5> created;
            check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory5), reinterpret_cast<IUnknown**>(created.GetAddressOf())), "无法创建 DirectWrite 工厂");
            return created;
        }();
        return factory.Get();
    }

    namespace {
        std::wstring localized(IDWriteLocalizedStrings* strings) {
            if (!strings || strings->GetCount() == 0) return {};
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (FAILED(strings->FindLocaleName(L"en-us", &index, &exists)) || !exists) index = 0;
            UINT32 length = 0;
            if (FAILED(strings->GetStringLength(index, &length))) return {};
            std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
            if (FAILED(strings->GetString(index, text.data(), length + 1))) return {};
            text.resize(length);
            return text;
        }

        std::wstring familyName(IDWriteFontFamily* family) {
            ComPtr<IDWriteLocalizedStrings> names;
            return SUCCEEDED(family->GetFamilyNames(names.GetAddressOf())) ? localized(names.Get()) : std::wstring();
        }

        const wchar_t* const defaultFamilies[] = {L"Segoe UI", L"Arial"};
    }

    FontResolver::FontResolver(const core::FontRegistry* fonts) : fonts_(fonts) {
        check(dwriteFactory()->CreateInMemoryFontFileLoader(loader_.GetAddressOf()), "无法创建内存字体加载器");
        check(dwriteFactory()->RegisterFontFileLoader(loader_.Get()), "无法登记内存字体加载器");
    }

    FontResolver::~FontResolver() {
        resolved_.clear();
        bundled_.clear();
        if (loader_) dwriteFactory()->UnregisterFontFileLoader(loader_.Get());
    }

    void FontResolver::syncGeneration() {
        const auto generation = fonts_ ? fonts_->generation() : 0;
        if (generation == generation_) return;
        generation_ = generation;
        resolved_.clear();
        bundled_.clear();
        system_.Reset();
    }

    IDWriteFontCollection* FontResolver::systemCollection() {
        // 每个代际重新取一次，系统字体安装或卸载后宿主推进代际即可生效
        if (!system_) check(dwriteFactory()->GetSystemFontCollection(system_.GetAddressOf(), TRUE), "无法读取系统字体");
        return system_.Get();
    }

    std::optional<ResolvedFont> FontResolver::faceOf(IDWriteFontCollection* collection, IDWriteFont* font, IDWriteFontFamily* family) {
        ResolvedFont result;
        result.collection = collection;
        result.family = familyName(family);
        result.weight = font->GetWeight();
        result.style = font->GetStyle();
        result.stretch = font->GetStretch();
        if (result.family.empty() || FAILED(font->CreateFontFace(result.face.GetAddressOf()))) return std::nullopt;
        result.face->GetMetrics(&result.metrics);
        return result;
    }

    const std::vector<ResolvedFont>& FontResolver::bundledFaces(const std::shared_ptr<const core::FontFamilyDefinition>& family) {
        if (const auto found = bundled_.find(family.get()); found != bundled_.end()) return found->second.second;
        std::vector<ResolvedFont> faces;
        auto* factory = dwriteFactory();
        for (const auto& source : family->faces) {
            if (!source.bytes || source.bytes->empty()) continue;
            // 每个字面单独成集合：按它自己的字重和字形请求时，DirectWrite 只能选中它，也不会再叠加模拟
            ComPtr<IDWriteFontFile> file;
            ComPtr<IDWriteFontSetBuilder1> builder;
            ComPtr<IDWriteFontSet> set;
            ComPtr<IDWriteFontCollection1> collection;
            ComPtr<IDWriteFontFamily> first;
            ComPtr<IDWriteFont> font;
            if (FAILED(loader_->CreateInMemoryFontFileReference(factory, source.bytes->data(), static_cast<UINT32>(source.bytes->size()), nullptr, file.GetAddressOf()))) continue;
            if (FAILED(factory->CreateFontSetBuilder(builder.GetAddressOf())) || FAILED(builder->AddFontFile(file.Get())) || FAILED(builder->CreateFontSet(set.GetAddressOf()))) continue;
            if (FAILED(factory->CreateFontCollectionFromFontSet(set.Get(), collection.GetAddressOf())) || collection->GetFontFamilyCount() == 0) continue;
            if (FAILED(collection->GetFontFamily(0, first.GetAddressOf())) || first->GetFontCount() == 0) continue;
            // 集合会附带模拟出来的粗体、斜体，取其中不带模拟的那个
            for (UINT32 index = 0; index < first->GetFontCount() && !font; ++index) {
                ComPtr<IDWriteFont> candidate;
                if (SUCCEEDED(first->GetFont(index, candidate.GetAddressOf())) && candidate->GetSimulations() == DWRITE_FONT_SIMULATIONS_NONE) font = candidate;
            }
            if (!font) continue;
            auto face = faceOf(collection.Get(), font.Get(), first.Get());
            if (!face) continue;
            // 覆盖项优先，否则读字节里的 OS/2
            const auto parsed = core::readFontFaceInfo({source.bytes->data(), source.bytes->size()}).value_or(core::FontFaceInfo{});
            face->info = {source.weight.value_or(parsed.weight), source.italic.value_or(parsed.italic)};
            face->bundled = true;
            faces.push_back(std::move(*face));
        }
        return bundled_.emplace(family.get(), std::make_pair(family, std::move(faces))).first->second.second;
    }

    std::vector<ResolvedFont> FontResolver::systemFaces(const std::wstring& name) {
        std::vector<ResolvedFont> faces;
        auto* collection = systemCollection();
        UINT32 index = 0;
        BOOL exists = FALSE;
        ComPtr<IDWriteFontFamily> family;
        if (FAILED(collection->FindFamilyName(name.c_str(), &index, &exists)) || !exists || FAILED(collection->GetFontFamily(index, family.GetAddressOf()))) return faces;
        for (UINT32 i = 0; i < family->GetFontCount(); ++i) {
            ComPtr<IDWriteFont> font;
            if (FAILED(family->GetFont(i, font.GetAddressOf()))) continue;
            // 模拟出来的字面与非常规宽度的字面（如 Arial Narrow）不作为可选字面
            if (font->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE || font->GetStretch() != DWRITE_FONT_STRETCH_NORMAL) continue;
            auto face = faceOf(nullptr, font.Get(), family.Get());
            if (!face) continue;
            face->info = {core::clampFontWeight(static_cast<int>(font->GetWeight())), font->GetStyle() != DWRITE_FONT_STYLE_NORMAL};
            if (std::none_of(faces.begin(), faces.end(), [&](const auto& other) { return other.info == face->info; })) faces.push_back(std::move(*face));
        }
        return faces;
    }

    std::optional<ResolvedFont> FontResolver::resolveFamily(const std::string& family, const core::TextStyle& style) {
        const auto pick = [&](const std::vector<ResolvedFont>& faces) -> std::optional<ResolvedFont> {
            std::vector<core::FontFaceInfo> infos;
            for (const auto& face : faces) infos.push_back(face.info);
            const auto choice = core::selectFontFace(infos, style.fontWeight, style.italic, style.fontSynthesis);
            if (!choice.found) return std::nullopt;
            auto result = faces[choice.index];
            result.syntheticBold = choice.syntheticBold;
            result.syntheticItalic = choice.syntheticItalic;
            return result;
        };
        // 登记过的族名只认自带字面，不再落到同名系统字体
        if (const auto definition = fonts_ ? fonts_->find(family) : nullptr) return pick(bundledFaces(definition));
        return pick(systemFaces(widen(family)));
    }

    ResolvedFont FontResolver::resolve(const core::TextStyle& style) {
        syncGeneration();
        auto key = core::joinFontFamilies(style.fontFamily);
        key += '\x1f' + std::to_string(style.fontWeight) + (style.italic ? 'i' : 'n') + (style.fontSynthesis ? 's' : 'x');
        if (const auto cached = resolved_.find(key); cached != resolved_.end()) return cached->second;
        std::optional<ResolvedFont> result;
        for (const auto& family : style.fontFamily)
            if ((result = resolveFamily(family, style))) break;
        for (const auto* family : defaultFamilies) {
            if (result) break;
            result = resolveFamily(narrow(family), style);
        }
        if (!result) {
            // 连默认字体都没有：取系统集合里的第一个字面，并按请求伪造
            auto* collection = systemCollection();
            ComPtr<IDWriteFontFamily> family;
            ComPtr<IDWriteFont> font;
            if (collection->GetFontFamilyCount() == 0 || FAILED(collection->GetFontFamily(0, family.GetAddressOf())) || FAILED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, font.GetAddressOf()))) throw std::runtime_error("系统里没有可用的字体");
            result = faceOf(nullptr, font.Get(), family.Get());
            if (!result) throw std::runtime_error("系统里没有可用的字体");
            result->syntheticBold = style.fontSynthesis && style.fontWeight >= core::syntheticBoldThreshold;
            result->syntheticItalic = style.fontSynthesis && style.italic;
        }
        resolved_.emplace(key, *result);
        return *result;
    }

    bool sameFontFace(IDWriteFontFace* left, IDWriteFontFace* right) {
        if (left == right) return true;
        if (!left || !right || left->GetIndex() != right->GetIndex()) return false;
        UINT32 leftCount = 1, rightCount = 1;
        ComPtr<IDWriteFontFile> leftFile, rightFile;
        if (FAILED(left->GetFiles(&leftCount, leftFile.GetAddressOf())) || FAILED(right->GetFiles(&rightCount, rightFile.GetAddressOf())) || !leftFile || !rightFile) return false;
        const void *leftKey = nullptr, *rightKey = nullptr;
        UINT32 leftSize = 0, rightSize = 0;
        if (FAILED(leftFile->GetReferenceKey(&leftKey, &leftSize)) || FAILED(rightFile->GetReferenceKey(&rightKey, &rightSize))) return false;
        return leftSize == rightSize && std::memcmp(leftKey, rightKey, leftSize) == 0;
    }
}
