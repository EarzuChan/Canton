#include <runtime/FileSystem.h>

#include <runtime/AppPaths.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>

namespace canton::runtime {
    namespace {
        std::atomic<std::uint64_t> gTempCounter{1};

        FsErrorCode mapWin32Error(DWORD err) noexcept {
            switch (err) {
                case ERROR_FILE_NOT_FOUND:
                case ERROR_PATH_NOT_FOUND:
                case ERROR_INVALID_DRIVE:
                    return FsErrorCode::NotFound;
                case ERROR_FILE_EXISTS:
                case ERROR_ALREADY_EXISTS:
                    return FsErrorCode::AlreadyExists;
                case ERROR_ACCESS_DENIED:
                case ERROR_SHARING_VIOLATION:
                case ERROR_LOCK_VIOLATION:
                case ERROR_WRITE_PROTECT:
                    return FsErrorCode::PermissionDenied;
                case ERROR_DIRECTORY:
                    return FsErrorCode::NotDirectory;
                case ERROR_INVALID_NAME:
                case ERROR_BAD_PATHNAME:
                case ERROR_FILENAME_EXCED_RANGE:
                case ERROR_INVALID_PARAMETER:
                    return FsErrorCode::InvalidPath;
                default:
                    return FsErrorCode::Io;
            }
        }

        [[noreturn]] void throwWin32FsError(DWORD err, std::string_view path, const char* action) {
            const auto code = mapWin32Error(err);
            throw FsError(code, std::string(path), std::string(action) + "失败（Win32 错误 " + std::to_string(err) + "）：" + std::string(path));
        }

        std::wstring requirePath(std::string_view path) {
            if (path.empty() || path.find('\0') != std::string_view::npos) {
                throw FsError(FsErrorCode::InvalidPath, std::string(path), "路径为空或包含非法字符");
            }
            try {
                return AppPaths::toWin32LongPath(path);
            } catch (const std::exception& error) {
                throw FsError(FsErrorCode::InvalidPath, std::string(path), error.what());
            }
        }

        double fileTimeToEpochMs(const FILETIME& ft) noexcept {
            ULARGE_INTEGER ui{};
            ui.LowPart = ft.dwLowDateTime;
            ui.HighPart = ft.dwHighDateTime;
            // Windows FILETIME 基准 1601-01-01，与 Unix 1970-01-01 相差 11644473600 秒
            constexpr std::uint64_t kEpochDiff100ns = 116444736000000000ull;
            if (ui.QuadPart < kEpochDiff100ns) return 0.0;
            return static_cast<double>(ui.QuadPart - kEpochDiff100ns) / 10000.0;
        }

        struct ScopedHandle {
            HANDLE h = INVALID_HANDLE_VALUE;
            ~ScopedHandle() {
                if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h);
            }
        };

