#include <runtime/AppPaths.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <stdexcept>

namespace canton::runtime {
    namespace {
        std::string sanitizeAppIdSegment(std::string_view appId) {
            std::string out;
            out.reserve(appId.size());
            for (const unsigned char c : appId) {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') {
                    out.push_back(static_cast<char>(c));
                } else {
                    out.push_back('_');
                }
            }
            while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
            return out.empty() ? std::string("CantonApp") : out;
        }

        std::filesystem::path knownFolderPath(REFKNOWNFOLDERID folderId, const char* label) {
            PWSTR raw = nullptr;
            const HRESULT hr = SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &raw);
            if (FAILED(hr) || !raw) {
                if (raw) CoTaskMemFree(raw);
                throw std::runtime_error(std::string("无法解析系统标准目录：") + label);
            }
            std::wstring wide(raw);
            CoTaskMemFree(raw);
            return std::filesystem::path(wide);
        }

        std::filesystem::path systemTempDir() {
            using GetTempPath2WFn = DWORD(WINAPI*)(DWORD, LPWSTR);
            std::wstring buf(MAX_PATH + 1, L'\0');
            DWORD len = 0;
            if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll")) {
                if (const auto fn = reinterpret_cast<GetTempPath2WFn>(GetProcAddress(k32, "GetTempPath2W"))) {
                    len = fn(static_cast<DWORD>(buf.size()), buf.data());
                    if (len > buf.size()) {
                        buf.resize(len + 1, L'\0');
                        len = fn(static_cast<DWORD>(buf.size()), buf.data());
                    }
                }
            }
            if (len == 0) {
                len = GetTempPathW(static_cast<DWORD>(buf.size()), buf.data());
                if (len > buf.size()) {
                    buf.resize(len + 1, L'\0');
                    len = GetTempPathW(static_cast<DWORD>(buf.size()), buf.data());
                }
            }
            buf.resize(len);
            while (!buf.empty() && (buf.back() == L'\\' || buf.back() == L'/')) buf.pop_back();
            return std::filesystem::path(buf);
        }

        void ensureDirectoryExists(const std::filesystem::path& dir) {
            if (dir.empty()) throw std::runtime_error("应用目录路径为空");
            std::error_code ec;
            const auto longPath = AppPaths::toWin32LongPath(dir);
            if (GetFileAttributesW(longPath.c_str()) != INVALID_FILE_ATTRIBUTES) return;
            std::filesystem::create_directories(std::filesystem::path(longPath), ec);
            if (ec) {
                throw std::runtime_error("无法创建应用目录：" + AppPaths::wideToUtf8(dir.wstring()) + "（" + ec.message() + "）");
            }
        }
    }

    std::wstring AppPaths::utf8ToWide(std::string_view utf8) {
        if (utf8.empty()) return {};
        const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (needed <= 0) throw std::runtime_error("路径包含无效 UTF-8 编码");
        std::wstring wide(static_cast<std::size_t>(needed), L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
        return wide;
    }

    std::string AppPaths::wideToUtf8(std::wstring_view wide) {
        if (wide.empty()) return {};
        const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (needed <= 0) return {};
        std::string utf8(static_cast<std::size_t>(needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed, nullptr, nullptr);
        return utf8;
    }

    std::wstring AppPaths::toWin32LongPath(const std::filesystem::path& path) {
        std::wstring raw = path.wstring();
        if (raw.empty()) return raw;
        std::replace(raw.begin(), raw.end(), L'/', L'\\');
        if (raw.starts_with(L"\\\\?\\") || raw.starts_with(L"\\\\.\\")) return raw;

        std::filesystem::path normalized = std::filesystem::path(raw).lexically_normal();
        std::wstring norm = normalized.wstring();
        std::replace(norm.begin(), norm.end(), L'/', L'\\');
        if (!normalized.is_absolute()) {
            std::error_code ec;
            auto absPath = std::filesystem::absolute(normalized, ec);
            if (!ec) {
                norm = absPath.lexically_normal().wstring();
                std::replace(norm.begin(), norm.end(), L'/', L'\\');
            }
        }
        if (norm.size() < 240) return norm;
        if (norm.starts_with(L"\\\\")) {
            return L"\\\\?\\UNC\\" + norm.substr(2);
        }
        return L"\\\\?\\" + norm;
    }

    std::wstring AppPaths::toWin32LongPath(std::string_view utf8Path) {
        return toWin32LongPath(std::filesystem::path(utf8ToWide(utf8Path)));
    }

    AppPaths::AppPaths(AppPathsConfig config) {
        const auto safeId = sanitizeAppIdSegment(config.appId);
        if (config.explicitDataRoot && !config.explicitDataRoot->empty()) {
            mode_ = AppPathRelocationMode::ExplicitRoot;
            const auto root = config.explicitDataRoot->lexically_normal();
            dataDir_ = root / L"data";
            tempDir_ = root / L"temp";
            return;
        }

        bool portable = config.portableFromManifest;
        if (!portable && !config.exeDir.empty()) {
            const auto marker = config.exeDir / L".canton-portable";
            const DWORD attrs = GetFileAttributesW(toWin32LongPath(marker).c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                portable = true;
            }
        }
        if (portable && !config.exeDir.empty()) {
            mode_ = AppPathRelocationMode::Portable;
            const auto root = config.exeDir.lexically_normal();
            dataDir_ = root / L"data";
            tempDir_ = root / L"temp";
            return;
        }

        mode_ = AppPathRelocationMode::Standard;
        const auto wideId = utf8ToWide(safeId);
        dataDir_ = (knownFolderPath(FOLDERID_LocalAppData, "LocalAppData") / wideId).lexically_normal();
        tempDir_ = (systemTempDir() / wideId).lexically_normal();
    }

    std::string AppPaths::appData() const {
        ensureDirectoryExists(dataDir_);
        return wideToUtf8(dataDir_.wstring());
    }

    std::string AppPaths::temp() const {
        ensureDirectoryExists(tempDir_);
        return wideToUtf8(tempDir_.wstring());
    }

    std::string AppPaths::downloads() { return wideToUtf8(knownFolderPath(FOLDERID_Downloads, "Downloads").wstring()); }
    std::string AppPaths::documents() { return wideToUtf8(knownFolderPath(FOLDERID_Documents, "Documents").wstring()); }
    std::string AppPaths::desktop() { return wideToUtf8(knownFolderPath(FOLDERID_Desktop, "Desktop").wstring()); }
    std::string AppPaths::pictures() { return wideToUtf8(knownFolderPath(FOLDERID_Pictures, "Pictures").wstring()); }
    std::string AppPaths::music() { return wideToUtf8(knownFolderPath(FOLDERID_Music, "Music").wstring()); }
    std::string AppPaths::videos() { return wideToUtf8(knownFolderPath(FOLDERID_Videos, "Videos").wstring()); }

}
