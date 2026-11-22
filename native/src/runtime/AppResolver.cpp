#include <runtime/AppResolver.h>

#include <system_error>
#include <utility>

namespace canton::runtime {
    namespace {
        std::string display(const std::filesystem::path& path) {
            const auto text = path.u8string();
            return {text.begin(), text.end()};
        }

        bool present(const std::filesystem::path& path) {
            std::error_code error;
            return std::filesystem::exists(path, error);
        }
    }

    const char* loadPolicyLabel(LoadPolicy policy) noexcept {
        switch (policy) {
            case LoadPolicy::LiveThenDist:
                return "lfd";
            case LoadPolicy::OnlyLive:
                return "live";
            case LoadPolicy::OnlyDist:
                return "dist";
        }
        return "dist";
    }

    std::optional<LoadPolicy> parseLoadPolicy(std::string_view label) noexcept {
        if (label == "lfd") return LoadPolicy::LiveThenDist;
        if (label == "live") return LoadPolicy::OnlyLive;
        if (label == "dist") return LoadPolicy::OnlyDist;
        return std::nullopt;
    }

    AppResolver::AppResolver(MountPackage mountPackage) : mountPackage_(std::move(mountPackage)) {}

    ResolvedDist AppResolver::locateDist(const AppSourceConfig& config) const {
        ResolvedDist result;
        if (!config.distOverride.empty()) {
            result.location = config.distOverride.is_absolute() || config.bundleRoot.empty() ? config.distOverride.lexically_normal() : (config.bundleRoot / config.distOverride).lexically_normal();
            if (present(result.location))
                result.ok = true;
            else {
                result.missing = true;
                result.error = "找不到指定的 Dist 包：" + display(result.location);
            }
            return result;
        }
        if (config.bundleRoot.empty()) {
            result.missing = true;
            result.error = "找不到随包 Dist：宿主所在目录未知";
            return result;
        }
        const auto directory = config.bundleRoot / BundledDirectory;
        const auto archive = config.bundleRoot / BundledArchive;
        const auto hasDirectory = present(directory), hasArchive = present(archive);
        if (hasDirectory && hasArchive) {
            result.location = config.bundleRoot;
            result.error = "随包 Dist 同时存在目录包和 ZIP 包，只能保留一个：" + display(directory) + "、" + display(archive);
            return result;
        }
        if (!hasDirectory && !hasArchive) {
            result.location = config.bundleRoot;
            result.missing = true;
            result.error = "找不到随包 Dist：" + display(directory) + " 或 " + display(archive);
            return result;
        }
        result.location = hasDirectory ? directory : archive;
        result.ok = true;
        return result;
    }

    ResolvedDist AppResolver::resolveDist(const AppSourceConfig& config) const {
        auto result = locateDist(config);
        if (!result.ok) return result;
        result.ok = false;
        if (!mountPackage_) {
            result.error = "宿主未提供 UI 包挂载器";
            return result;
        }
        try {
            result.package = mountPackage_(result.location);
            result.ok = static_cast<bool>(result.package);
            if (!result.ok) result.error = "无法挂载 UI 包：" + display(result.location);
        } catch (const std::exception& error) {
            result.error = error.what();
        }
        return result;
    }
}
