#pragma once

#include <runtime/PainterResources.h>

#include <string>

namespace canton::runtime {
    // Live：资源从 DevServer 取。只允许同源、不越出站点根的 URL，受字节预算和总时限约束
    [[nodiscard]] core::PainterLoader livePainterLoader(std::string devServerUrl, PainterResourceLimits limits = {}, std::shared_ptr<ResourceWorkers> workers = createResourceWorkers());
    [[nodiscard]] core::AssetLoader liveAssetLoader(std::string devServerUrl, std::shared_ptr<ResourceWorkers> workers = createResourceWorkers());
}
