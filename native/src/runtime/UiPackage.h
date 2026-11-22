#pragma once

#include <core/UiPackage.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace canton::runtime {
    inline constexpr std::string_view uiManifestName = "canton-ui.json";

    // 挂载 UI 包：path 是目录包或 ZIP 包。清单在挂载时整体校验，条目在读取时核对大小与 SHA-256；
    // 规则与 packages/vite-plugin 的 ui-package.ts、cli 的 UiPackage.ts 一致。校验不过抛 std::runtime_error
    [[nodiscard]] std::shared_ptr<const core::UiPackage> mountUiPackage(const std::filesystem::path& path);

    // 把资产引用（逻辑名，或以 / 开头的 URL 路径）换成包内条目名
    std::string packageAssetName(std::string_view reference);
}
