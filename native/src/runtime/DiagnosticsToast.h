#pragma once

#include <core/Log.h>
#include <runtime/DiagnosticsTypes.h>

#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace canton::runtime {

    class DiagnosticsModel;

    class DiagnosticsToast final {
       public:
        static bool show(DiagnosticsModel& model, core::LogLevel level, std::string_view tag, std::string title, std::string content, bool coalesce = true);

        template <typename... Args>
        static bool v(DiagnosticsModel& model, std::string_view tag, std::string title, Args&&... args) {
            return show(model, core::LogLevel::Verbose, tag, std::move(title), join(std::forward<Args>(args)...));
        }

        template <typename... Args>
        static bool d(DiagnosticsModel& model, std::string_view tag, std::string title, Args&&... args) {
            return show(model, core::LogLevel::Debug, tag, std::move(title), join(std::forward<Args>(args)...));
        }

        template <typename... Args>
        static bool i(DiagnosticsModel& model, std::string_view tag, std::string title, Args&&... args) {
            return show(model, core::LogLevel::Info, tag, std::move(title), join(std::forward<Args>(args)...));
        }

        template <typename... Args>
        static bool w(DiagnosticsModel& model, std::string_view tag, std::string title, Args&&... args) {
            return show(model, core::LogLevel::Warn, tag, std::move(title), join(std::forward<Args>(args)...));
        }

        template <typename... Args>
        static bool e(DiagnosticsModel& model, std::string_view tag, std::string title, Args&&... args) {
            return show(model, core::LogLevel::Error, tag, std::move(title), join(std::forward<Args>(args)...));
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
