#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace canton::runtime {

    enum class FsErrorCode : std::uint8_t {
        NotFound,
        AlreadyExists,
        PermissionDenied,
        IsDirectory,
        NotDirectory,
        InvalidPath,
        Io,
    };

    [[nodiscard]] const char* fsErrorCodeName(FsErrorCode code) noexcept;

    class FsError final : public std::runtime_error {
       public:
        FsError(FsErrorCode code, std::string path, std::string message)
            : std::runtime_error(std::move(message)), code_(code), path_(std::move(path)) {}

        [[nodiscard]] FsErrorCode code() const noexcept { return code_; }
        [[nodiscard]] const std::string& path() const noexcept { return path_; }

       private:
        FsErrorCode code_;
        std::string path_;
    };

    struct FsStat {
        bool isFile = false;
        bool isDirectory = false;
        std::uint64_t size = 0;
        double modifiedTimeMs = 0.0;
        double createdTimeMs = 0.0;
    };

    struct FsDirEntry {
        std::string name;
        bool isFile = false;
        bool isDirectory = false;
    };

    // 同步底层实现（供后台 IoThreadPool 线程及未来 M7 Native SDK 复用）
    [[nodiscard]] std::string fsReadBytes(std::string_view path);
    [[nodiscard]] std::string fsReadText(std::string_view path);
    void fsWriteBytes(std::string_view path, std::string_view bytes, bool atomic = false);
    void fsWriteText(std::string_view path, std::string_view text, bool atomic = false);
    [[nodiscard]] bool fsExists(std::string_view path);
    [[nodiscard]] FsStat fsStat(std::string_view path);
    [[nodiscard]] std::vector<FsDirEntry> fsReadDir(std::string_view path);
    void fsMkdir(std::string_view path, bool recursive = true);
    void fsRemove(std::string_view path, bool recursive = false);
    void fsRename(std::string_view fromPath, std::string_view toPath);
    void fsCopy(std::string_view fromPath, std::string_view toPath, bool overwrite = false);

    // 供 downloadFile 复用的同目录临时文件原子替换工具
    [[nodiscard]] std::wstring fsMakeSiblingTempPath(const std::wstring& targetLongPath);
    void fsAtomicReplaceFile(const std::wstring& tempLongPath, const std::wstring& targetLongPath, std::string_view displayPath);

}
