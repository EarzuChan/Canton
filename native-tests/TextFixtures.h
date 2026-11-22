#pragma once

#include <core/LayoutNode.h>
#include <stdexcept>
#include <utility>

namespace test_support {
    inline canton::core::TextModifier text(std::string value, std::uint32_t color = 0xff000000u, float fontSize = 16, float lineHeight = 0) {
        canton::core::TextModifier result;
        result.text = std::move(value);
        result.color = color;
        result.style.fontSize = fontSize;
        result.style.lineHeight = lineHeight;
        return result;
    }

    inline canton::core::TextFieldModifier textField(std::string value, std::string placeholder = {}, std::uint32_t color = 0xff000000u, float fontSize = 16) {
        canton::core::TextFieldModifier result;
        result.value = std::move(value);
        result.placeholder = std::move(placeholder);
        result.presentation = text({}, color, fontSize);
        return result;
    }

    inline std::string textOf(const canton::core::LayoutNode& node) {
        for (const auto& instance : node.modifier.elements()) {
            if (const auto* value = std::get_if<canton::core::TextModifier>(&instance.descriptor.value)) return value->text;
            if (const auto* value = std::get_if<canton::core::TextFieldModifier>(&instance.descriptor.value)) return value->value;
        }
        return {};
    }

    inline const std::shared_ptr<const canton::core::TextLayout>& textLayoutOf(const canton::core::LayoutNode& node) {
        for (const auto& instance : node.modifier.elements())
            if (canton::core::textPresentation(instance.descriptor.value)) return instance.textLayout;
        throw std::logic_error("夹具节点没有文本 Modifier");
    }

    inline const canton::core::ModifierInstance* editable(const canton::core::LayoutNode& node) {
        for (const auto& instance : node.modifier.elements())
            if (std::holds_alternative<canton::core::TextFieldModifier>(instance.descriptor.value)) return &instance;
        return nullptr;
    }

    inline canton::core::EventSlotId event(const canton::core::LayoutNode& node, canton::core::EventSlotKind kind) {
        for (const auto& instance : node.modifier.elements()) {
            const auto slot = canton::core::modifierEventSlot(instance.descriptor.value, kind);
            if (slot.valid()) return slot;
        }
        return {};
    }
}
