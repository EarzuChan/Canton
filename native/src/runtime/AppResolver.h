#pragma once

#include <core/UiPackage.h>
#include <runtime/LoadPolicy.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace canton::runtime {
    struct ResolvedDist {
        bool ok = false;
        // 没有可用的 Dist 包；与包存在但无效的情形区分，后者不算“没有 Dist”
        bool missing = false;
        std::filesystem::path location;
        std::shared_ptr<const core::UiPackage> package;
        std::string error;
    };

    // 定位并挂载 Dist 包：覆写位置优先，否则取随包位置
    class AppResolver {
       public:
        static constexpr const char* DefaultDevServer = "http://127.0.0.1:9178";
        static constexpr const char* BundledDirectory = "ui";
        static constexpr const char* BundledArchive = "ui.zip";

        using MountPackage = std::function<std::shared_ptr<const core::UiPackage>(const std::filesystem::path&)>;

        explicit AppResolver(MountPackage mountPackage);

        // 只定位，不读包内容
        [[nodiscard]] ResolvedDist locateDist(const AppSourceConfig& config) const;
        [[nodiscard]] ResolvedDist resolveDist(const AppSourceConfig& config) const;

       private:
        MountPackage mountPackage_;
    };
}
