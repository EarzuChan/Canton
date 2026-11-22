#include <core/Log.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace canton::core {
    namespace {
        std::atomic<bool> colorsEnabled{false};

        std::string singleLine(std::string_view value) {
            std::string result;
            result.reserve(value.size());
            bool previousSpace = false;
            for (const unsigned char c : value) {
                if (c < 0x20 || c == 0x7f) {
                    if (!previousSpace) result.push_back(' ');
                    previousSpace = true;
                } else {
                    result.push_back(static_cast<char>(c));
                    previousSpace = c == ' ';
                }
            }
            return result;
        }

        const char* levelCode(LogLevel level) noexcept {
            switch (level) {
                case LogLevel::Verbose:
                    return "V";
                case LogLevel::Debug:
                    return "D";
                case LogLevel::Info:
                    return "I";
                case LogLevel::Warn:
                    return "W";
                case LogLevel::Error:
                    return "E";
            }
            return "I";
        }

        const char* levelColor(LogLevel level) noexcept {
            switch (level) {
                case LogLevel::Verbose:
                    return "\x1b[90m";
                case LogLevel::Debug:
                    return "\x1b[36m";
                case LogLevel::Info:
                    return "\x1b[32m";
                case LogLevel::Warn:
                    return "\x1b[33m";
                case LogLevel::Error:
                    return "\x1b[31m";
            }
            return "\x1b[32m";
        }

    }

    std::string Log::format(LogLevel level, std::string_view tag, std::string_view message, std::chrono::system_clock::time_point timestamp, bool colors) {
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()) % 1000;
        const auto rawTime = std::chrono::system_clock::to_time_t(timestamp);
        std::tm local{};
        localtime_s(&local, &rawTime);
        std::ostringstream time;
        time << std::put_time(&local, "%H:%M:%S") << '.' << std::setw(3) << std::setfill('0') << ms.count();
        const auto body = singleLine(message);
        const auto color = levelColor(level);
        std::string line = time.str() + " - A - " + singleLine(tag) + " ";
        if (colors) line += color;
        line += "[" + std::string(levelCode(level)) + "]";
        if (colors) line += "\x1b[0m";
        line += " > ";
        if (colors) line += color;
        line += body;
        if (colors) line += "\x1b[0m";
        return line;
    }

    void Log::setColors(bool enabled) noexcept {
        colorsEnabled.store(enabled, std::memory_order_relaxed);
    }

    void Log::write(LogLevel level, std::string_view tag, std::string message) {
        std::clog << format(level, tag, message, std::chrono::system_clock::now(), colorsEnabled.load(std::memory_order_relaxed)) << '\n';
        std::clog.flush();
    }
}
