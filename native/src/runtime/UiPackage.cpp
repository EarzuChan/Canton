#include <runtime/UiPackage.h>

#include <core/Version.h>
#include <quickjs/QuickJsBuild.h>
#include <runtime/Digest.h>
#include <runtime/Json.h>
#include <runtime/ZipArchive.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <charconv>
#include <cmath>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace canton::runtime {
    namespace {
        bool validUtf8(std::string_view bytes) {
            while (!bytes.empty()) {
                const auto lead = static_cast<unsigned char>(bytes.front());
                bytes.remove_prefix(1);
                if (lead < 0x80) continue;
                if (lead < 0xc2 || lead > 0xf4) return false;
                const auto count = lead < 0xe0 ? 1u : lead < 0xf0 ? 2u : 3u;
                if (bytes.size() < count) return false;
                const auto first = static_cast<unsigned char>(bytes.front());
                if ((lead == 0xe0 && first < 0xa0) || (lead == 0xed && first > 0x9f) || (lead == 0xf0 && first < 0x90) || (lead == 0xf4 && first > 0x8f)) return false;
                for (const unsigned char c : bytes.substr(0, count))
                    if ((c & 0xc0) != 0x80) return false;
                bytes.remove_prefix(count);
            }
            return true;
        }

        [[noreturn]] void fail(std::string_view reason, std::string_view name = {}) {
            throw std::runtime_error("UI 包：" + std::string(reason) + (name.empty() ? "" : "：" + std::string(name)));
        }

        bool portableName(std::string_view name) {
            if (name.empty() || name.size() > 1024 || !validUtf8(name)) return false;
            for (const unsigned char c : name)
                if (c < 32 || c == 127 || std::string_view("\\:*?\"<>|").find(static_cast<char>(c)) != std::string_view::npos) return false;
            while (true) {
                const auto slash = name.find('/');
                const auto part = name.substr(0, slash);
                if (part.empty() || part.back() == '.' || part.back() == ' ') return false;
                std::string stem(part.substr(0, part.find('.')));
                for (auto& c : stem)
                    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul") return false;
                if (stem.starts_with("com") || stem.starts_with("lpt")) {
                    const std::string_view suffix(stem.data() + 3, stem.size() - 3);
                    if ((suffix.size() == 1 && suffix.front() >= '1' && suffix.front() <= '9') || suffix == "¹" || suffix == "²" || suffix == "³") return false;
                }
                if (slash == std::string_view::npos) return true;
                name.remove_prefix(slash + 1);
            }
        }

    }

    namespace {
        constexpr std::size_t manifestLimit = 4 * 1024 * 1024;
        constexpr std::size_t entryCountLimit = 8192;

        std::filesystem::path utf8Path(std::string_view name) {
            return std::filesystem::path(std::u8string(name.begin(), name.end()));
        }

        bool redirected(const std::filesystem::path& path) {
            const auto flags = GetFileAttributesW(path.c_str());
            return flags != INVALID_FILE_ATTRIBUTES && (flags & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        }

        // 重名判定用的折叠：NFC 规范化后转小写，与构建侧的 normalize('NFC').toLowerCase() 对应
        std::string folded(std::string_view name) {
            if (name.empty()) return {};
            const auto wideSize = MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
            std::wstring wide(static_cast<std::size_t>(wideSize), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), wide.data(), wideSize);
            const auto normalizedSize = NormalizeString(NormalizationC, wide.data(), static_cast<int>(wide.size()), nullptr, 0);
            if (normalizedSize > 0) {
                std::wstring normalized(static_cast<std::size_t>(normalizedSize), L'\0');
                const auto written = NormalizeString(NormalizationC, wide.data(), static_cast<int>(wide.size()), normalized.data(), normalizedSize);
                if (written > 0) {
                    normalized.resize(static_cast<std::size_t>(written));
                    wide = std::move(normalized);
                }
            }
            const auto lowerSize = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr, 0);
            if (lowerSize > 0) {
                std::wstring lower(static_cast<std::size_t>(lowerSize), L'\0');
                LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, wide.data(), static_cast<int>(wide.size()), lower.data(), lowerSize, nullptr, nullptr, 0);
                wide = std::move(lower);
            }
            const auto size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
            std::string result(static_cast<std::size_t>(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
            return result;
        }

        bool endsWithIgnoringCase(std::string_view text, std::string_view suffix) {
            if (text.size() < suffix.size()) return false;
            for (std::size_t i = 0; i < suffix.size(); ++i) {
                auto c = text[text.size() - suffix.size() + i];
                if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
                if (c != suffix[i]) return false;
            }
            return true;
        }

        // JSON 数是双精度；清单里的整数字段必须是不带小数的非负数
        std::optional<std::uint64_t> wholeNumber(const JsonValue* value) {
            const auto* number = jsonNumber(value);
            if (!number || !(*number >= 0) || *number > 9.0e15 || std::floor(*number) != *number) return std::nullopt;
            return static_cast<std::uint64_t>(*number);
        }

        struct PackageEntry {
            core::UiEntryKind kind = core::UiEntryKind::Asset;
            std::size_t size = 0;
            std::string hash;
        };
    }

    namespace {
        class FileUiPackage final : public core::UiPackage {
           public:
            explicit FileUiPackage(const std::filesystem::path& path) {
                if (redirected(path)) fail("包落点不能是链接");
                std::error_code error;
                if (std::filesystem::is_directory(path, error))
                    root_ = std::filesystem::canonical(path);
                else if (std::filesystem::is_regular_file(path, error)) {
                    // 清单本身也是 ZIP 里的一个条目
                    zip_ = std::make_unique<ZipArchive>(path, entryCountLimit + 1, core::UiEntryLimit);
                    for (const auto& [name, entry] : zip_->entries())
                        if (!portableName(name)) fail("ZIP 条目名无效", name);
                } else
                    fail("不存在普通目录或 ZIP 文件");

                const auto json = readPhysical(uiManifestName, manifestLimit);
                JsonValue data;
                try {
                    if (json.find('\0') != std::string::npos) throw std::runtime_error("含有 NUL");
                    data = parseJson(json);
                } catch (const std::exception&) {
                    fail("运行时清单无效", uiManifestName);
                }
                if (!std::holds_alternative<JsonValue::Object>(data.value)) fail("运行时清单无效", uiManifestName);
                if (wholeNumber(jsonMember(data, "format")) != std::uint64_t{3}) fail("不支持的 UI 包格式");
                const auto* framework = jsonString(jsonMember(data, "framework"));
                if (!framework || *framework != core::PackageVersion) fail("UI 包的框架版本与宿主不一致，当前需要 " + std::string(core::PackageVersion));
                if (wholeNumber(jsonMember(data, "protocol")) != std::uint64_t{core::RuntimeVersion}) fail("脚本与原生协议不匹配，当前需要 " + std::to_string(core::RuntimeVersion));
                const auto* script = jsonString(jsonMember(data, "scriptFormat"));
                if (!script || (*script != "js" && *script != "bytecode")) fail("脚本表示无效");
                scriptFormat_ = *script == "bytecode" ? core::UiScriptFormat::Bytecode : core::UiScriptFormat::Js;
                const auto bytecode = scriptFormat_ == core::UiScriptFormat::Bytecode;
                const auto* engine = jsonMember(data, "engine");
                if (bytecode) {
                    const auto* identity = jsonString(engine);
                    if (!identity || *identity != quickjs::buildIdentity()) fail("QuickJS 字节码构建身份不匹配");
                } else if (engine)
                    fail("JS 包不能声明字节码引擎");

                const auto* rows = jsonArray(jsonMember(data, "entries"));
                if (!rows || rows->empty() || rows->size() > entryCountLimit) fail("条目清单无效");
                std::unordered_map<std::string, std::string> names{{std::string(uiManifestName), std::string(uiManifestName)}};
                std::unordered_map<std::string, std::string> directories;
                const auto registerName = [&](const std::string& name) {
                    const auto key = folded(name);
                    if (directories.contains(key) || !names.emplace(key, name).second) fail("条目路径冲突", name);
                    for (auto slash = name.find('/'); slash != std::string::npos; slash = name.find('/', slash + 1)) {
                        const auto directory = name.substr(0, slash);
                        const auto directoryKey = folded(directory);
                        const auto found = directories.emplace(directoryKey, directory).first;
                        if (names.contains(directoryKey) || found->second != directory) fail("条目目录冲突", directory);
                    }
                };
                for (const auto& row : *rows) {
                    const auto* object = std::get_if<JsonValue::Object>(&row.value);
                    if (!object || object->size() != 4) fail("条目格式无效");
                    const auto* name = jsonString(jsonMember(row, "path"));
                    const auto* kind = jsonString(jsonMember(row, "kind"));
                    const auto* hash = jsonString(jsonMember(row, "sha256"));
                    const auto size = wholeNumber(jsonMember(row, "size"));
                    if (!name || !kind || !hash) fail("条目字段类型无效");
                    if (!portableName(*name) || (*kind != "module" && *kind != "asset") || !size || *size > core::UiEntryLimit || hash->size() != 64 || hash->find_first_not_of("0123456789abcdef") != std::string::npos) fail("条目字段无效", *name);
                    if (*kind == "module" && (bytecode ? !name->ends_with(".cbc") : endsWithIgnoringCase(*name, ".cbc"))) fail("UI 包模块名不符合脚本表示", *name);
                    registerName(*name);
                    if (zip_) {
                        const auto found = zip_->entries().find(*name);
                        if (found == zip_->entries().end() || found->second.size != *size) fail("ZIP 索引与清单不符", *name);
                    }
                    entries_.emplace(*name, PackageEntry{*kind == "module" ? core::UiEntryKind::Module : core::UiEntryKind::Asset, static_cast<std::size_t>(*size), *hash});
                }
                if (zip_ && zip_->entries().size() != entries_.size() + 1) fail("ZIP 有未登记的条目");
                const auto* entry = jsonString(jsonMember(data, "entry"));
                if (!entry) fail("入口字段不是字符串");
                entry_ = *entry;
                if (bytecode && entry_ != "app.cbc") fail("字节码包入口必须为 app.cbc", entry_);
                const auto found = entries_.find(entry_);
                if (found == entries_.end() || found->second.kind != core::UiEntryKind::Module) fail("入口不是已登记模块", entry_);

                if (const auto* appIdVal = jsonMember(data, "appId")) {
                    const auto* s = jsonString(appIdVal);
                    if (!s) fail("清单 appId 必须是字符串");
                    appId_ = *s;
                }
                if (const auto* singleVal = jsonMember(data, "singleInstance")) {
                    if (!std::holds_alternative<bool>(singleVal->value)) fail("清单 singleInstance 必须是布尔值");
                    singleInstance_ = std::get<bool>(singleVal->value);
                }
                if (const auto* portableVal = jsonMember(data, "portable")) {
                    if (!std::holds_alternative<bool>(portableVal->value)) fail("清单 portable 必须是布尔值");
                    portable_ = std::get<bool>(portableVal->value);
                }
            }

            core::UiScriptFormat scriptFormat() const noexcept override {
                return scriptFormat_;
            }

            std::string_view entry() const noexcept override {
                return entry_;
            }

            std::string_view appId() const noexcept override {
                return appId_;
            }

            bool singleInstance() const noexcept override {
                return singleInstance_;
            }

            bool portable() const noexcept override {
                return portable_;
            }

            bool exists(std::string_view path, core::UiEntryKind kind) const noexcept override {
                const auto found = entries_.find(std::string(path));
                if (found == entries_.end() || found->second.kind != kind) return false;
                if (zip_) return zip_->entries().contains(std::string(path));
                try {
                    auto p = root_;
                    for (const auto& part : utf8Path(path)) {
                        p /= part;
                        if (redirected(p)) return false;
                    }
                    std::error_code ec;
                    return std::filesystem::is_regular_file(p, ec);
                } catch (...) {
                    return false;
                }
            }

            std::string read(std::string_view path, core::UiEntryKind kind, std::size_t limit) const override {
                const auto found = entries_.find(std::string(path));
                if (found == entries_.end() || found->second.kind != kind) fail(kind == core::UiEntryKind::Module ? "找不到已登记模块" : "找不到已登记资产", path);
                const auto& entry = found->second;
                if (entry.size > limit) fail("超过读取预算", path);
                auto result = readPhysical(path, entry.size);
                if (result.size() != entry.size || sha256Hex({reinterpret_cast<const std::uint8_t*>(result.data()), result.size()}) != entry.hash) fail("大小或摘要不匹配", path);
                return result;
            }

           private:
            std::string readPhysical(std::string_view name, std::size_t limit) const {
                if (zip_) return zip_->read(std::string(name), limit);
                auto path = root_;
                for (const auto& part : utf8Path(name)) {
                    path /= part;
                    if (redirected(path)) fail("禁止读取链接", name);
                }
                std::error_code error;
                if (!std::filesystem::is_regular_file(path, error)) fail("找不到普通文件", name);
                const auto size = std::filesystem::file_size(path, error);
                if (error || size > limit) fail("超过读取预算", name);
                std::ifstream stream(path, std::ios::binary);
                std::string result(static_cast<std::size_t>(size), '\0');
                if (!stream || (size > 0 && !stream.read(result.data(), static_cast<std::streamsize>(size))) || stream.peek() != std::char_traits<char>::eof()) fail("文件读取失败或正在更改", name);
                return result;
            }

            std::filesystem::path root_;
            std::unique_ptr<ZipArchive> zip_;
            std::unordered_map<std::string, PackageEntry> entries_;
            std::string entry_;
            std::string appId_;
            bool singleInstance_ = false;
            bool portable_ = false;
            core::UiScriptFormat scriptFormat_ = core::UiScriptFormat::Js;
        };
    }

    std::shared_ptr<const core::UiPackage> mountUiPackage(const std::filesystem::path& path) {
        return std::make_shared<FileUiPackage>(path);
    }

    std::string packageAssetName(std::string_view reference) {
        if (!reference.starts_with('/')) {
            if (!portableName(reference)) fail("资产逻辑名无效", reference);
            return std::string(reference);
        }
        auto encoded = reference.substr(1);
        encoded = encoded.substr(0, encoded.find_first_of("?#"));
        std::string name;
        for (std::size_t i = 0; i < encoded.size(); ++i) {
            if (encoded[i] != '%')
                name += encoded[i];
            else {
                unsigned int byte = 0;
                if (i + 2 >= encoded.size()) fail("资产 URL 编码无效", reference);
                const auto parsed = std::from_chars(encoded.data() + i + 1, encoded.data() + i + 3, byte, 16);
                if (parsed.ec != std::errc{} || parsed.ptr != encoded.data() + i + 3 || byte == '/') fail("资产 URL 编码无效", reference);
                name += static_cast<char>(byte);
                i += 2;
            }
        }
        if (!portableName(name)) fail("资产 URL 路径无效", reference);
        return name;
    }
}
