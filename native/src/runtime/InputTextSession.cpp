#include <runtime/InputTextSession.h>


namespace canton::runtime {
    void InputTextSession::reset() {
        focusedNode_.reset();
        dragAnchor_.reset();
        state_.reset();
        viewportX_ = 0.0f;
        viewportY_ = 0.0f;
        preferredX_.reset();
        pendingPlatformEdit_ = false;
        compositionCancelled_ = false;
        temporaryUnderlines_.clear();
    }
}

