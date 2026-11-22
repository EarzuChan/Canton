#pragma once

#include <core/EventSlot.h>

#include <span>
#include <string_view>

namespace canton::quickjs {
    struct ModifierCallbackField {
        std::string_view field;
        canton::core::EventSlotKind kind;
    };

    struct ModifierProtocolEntry {
        std::string_view type;
        std::span<const std::string_view> fields;
        std::span<const ModifierCallbackField> callbacks;
    };

    namespace protocol {
#include "ModifierNativeProtocol.inc"
    }

    inline const ModifierProtocolEntry* findModifierProtocol(std::string_view type) noexcept {
        for (const auto& entry : protocol::modifierProtocolEntries)
            if (entry.type == type) return &entry;
        return nullptr;
    }

    inline canton::core::EventSlotKind modifierCallbackKind(const ModifierProtocolEntry& entry, std::string_view field) noexcept {
        for (const auto& callback : entry.callbacks)
            if (callback.field == field) return callback.kind;
        return canton::core::EventSlotKind::None;
    }
}
