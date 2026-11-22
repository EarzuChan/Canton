#pragma once

#include <core/InputEditing.h>
#include <core/InputSession.h>
#include <core/LayoutNode.h>
#include <core/Paint.h>
#include <core/SceneFramePipeline.h>
#include <core/TextLayoutService.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace canton::runtime {

    struct TextDragSelection {
        canton::core::TextSelectionRange range;
        canton::core::TextSelectionGranularity granularity = canton::core::TextSelectionGranularity::Character;
    };

    // 单个视口的文本编辑与输入法桥接：实现 core::TextInputBridge，维护显式 compositionRange 与视口滚动，不含任何平台 API。
    class InputTextSession final : public canton::core::TextInputBridge {
       public:
        explicit InputTextSession(const canton::core::TextLayoutService& textLayout = canton::core::defaultTextLayoutService()) noexcept;

        void reset();
        void commitState(InputTextSession&& candidate) noexcept;

        // 当鼠标点击移位、失焦、按 Esc 或外部覆写 value 主动打断未完成组合时通知平台（如 ImmNotifyIME CPS_CANCEL）
        void setCancelCompositionHook(std::function<void()> hook) {
            onCancelPlatformComposition_ = std::move(hook);
        }

        [[nodiscard]] const std::optional<canton::core::NodeId>& focusedNode() const noexcept {
            return focusedNode_;
        }

        [[nodiscard]] canton::core::ModifierHandle focusedModifier() const noexcept {
            return focusedModifier_;
        }

        [[nodiscard]] canton::core::TextInputState& state() noexcept {
            return state_;
        }

        [[nodiscard]] const canton::core::TextInputState& state() const noexcept {
            return state_;
        }

        [[nodiscard]] float viewportX() const noexcept {
            return viewportX_;
        }

        [[nodiscard]] float viewportY() const noexcept {
            return viewportY_;
        }

        [[nodiscard]] bool isActive(const canton::core::LayoutTree& tree, bool runtimeReady) const;
        // 焦点位于可编辑（enabled && !readOnly）的 <Input> 上，允许关联系统输入法上下文（HIMC）
        [[nodiscard]] bool isEditableActive(const canton::core::LayoutTree& tree, bool runtimeReady) const;

        // TextInputBridge 接口
        void focus(const canton::core::LayoutTree& tree, canton::core::NodeId id, canton::core::ModifierHandle receiver, const canton::core::InputCallbacks& callbacks) override;
        void finishFocusedInput(const canton::core::LayoutTree& tree, bool submit, const canton::core::InputCallbacks& callbacks) override;
        void pointerDown(canton::core::LayoutTree& tree, const canton::core::HitTestResult& hit, float x, float y, const canton::core::InputCallbacks& callbacks, canton::core::PointerDownOptions options) override;
        bool pointerDrag(canton::core::LayoutTree& tree, bool runtimeReady, float x, float y, const canton::core::InputCallbacks& callbacks) override;
        void cancelDrag() noexcept override;
        bool keyPressed(canton::core::LayoutTree& tree, bool runtimeReady, const canton::core::KeyInput& key, const canton::core::InputCallbacks& callbacks) override;
        void synchronizePublishedInput(const canton::core::LayoutTree& tree, bool runtimeReady) override;

        // 平台字符输入（WM_CHAR 拼合 UTF-16 代理对后的 UTF-8）与 IMM32 组合输入入口
        bool insertText(const canton::core::LayoutTree& tree, bool runtimeReady, std::string_view utf8Text, const canton::core::InputCallbacks& callbacks);
        bool setComposition(const canton::core::LayoutTree& tree, bool runtimeReady, std::string preedit, std::size_t cursorOffsetBytes, std::vector<canton::core::TextCompositionClause> clauses, const canton::core::InputCallbacks& callbacks);
        bool commitComposition(const canton::core::LayoutTree& tree, bool runtimeReady, std::optional<std::string> committed, const canton::core::InputCallbacks& callbacks);
        bool cancelComposition(const canton::core::LayoutTree& tree, bool runtimeReady, const canton::core::InputCallbacks& callbacks, bool notifyPlatform = false);

        void updateViewport(const canton::core::LayoutTree& tree, bool runtimeReady);
        // 当前光标在窗口根坐标系（物理像素）下的矩形，供 ImmSetCandidateWindow 定位系统候选框
        [[nodiscard]] std::optional<canton::core::Rect> caretBoundsInRoot(const canton::core::LayoutTree& tree, bool runtimeReady) const;
        [[nodiscard]] std::vector<canton::core::DrawOp> buildOverlayOps(const canton::core::LayoutTree& tree, bool runtimeReady) const;
        void populatePublishedFrame(const canton::core::LayoutTree& tree, bool runtimeReady, canton::core::PublishedFrameContent& content);

       private:
        const canton::core::LayoutNode* activeNode(const canton::core::LayoutTree& tree, bool runtimeReady) const;
        canton::core::LayoutNode* activeNode(canton::core::LayoutTree& tree, bool runtimeReady);
        const canton::core::ModifierInstance& inputInstance(const canton::core::LayoutNode& node) const;
        canton::core::TextSelectionRange selectionAtPoint(const canton::core::ModifierInstance& instance, canton::core::Point localPoint, canton::core::TextSelectionGranularity granularity) const;
        std::string normalizeInsertionText(const canton::core::ModifierInstance& instance, std::string_view text) const;
        void updateViewportForInstance(const canton::core::ModifierInstance& instance);
        bool applyEdit(const canton::core::LayoutNode& node, const canton::core::InputEditResult& edit, const canton::core::InputCallbacks& callbacks, bool imeIntent = false);
        void abortActiveComposition(bool notifyPlatform);

        const canton::core::TextLayoutService* textLayout_;
        std::optional<canton::core::NodeId> focusedNode_;
        std::uint64_t focusedGeneration_ = 0;
        canton::core::ModifierHandle focusedModifier_;
        std::string publishedModelValue_;
        bool readOnly_ = false;
        std::optional<TextDragSelection> dragAnchor_;
        canton::core::TextInputState state_;
        float viewportX_ = 0.0f;
        float viewportY_ = 0.0f;
        std::optional<float> preferredX_;
        std::function<void()> onCancelPlatformComposition_;
    };

}
