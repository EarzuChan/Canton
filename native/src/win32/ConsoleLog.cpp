#include "ConsoleLog.h"

#include <core/Log.h>

#include <windows.h>

namespace canton::win32 {
    void enableConsoleLogColors() noexcept {
        const auto handle = GetStdHandle(STD_ERROR_HANDLE);
        DWORD mode = 0;
        core::Log::setColors(handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &mode) && SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING));
    }
}
