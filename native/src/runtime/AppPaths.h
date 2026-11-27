#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace canton::runtime {

    enum class AppPathRelocationMode : std::uint8_t {
        Standard,
        Portable,
        ExplicitRoot,
    };

    struct AppPathsConfig {
        std::filesystem::path exeDir;
        std::string appId;
        std::optional<std::filesystem::path> explicitDataRoot;
        bool portableFromManifest = false;
    };

    // 应用路径与用户标准目录解析器（启动期只读定案，按需懒创建 data/temp）
    class AppPaths final {
       public:
        AppPaths() = default;
        explicit AppPaths(AppPathsConfig config);

        [[nodiscard]] AppPathRelocationMode relocationMode() const noexcept { return mode_; }
        [[nodiscard]] const std::filesystem::path& dataDirPath() const noexcept { return dataDir_; }
        [[nodiscard]] const std::filesystem::path& tempDirPath() const noexcept { return tempDir_; }

        // 应用专属目录：首次查询时按需懒创建并返回 UTF-8 路径
        [[nodiscard]] std::string appData() const;
        [[nodiscard]] std::string temp() const;

        // Windows 用户标准目录（SHGetKnownFolderPath）
        [[nodiscard]] static std::string downloads();
        [[nodiscard]] static std::string documents();
        [[nodiscard]] static std::string desktop();
        [[nodiscard]] static std::string pictures();
        [[nodiscard]] static std::string music();
        [[nodiscard]] static std::string videos();

        // 把 UTF-8 路径转为 Win32 UTF-16 路径（超长绝对路径自动补 \\?\ 或 \\?\UNC\ 前缀）
        [[nodiscard]] static std::wstring toWin32LongPath(std::string_view utf8Path);
        [[nodiscard]] static std::wstring toWin32LongPath(const std::filesystem::path& path);
        [[nodiscard]] static std::string wideToUtf8(std::wstring_view wide);
        [[nodiscard]] static std::wstring utf8ToWide(std::string_view utf8);

       private:
        AppPathRelocationMode mode_ = AppPathRelocationMode::Standard;
        std::filesystem::path dataDir_;
        std::filesystem::path tempDir_;
    };

}
