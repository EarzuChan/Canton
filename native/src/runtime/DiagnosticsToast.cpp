#include <core/Clock.h>
#include <runtime/DiagnosticsToast.h>


#include <runtime/DiagnosticsState.h>

namespace canton::runtime {
    bool DiagnosticsToast::show(DiagnosticsState& state, core::LogLevel level, std::string_view tag, std::string title, std::string content, bool coalesce) {
        const auto visible = state.addToast(level, title, content, core::monotonicMillis(), coalesce);
        const auto message = (visible ? std::string("气泡之《") : std::string("气泡已关闭，本为《")) + title + "》：" + content;
        core::Log::write(level, tag, message);
        return visible;
    }
}

