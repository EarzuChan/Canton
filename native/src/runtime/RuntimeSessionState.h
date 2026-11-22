#pragma once

#include <core/Geometry.h>

#include <filesystem>
#include <string_view>

namespace canton::runtime {

    class CantonRuntime;
    class DiagnosticsState;
    struct RuntimeFramePumpResult;

    class RuntimeSessionState final {
       public:
        void reset(CantonRuntime& runtime, DiagnosticsState& diagnostics);

        void resize(int width, int height, CantonRuntime& runtime) noexcept;
        [[nodiscard]] canton::core::Constraints constraints() const noexcept;

        void markLoaded() noexcept;
        void markUnloaded() noexcept;
        void setLayoutTreeEmptyError(DiagnosticsState& diagnostics, CantonRuntime& runtime);
        [[nodiscard]] bool loaded() const noexcept;
        [[nodiscard]] bool interactive(const DiagnosticsState& diagnostics) const noexcept;

        [[nodiscard]] bool applyFrameError(const RuntimeFramePumpResult& frame, DiagnosticsState& diagnostics, CantonRuntime& runtime, const std::filesystem::path& relatedPath);

       private:
        static constexpr std::string_view TAG = "RuntimeSessionState";
        bool loaded_ = false;
        int width_ = 0;
        int height_ = 0;
    };

}
