#pragma once

#include <core/Log.h>
#include <runtime/DiagnosticsTypes.h>

#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace canton::runtime {

    class DiagnosticsState;

    class DiagnosticsToast final {
       public:
        static bool show(DiagnosticsState& state, core::LogLevel level, std::string_view tag, std::string title, std::string content, bool coalesce = true);

        template <typename... Args>
        static bool v(DiagnosticsState& state, std::string_view tag, std::string title, Args&&... args) {
            return show(state, core::LogLevel::Verbose, tag, std::move(title), join(std::forward<Args>(args)...));
        }

        template <typename... Args>
        static bool d(DiagnosticsState& state, std::string_view tag, std::string title, Args&&... args) {
            return show(state, core::LogLevel::Debug, tag, std::move(title), join(std::forward<Args>(args)...));
        }

        template <typename... Args>
        static bool i(DiagnosticsState& state, std::string_view tag, std::string title, Args&&... args) {
            return show(state, core::LogLevel::Info, tag, std::move(title), join(std::forward<Args>(args)...));
        }

        template <typename... Args>
        static bool w(DiagnosticsState& state, std::string_view tag, std::string title, Args&&... args) {
            return show(state, core::LogLevel::Warn, tag, std::move(title), join(std::forward<Args>(args)...));
        }

        template <typename... Args>
        static bool e(DiagnosticsState& state, std::string_view tag, std::string title, Args&&... args) {
            return show(state, core::LogLevel::Error, tag, std::move(title), join(std::forward<Args>(args)...));
        }

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
