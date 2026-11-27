#pragma once

#include <core/PlatformTypes.h>

#include <string>
#include <string_view>
#include <vector>

namespace canton::win32 {

    // 通过 ICustomDestinationList 写入任务栏右键 Jump List（点击条目经命令行进入 LaunchRequest）
    void setJumpList(std::string_view appId, const std::vector<core::JumpListCategorySpec>& categories);

    // 清空当前 AUMID 的自定义 Jump List
    void clearJumpList(std::string_view appId);

}
