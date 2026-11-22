#pragma once

#include <string>
#include <string_view>

namespace canton::runtime {
    std::string packageAssetName(std::string_view reference);
}