        void writeAllToHandle(HANDLE h, std::string_view bytes, std::string_view displayPath) {
            std::size_t offset = 0;
            while (offset < bytes.size()) {
                const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 20));
                DWORD written = 0;
                if (!WriteFile(h, bytes.data() + offset, chunk, &written, nullptr)) {
                    throwWin32FsError(GetLastError(), displayPath, "写入文件");
                }
                offset += written;
            }
        }
    }

    const char* fsErrorCodeName(FsErrorCode code) noexcept {
        switch (code) {
            case FsErrorCode::NotFound: return "NOT_FOUND";
            case FsErrorCode::AlreadyExists: return "ALREADY_EXISTS";
            case FsErrorCode::PermissionDenied: return "PERMISSION_DENIED";
            case FsErrorCode::IsDirectory: return "IS_DIRECTORY";
            case FsErrorCode::NotDirectory: return "NOT_DIRECTORY";
            case FsErrorCode::InvalidPath: return "INVALID_PATH";
            case FsErrorCode::Io: return "IO";
        }
        return "IO";
    }

    std::wstring fsMakeSiblingTempPath(const std::wstring& targetLongPath) {
        const auto seq = gTempCounter.fetch_add(1, std::memory_order_relaxed);
        const auto pid = GetCurrentProcessId();
        const auto tick = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        wchar_t suffix[96]{};
        std::swprintf(suffix, std::size(suffix), L".canton-tmp-%lu-%llu-%llx", pid, static_cast<unsigned long long>(seq), static_cast<unsigned long long>(tick & 0xffffffffull));
        return targetLongPath + suffix;
    }

    void fsAtomicReplaceFile(const std::wstring& tempLongPath, const std::wstring& targetLongPath, std::string_view displayPath) {
        const DWORD targetAttrs = GetFileAttributesW(targetLongPath.c_str());
        if (targetAttrs != INVALID_FILE_ATTRIBUTES) {
            if ((targetAttrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                DeleteFileW(tempLongPath.c_str());
                throw FsError(FsErrorCode::IsDirectory, std::string(displayPath), "目标路径是目录，无法原子替换：" + std::string(displayPath));
            }
            if (!ReplaceFileW(targetLongPath.c_str(), tempLongPath.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
                const DWORD err = GetLastError();
                if (!MoveFileExW(tempLongPath.c_str(), targetLongPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                    const DWORD err2 = GetLastError();
                    DeleteFileW(tempLongPath.c_str());
                    throwWin32FsError(err2 != 0 ? err2 : err, displayPath, "原子替换文件");
                }
            }
        } else {
            if (!MoveFileExW(tempLongPath.c_str(), targetLongPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                const DWORD err = GetLastError();
                DeleteFileW(tempLongPath.c_str());
                throwWin32FsError(err, displayPath, "移动原子临时文件");
            }
        }
    }

    std::string fsReadBytes(std::string_view path) {
        const auto wide = requirePath(path);
        const DWORD attrs = GetFileAttributesW(wide.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            throw FsError(FsErrorCode::IsDirectory, std::string(path), "路径是目录，无法按文件读取：" + std::string(path));
        }
        ScopedHandle file{CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        if (file.h == INVALID_HANDLE_VALUE) throwWin32FsError(GetLastError(), path, "打开文件读取");
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.h, &size)) throwWin32FsError(GetLastError(), path, "读取文件大小");
        if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) > (256ull * 1024 * 1024)) {
            throw FsError(FsErrorCode::Io, std::string(path), "文件过大，超过单次读入内存上限：" + std::string(path));
        }
        std::string out(static_cast<std::size_t>(size.QuadPart), '\0');
        std::size_t offset = 0;
        while (offset < out.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(out.size() - offset, 1u << 20));
            DWORD read = 0;
            if (!ReadFile(file.h, out.data() + offset, chunk, &read, nullptr)) {
                throwWin32FsError(GetLastError(), path, "读取文件");
            }
            if (read == 0) break;
            offset += read;
        }
        out.resize(offset);
        return out;
    }

    std::string fsReadText(std::string_view path) {
        return fsReadBytes(path);
    }

    void fsWriteBytes(std::string_view path, std::string_view bytes, bool atomic) {
        const auto wide = requirePath(path);
        const DWORD attrs = GetFileAttributesW(wide.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            throw FsError(FsErrorCode::IsDirectory, std::string(path), "路径是目录，无法写入文件：" + std::string(path));
        }
        if (!atomic) {
            ScopedHandle file{CreateFileW(wide.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)};
            if (file.h == INVALID_HANDLE_VALUE) throwWin32FsError(GetLastError(), path, "创建写入文件");
            writeAllToHandle(file.h, bytes, path);
            return;
        }

        const auto tempWide = fsMakeSiblingTempPath(wide);
        {
            ScopedHandle file{CreateFileW(tempWide.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
            if (file.h == INVALID_HANDLE_VALUE) throwWin32FsError(GetLastError(), path, "创建原子写临时文件");
            try {
                writeAllToHandle(file.h, bytes, path);
                if (!FlushFileBuffers(file.h)) throwWin32FsError(GetLastError(), path, "原子写刷盘");
            } catch (...) {
                file.h = (CloseHandle(file.h), INVALID_HANDLE_VALUE);
                DeleteFileW(tempWide.c_str());
                throw;
            }
        }
        fsAtomicReplaceFile(tempWide, wide, path);
    }

    void fsWriteText(std::string_view path, std::string_view text, bool atomic) {
        fsWriteBytes(path, text, atomic);
    }

    bool fsExists(std::string_view path) {
        if (path.empty() || path.find('\0') != std::string_view::npos) return false;
        const auto wide = requirePath(path);
        return GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    FsStat fsStat(std::string_view path) {
        const auto wide = requirePath(path);
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data)) {
            throwWin32FsError(GetLastError(), path, "读取文件状态");
        }
        FsStat st;
        st.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        st.isFile = !st.isDirectory;
        ULARGE_INTEGER sz{};
        sz.LowPart = data.nFileSizeLow;
        sz.HighPart = data.nFileSizeHigh;
        st.size = st.isDirectory ? 0ull : sz.QuadPart;
        st.modifiedTimeMs = fileTimeToEpochMs(data.ftLastWriteTime);
        st.createdTimeMs = fileTimeToEpochMs(data.ftCreationTime);
        return st;
    }

    std::vector<FsDirEntry> fsReadDir(std::string_view path) {
        const auto wide = requirePath(path);
        const DWORD attrs = GetFileAttributesW(wide.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) throwWin32FsError(GetLastError(), path, "读取目录");
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            throw FsError(FsErrorCode::NotDirectory, std::string(path), "路径不是目录：" + std::string(path));
        }
        std::wstring pattern = wide;
        if (!pattern.empty() && pattern.back() != L'\\') pattern.push_back(L'\\');
        pattern.push_back(L'*');

        WIN32_FIND_DATAW fd{};
        HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) {
            const DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND) return {};
            throwWin32FsError(err, path, "遍历目录");
        }
        std::vector<FsDirEntry> entries;
        do {
            const std::wstring_view name(fd.cFileName);
            if (name == L"." || name == L"..") continue;
            FsDirEntry entry;
            entry.name = AppPaths::wideToUtf8(name);
            entry.isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            entry.isFile = !entry.isDirectory;
            entries.push_back(std::move(entry));
        } while (FindNextFileW(find, &fd));
        FindClose(find);
        return entries;
    }

    void fsMkdir(std::string_view path, bool recursive) {
        const auto wide = requirePath(path);
        std::error_code ec;
        if (recursive) {
            std::filesystem::create_directories(std::filesystem::path(wide), ec);
            if (ec) {
                const DWORD attrs = GetFileAttributesW(wide.c_str());
                if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                    throw FsError(FsErrorCode::AlreadyExists, std::string(path), "同名普通文件已存在：" + std::string(path));
                }
                throw FsError(FsErrorCode::Io, std::string(path), "创建目录失败：" + ec.message());
            }
        } else {
            if (!CreateDirectoryW(wide.c_str(), nullptr)) {
                throwWin32FsError(GetLastError(), path, "创建目录");
            }
        }
    }

    namespace {
        void removeTreeWin32(const std::wstring& dirWide, std::string_view originalPath) {
            std::wstring prefixed = dirWide;
            if (prefixed.rfind(L"\\\\?\\", 0) != 0 && prefixed.size() >= 2 && prefixed[1] == L':') {
                prefixed = L"\\\\?\\" + prefixed;
            }
            std::wstring pattern = prefixed;
            if (!pattern.empty() && pattern.back() != L'\\') pattern.push_back(L'\\');
            pattern.push_back(L'*');

            WIN32_FIND_DATAW fd{};
            HANDLE find = FindFirstFileW(pattern.c_str(), &fd);
            if (find != INVALID_HANDLE_VALUE) {
                do {
                    const std::wstring_view name(fd.cFileName);
                    if (name == L"." || name == L"..") continue;
                    std::wstring child = prefixed;
                    if (!child.empty() && child.back() != L'\\') child.push_back(L'\\');
                    child.append(name);
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                        removeTreeWin32(child, originalPath);
                    } else {
                        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0) {
                            SetFileAttributesW(child.c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                        }
                        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                            if (!RemoveDirectoryW(child.c_str())) {
                                const DWORD err = GetLastError();
                                FindClose(find);
                                throwWin32FsError(err, originalPath, "递归删除目录");
                            }
                        } else if (!DeleteFileW(child.c_str())) {
                            const DWORD err = GetLastError();
                            FindClose(find);
                            throwWin32FsError(err, originalPath, "递归删除文件");
                        }
                    }
                } while (FindNextFileW(find, &fd));
                FindClose(find);
            }
            if (!RemoveDirectoryW(prefixed.c_str())) {
                throwWin32FsError(GetLastError(), originalPath, "递归删除目录");
            }
        }
    }

    void fsRemove(std::string_view path, bool recursive) {
        const auto wide = requirePath(path);
        const DWORD attrs = GetFileAttributesW(wide.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) throwWin32FsError(GetLastError(), path, "删除路径");
        if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            if (!DeleteFileW(wide.c_str())) throwWin32FsError(GetLastError(), path, "删除文件");
            return;
        }
        if (!recursive) {
            if (!RemoveDirectoryW(wide.c_str())) throwWin32FsError(GetLastError(), path, "删除目录");
            return;
        }
        removeTreeWin32(wide, path);
    }

    void fsRename(std::string_view fromPath, std::string_view toPath) {
        const auto fromWide = requirePath(fromPath);
        const auto toWide = requirePath(toPath);
        if (!MoveFileExW(fromWide.c_str(), toWide.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            throwWin32FsError(GetLastError(), fromPath, "重命名路径");
        }
    }

    void fsCopy(std::string_view fromPath, std::string_view toPath, bool overwrite) {
        const auto fromWide = requirePath(fromPath);
        const auto toWide = requirePath(toPath);
        if (!CopyFileW(fromWide.c_str(), toWide.c_str(), overwrite ? FALSE : TRUE)) {
            throwWin32FsError(GetLastError(), toPath, "复制文件");
        }
    }

}
