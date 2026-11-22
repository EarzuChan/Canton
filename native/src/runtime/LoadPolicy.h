#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace canton::runtime {
    // 三种加载策略，行为见 batch1 ADR 009
    enum class LoadPolicy {
        // 先连 Live DevServer，连不上且有 Dist 时回退并提示
        LiveThenDist,
        // 只连 Live DevServer，连不上进错误屏并自动重试
        OnlyLive,
        // 只读 Dist，不连 DevServer
        OnlyDist,
    };

    [[nodiscard]] const char* loadPolicyLabel(LoadPolicy policy) noexcept;
    [[nodiscard]] std::optional<LoadPolicy> parseLoadPolicy(std::string_view label) noexcept;

    // 应用源配置，由宿主装配时给定
    struct AppSourceConfig {
        LoadPolicy policy = LoadPolicy::OnlyDist;
        std::string devServerUrl;
        // 随包 Dist 所在目录：其下的 ui/（目录包）或 ui.zip（ZIP 包）
        std::filesystem::path bundleRoot;
        // 覆写 Dist 包位置，目录或 ZIP；空表示用随包位置
        std::filesystem::path distOverride;
    };
}
