#pragma once

#include <core/InputEditing.h>
#include <core/LayoutNode.h>

#include <optional>
#include <vector>

namespace canton::runtime {

    struct TextDragSelection {
        canton::core::TextSelectionRange range;
        canton::core::TextSelectionGranularity granularity = canton::core::TextSelectionGranularity::Character;
    };

    class InputTextSession final {
       public:
        void reset();

        std::optional<canton::core::NodeId>& focusedNode() noexcept {
            return focusedNode_;
        }

        const std::optional<canton::core::NodeId>& focusedNode() const noexcept {
            return focusedNode_;
        }

        std::optional<TextDragSelection>& dragAnchor() noexcept {
            return dragAnchor_;
        }

        const std::optional<TextDragSelection>& dragAnchor() const noexcept {
            return dragAnchor_;
        }

        canton::core::TextInputState& state() noexcept {
            return state_;
        }

        const canton::core::TextInputState& state() const noexcept {
            return state_;
        }

        float& viewportX() noexcept {
            return viewportX_;
        }

        float viewportX() const noexcept {
            return viewportX_;
        }

        float& viewportY() noexcept {
            return viewportY_;
        }

        float viewportY() const noexcept {
            return viewportY_;
        }

        std::optional<float>& preferredX() noexcept {
            return preferredX_;
        }

        bool& pendingPlatformEdit() noexcept {
            return pendingPlatformEdit_;
        }

        bool& compositionCancelled() noexcept {
            return compositionCancelled_;
        }

        std::vector<core::TextSelectionRange>& temporaryUnderlines() noexcept {
            return temporaryUnderlines_;
        }

        const std::vector<core::TextSelectionRange>& temporaryUnderlines() const noexcept {
            return temporaryUnderlines_;
        }

       private:
        std::optional<canton::core::NodeId> focusedNode_;
        std::optional<TextDragSelection> dragAnchor_;
        canton::core::TextInputState state_;
        float viewportX_ = 0.0f;
        float viewportY_ = 0.0f;
        std::optional<float> preferredX_;
        bool pendingPlatformEdit_ = false;
        bool compositionCancelled_ = false;
        std::vector<core::TextSelectionRange> temporaryUnderlines_;
    };

}
