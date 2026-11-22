#pragma once

#include <sstream>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace canton::core {
    enum class LogLevel { Verbose, Debug, Info, Warn, Error };

    class Log final {
       public:
        template <typename... Args>
        static void v(std::string_view tag, Args&&... args) {
            write(LogLevel::Verbose, tag, join(std::forward<Args>(args)...));
        }

        template <typename... Args>
        static void d(std::string_view tag, Args&&... args) {
            write(LogLevel::Debug, tag, join(std::forward<Args>(args)...));
        }

        template <typename... Args>
        static void i(std::string_view tag, Args&&... args) {
            write(LogLevel::Info, tag, join(std::forward<Args>(args)...));
        }

        template <typename... Args>
        static void w(std::string_view tag, Args&&... args) {
            write(LogLevel::Warn, tag, join(std::forward<Args>(args)...));
        }

        template <typename... Args>
        static void e(std::string_view tag, Args&&... args) {
            write(LogLevel::Error, tag, join(std::forward<Args>(args)...));
        }

        static void write(LogLevel level, std::string_view tag, std::string message);
        static std::string format(LogLevel level, std::string_view tag, std::string_view message, std::chrono::system_clock::time_point timestamp, bool colors = false);
        // 终端能显示 ANSI 颜色时由宿主打开（见 win32/ConsoleLog.h），默认不着色
        static void setColors(bool enabled) noexcept;

       private:
        template <typename... Args>
        static std::string join(Args&&... args) {
            std::ostringstream out;
            bool first = true;
            ((out << (first ? "" : " ") << std::forward<Args>(args), first = false), ...);
            return out.str();
        }
    };
}
