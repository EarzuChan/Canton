#pragma once

namespace canton::win32 {
    // 为标准错误打开虚拟终端序列，成功时让日志着色输出；由宿主启动时调用
    void enableConsoleLogColors() noexcept;
}
