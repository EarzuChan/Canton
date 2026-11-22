#include <runtime/InputTextSession.h>

#include <core/Clock.h>
#include <core/ModifierGeometry.h>
#include <core/TextMetrics.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace canton::runtime {
    namespace {
        float computeUpdatedViewportX(const core::ModifierInstance& instance, const std::string& text, std::size_t cursorIndex, float currentViewportX, const core::TextLayoutService& service) {
            const auto layout = core::TextInputOverlayBuilder::layout(instance, text, currentViewportX, service, 0.0f);
            if (!layout.metrics.singleLine || layout.metrics.textWidth <= 0.0f) return 0.0f;
            const auto caret = service.caretRect(*layout.text, std::min(cursorIndex, text.size()));
            const auto maxScroll = std::max(0.0f, layout.text->width + 2.0f - layout.metrics.textWidth);
            auto next = std::clamp(currentViewportX, 0.0f, maxScroll);
            constexpr float margin = 2.0f;
            if (caret.x < next)
                next = std::max(0.0f, caret.x);
            else if (caret.x + margin > next + layout.metrics.textWidth)
                next = std::min(maxScroll, caret.x + margin - layout.metrics.textWidth);
            return next;
        }

        float computeUpdatedViewportY(const core::ModifierInstance& instance, const std::string& text, std::size_t cursorIndex, float currentViewportY, const core::TextLayoutService& service) {
            const auto layout = core::TextInputOverlayBuilder::layout(instance, text, 0.0f, service, currentViewportY);
            if (layout.metrics.singleLine || layout.metrics.textHeight <= 0.0f) return 0.0f;
            const auto caret = service.caretRect(*layout.text, std::min(cursorIndex, text.size()));
            const auto maxScroll = std::max(0.0f, layout.text->height - layout.metrics.textHeight);
            auto next = std::clamp(currentViewportY, 0.0f, maxScroll);
            if (caret.y < next)
                next = std::max(0.0f, caret.y);
            else if (caret.y + caret.height > next + layout.metrics.textHeight)
                next = std::min(maxScroll, caret.y + caret.height - layout.metrics.textHeight);
            return next;
        }
    }

    InputTextSession::InputTextSession(const core::TextLayoutService& textLayout) noexcept : textLayout_(&textLayout) {}

    void InputTextSession::reset() {
        focusedNode_.reset();
        focusedGeneration_ = 0;
        focusedModifier_ = {};
        publishedModelValue_.clear();
        readOnly_ = false;
        dragAnchor_.reset();
        state_.reset();
        viewportX_ = 0.0f;
        viewportY_ = 0.0f;
        preferredX_.reset();
    }

    void InputTextSession::commitState(InputTextSession&& candidate) noexcept {
        focusedNode_ = candidate.focusedNode_;
        focusedGeneration_ = candidate.focusedGeneration_;
        focusedModifier_ = candidate.focusedModifier_;
        publishedModelValue_ = std::move(candidate.publishedModelValue_);
        readOnly_ = candidate.readOnly_;
        dragAnchor_ = std::move(candidate.dragAnchor_);
        state_ = std::move(candidate.state_);
        viewportX_ = candidate.viewportX_;
        viewportY_ = candidate.viewportY_;
        preferredX_ = candidate.preferredX_;
    }

    const core::LayoutNode* InputTextSession::activeNode(const core::LayoutTree& tree, bool runtimeReady) const {
        if (!runtimeReady || !focusedNode_ || !tree.contains(*focusedNode_)) return nullptr;
        const auto& node = tree.node(*focusedNode_);
        if (node.generation != focusedGeneration_ || !core::nodeInteractionEnabled(tree, node.id)) return nullptr;
        const auto* instance = node.modifier.find(focusedModifier_);
        const auto* field = instance ? std::get_if<core::TextFieldModifier>(&instance->descriptor.value) : nullptr;
        return field && field->enabled ? &node : nullptr;
    }

    core::LayoutNode* InputTextSession::activeNode(core::LayoutTree& tree, bool runtimeReady) {
        const auto* node = std::as_const(*this).activeNode(tree, runtimeReady);
        return node ? &tree.mutableNode(node->id) : nullptr;
    }

    const core::ModifierInstance& InputTextSession::inputInstance(const core::LayoutNode& node) const {
        const auto* instance = node.modifier.find(focusedModifier_);
        if (!instance || !std::holds_alternative<core::TextFieldModifier>(instance->descriptor.value)) throw std::logic_error("文本编辑受体已退休");
        return *instance;
    }

    bool InputTextSession::isActive(const core::LayoutTree& tree, bool runtimeReady) const {
        return activeNode(tree, runtimeReady) != nullptr;
    }

    bool InputTextSession::isEditableActive(const core::LayoutTree& tree, bool runtimeReady) const {
        const auto* node = activeNode(tree, runtimeReady);
        if (!node) return false;
        const auto& field = std::get<core::TextFieldModifier>(inputInstance(*node).descriptor.value);
        return field.enabled && !field.readOnly;
    }

    void InputTextSession::abortActiveComposition(bool notifyPlatform) {
        if (!state_.composing()) return;
        (void)state_.cancelComposition();
        if (notifyPlatform && onCancelPlatformComposition_) onCancelPlatformComposition_();
    }

    void InputTextSession::focus(const core::LayoutTree& tree, core::NodeId id, core::ModifierHandle receiver, const core::InputCallbacks& callbacks) {
        if (activeNode(tree, true) && focusedNode_ == id && focusedModifier_ == receiver) {
            const auto& field = std::get<core::TextFieldModifier>(inputInstance(tree.node(id)).descriptor.value);
            if (field.readOnly && !readOnly_) abortActiveComposition(true);
            readOnly_ = field.readOnly;
            return;
        }
        finishFocusedInput(tree, false, callbacks);
        if (!tree.contains(id) || !core::nodeInteractionEnabled(tree, id)) return;
        const auto* instance = tree.node(id).modifier.find(receiver);
        const auto* field = instance ? std::get_if<core::TextFieldModifier>(&instance->descriptor.value) : nullptr;
        if (!field || !field->enabled) return;
        focusedNode_ = id;
        focusedGeneration_ = tree.node(id).generation;
        focusedModifier_ = receiver;
        publishedModelValue_ = field->value;
        readOnly_ = field->readOnly;
        viewportX_ = 0.0f;
        viewportY_ = 0.0f;
        preferredX_.reset();
        state_.begin(field->value, field->selectAllOnFocus);
        updateViewportForInstance(*instance);
    }

    void InputTextSession::finishFocusedInput(const core::LayoutTree& tree, bool submit, const core::InputCallbacks& callbacks) {
        abortActiveComposition(true);
        const auto* node = activeNode(tree, true);
        if (node != nullptr) {
            const auto& instance = inputInstance(*node);
            if (submit && callbacks.invokeStringEvent) {
                callbacks.invokeStringEvent(core::modifierEventSlot(instance.descriptor.value, core::EventSlotKind::InputSubmit), state_.text());
            }
            if (state_.changedSinceBegin() && callbacks.invokeStringEvent) {
                callbacks.invokeStringEvent(core::modifierEventSlot(instance.descriptor.value, core::EventSlotKind::InputChange), state_.text());
            }
            if (callbacks.invokeStringEvent) {
                callbacks.invokeStringEvent(core::modifierEventSlot(instance.descriptor.value, core::EventSlotKind::InputBlur), state_.text());
            }
            if (callbacks.invalidateNativeState) {
                callbacks.invalidateNativeState(node->id, core::DirtyFlag::Paint);
            }
        }
        reset();
    }

    core::TextSelectionRange InputTextSession::selectionAtPoint(const core::ModifierInstance& instance, core::Point localPoint, core::TextSelectionGranularity granularity) const {
        const auto& text = state_.text();
        if (granularity == core::TextSelectionGranularity::All) return {0, text.size()};
        const auto prepared = core::TextInputOverlayBuilder::layout(instance, text, viewportX_, *textLayout_, viewportY_);
        const core::Point contentPoint{localPoint.x - instance.bounds.x + prepared.metrics.viewportX, localPoint.y - instance.bounds.y + prepared.metrics.viewportY};
        const auto index = textLayout_->byteIndexAtPoint(*prepared.text, contentPoint);
        if (granularity == core::TextSelectionGranularity::Character) return {index, index};
        if (granularity == core::TextSelectionGranularity::Word) return core::wordRangeAt(text, index);
        // VisualLine
        for (auto it = prepared.text->lines.rbegin(); it != prepared.text->lines.rend(); ++it) {
            if (index >= it->start) return {it->start, it->end};
        }
        return {0, text.size()};
    }

    void InputTextSession::pointerDown(core::LayoutTree& tree, const core::HitTestResult& hit, float x, float y, const core::InputCallbacks& callbacks, core::PointerDownOptions options) {
        preferredX_.reset();
        const auto* instance = hit.hit && tree.contains(hit.node) ? tree.node(hit.node).modifier.find(hit.modifier) : nullptr;
        const auto* field = instance ? std::get_if<core::TextFieldModifier>(&instance->descriptor.value) : nullptr;
        if (!field || !field->enabled || !core::nodeInteractionEnabled(tree, hit.node)) {
            finishFocusedInput(tree, false, callbacks);
            return;
        }
        const auto same = activeNode(tree, true) && focusedNode_ == hit.node && focusedModifier_ == hit.modifier;
        if (!same) {
            finishFocusedInput(tree, false, callbacks);
            focusedNode_ = hit.node;
            focusedGeneration_ = tree.node(hit.node).generation;
            focusedModifier_ = hit.modifier;
            publishedModelValue_ = field->value;
            readOnly_ = field->readOnly;
            viewportX_ = 0.0f;
            viewportY_ = 0.0f;
            state_.begin(field->value, field->selectAllOnFocus);
        } else {
            // 鼠标点击移位主动取消未完成组合并通知系统收起候选框
            abortActiveComposition(true);
            readOnly_ = field->readOnly;
        }

        const auto local = core::rootToNodeContent(tree, hit.node, {x, y}, hit.modifier);
        using Granularity = core::TextSelectionGranularity;
        const auto granularity = options.clickCount > 3 ? Granularity::All : options.clickCount == 3 ? Granularity::VisualLine : options.clickCount == 2 ? Granularity::Word : Granularity::Character;
        const auto range = selectionAtPoint(*instance, local, granularity);
        const auto anchor = state_.hasSelection() ? state_.selectionStart() : state_.cursorIndex();
        if (options.extendSelection && same && granularity != Granularity::All) {
            (void)state_.selectRange(anchor, range.end <= anchor ? range.start : range.end);
            dragAnchor_ = TextDragSelection{{anchor, anchor}, granularity};
        } else if (granularity == Granularity::Character) {
            (void)state_.moveCursorTo(range.start);
            dragAnchor_ = TextDragSelection{range, granularity};
        } else {
            (void)state_.selectRange(range.start, range.end);
            if (granularity != Granularity::All) dragAnchor_ = TextDragSelection{range, granularity};
            else dragAnchor_.reset();
        }
        updateViewportForInstance(*instance);
        if (callbacks.invalidateNativeState) callbacks.invalidateNativeState(hit.node, core::DirtyFlag::Paint);
    }

    bool InputTextSession::pointerDrag(core::LayoutTree& tree, bool runtimeReady, float x, float y, const core::InputCallbacks& callbacks) {
        const auto* node = activeNode(tree, runtimeReady);
        if (!node || !dragAnchor_) return false;
        const auto& instance = inputInstance(*node);
        const auto local = core::rootToNodeContent(tree, node->id, {x, y}, focusedModifier_);
        const auto current = selectionAtPoint(instance, local, dragAnchor_->granularity);
        const auto anchor = dragAnchor_->range;
        if (current.end < anchor.start) {
            (void)state_.selectRange(anchor.end, current.start);
        } else if (current.start > anchor.end) {
            (void)state_.selectRange(anchor.start, current.end);
        } else {
            (void)state_.selectRange(anchor.start, dragAnchor_->granularity == core::TextSelectionGranularity::Character ? current.end : anchor.end);
        }
        updateViewportForInstance(instance);
        if (callbacks.invalidateNativeState) callbacks.invalidateNativeState(node->id, core::DirtyFlag::Paint);
        return true;
    }

    void InputTextSession::cancelDrag() noexcept {
        dragAnchor_.reset();
    }

    bool InputTextSession::keyPressed(core::LayoutTree& tree, bool runtimeReady, const core::KeyInput& key, const core::InputCallbacks& callbacks) {
        const auto* node = activeNode(tree, runtimeReady);
        if (node == nullptr) return false;
        const auto& instance = inputInstance(*node);
        const auto& field = std::get<core::TextFieldModifier>(instance.descriptor.value);
        readOnly_ = field.readOnly;
        state_.setEditTime(core::monotonicMillis());

        const auto is = [&](std::string_view name) { return key.key == name; };
        if (!is("ArrowUp") && !is("ArrowDown") && !is("PageUp") && !is("PageDown")) preferredX_.reset();

        const auto commandDown = key.modifiers.command || key.modifiers.ctrl;
        const auto shiftDown = key.modifiers.shift;
        const auto altDown = key.modifiers.alt;
        const auto shortcut = [&](char lower) {
            const auto upper = static_cast<char>(lower - 'a' + 'A');
            return key.key.size() == 1 && (key.key[0] == lower || key.key[0] == upper);
        };

        if (state_.composing()) {
            if (is("Escape")) {
                abortActiveComposition(true);
                updateViewportForInstance(instance);
                if (callbacks.enqueueImeCompositionIntent) callbacks.enqueueImeCompositionIntent(node->id);
                if (callbacks.invalidateNativeState) callbacks.invalidateNativeState(node->id, core::DirtyFlag::Paint);
                return true;
            }
            if (commandDown && shortcut('z')) {
                abortActiveComposition(true);
                updateViewportForInstance(instance);
                if (callbacks.enqueueImeCompositionIntent) callbacks.enqueueImeCompositionIntent(node->id);
                if (callbacks.invalidateNativeState) callbacks.invalidateNativeState(node->id, core::DirtyFlag::Paint);
                return true;
            }
        }

        core::InputEditResult edit;
        if (commandDown && !altDown && shortcut('a')) {
            if (callbacks.enqueueKeyIntent) callbacks.enqueueKeyIntent(node->id);
            edit = state_.selectAll();
        } else if (commandDown && !altDown && shortcut('c')) {
            const auto selected = state_.selectedText();
            if (selected.empty()) return false;
            if (callbacks.enqueueKeyIntent) callbacks.enqueueKeyIntent(node->id);
            if (callbacks.writeClipboard) callbacks.writeClipboard(selected);
            if (callbacks.invalidateNativeState) callbacks.invalidateNativeState(node->id, core::DirtyFlag::Paint);
            return true;
        } else if (commandDown && !altDown && shortcut('x')) {
            if (readOnly_) return true;
            if (callbacks.enqueueKeyIntent) callbacks.enqueueKeyIntent(node->id);
            const auto selected = state_.selectedText();
            if (!selected.empty() && callbacks.writeClipboard) callbacks.writeClipboard(selected);
            edit = state_.cutSelection();
        } else if (commandDown && !altDown && shortcut('v')) {
            if (readOnly_) return true;
            if (callbacks.enqueueKeyIntent) callbacks.enqueueKeyIntent(node->id);
            const auto pasted = callbacks.readClipboard ? callbacks.readClipboard() : std::string{};
            edit = state_.replaceSelectionWithText(normalizeInsertionText(instance, pasted));
        } else if (commandDown && !altDown && shortcut('z')) {
            if (readOnly_) return true;
            if (callbacks.enqueueKeyIntent) callbacks.enqueueKeyIntent(node->id);
            edit = shiftDown ? state_.redo() : state_.undo();
        } else if (commandDown && !altDown && shortcut('y')) {
            if (readOnly_) return true;
            if (callbacks.enqueueKeyIntent) callbacks.enqueueKeyIntent(node->id);
            edit = state_.redo();
        } else if (is("Backspace")) {
            if (readOnly_) return true;
            if (callbacks.enqueueKeyIntent) callbacks.enqueueKeyIntent(node->id);
            edit = commandDown ? state_.deleteWordBackward() : state_.backspace();
        } else if (is("Delete")) {
            if (readOnly_) return true;
            if (callbacks.enqueueKeyIntent) callbacks.enqueueKeyIntent(node->id);
            edit = commandDown ? state_.deleteWordForward() : state_.deleteForward();
        } else if (is("ArrowLeft")) {
            if (callbacks.enqueueKeyIntent) callbacks.enqueueKeyIntent(node->id);
            if (commandDown)
                edit = shiftDown ? state_.extendSelectionWordLeft() : state_.moveWordLeft();
            else
                edit = shiftDown ? state_.extendSelectionLeft() : state_.moveLeft();
        } else if (is("ArrowRight")) {
            if (callbacks.enqueueKeyIntent) callbacks.enqueueKeyIntent(node->id);
            if (commandDown)
                edit = shiftDown ? state_.extendSelectionWordRight() : state_.moveWordRight();
            else
                edit = shiftDown ? state_.extendSelectionRight() : state_.moveRight();
        } else if (is("ArrowUp") || is("ArrowDown") || (core::TextInputOverlayBuilder::allowsLineBreak(instance) && (is("PageUp") || is("PageDown")))) {
            if (callbacks.enqueueKeyIntent) callbacks.enqueueKeyIntent(node->id);
            const auto prepared = core::TextInputOverlayBuilder::layout(instance, state_.text(), viewportX_, *textLayout_, viewportY_);
            const auto caret = textLayout_->caretRect(*prepared.text, state_.cursorIndex());
            if (!preferredX_) preferredX_ = caret.x;
            const auto dir = is("ArrowUp") || is("PageUp") ? -1.0f : 1.0f;
            const auto isPage = is("PageUp") || is("PageDown");
            const auto distance = isPage ? std::max(caret.height, instance.bounds.height) : caret.height;
            const auto index = textLayout_->byteIndexAtPoint(*prepared.text, {*preferredX_, caret.y + caret.height * 0.5f + distance * dir});
            edit = state_.moveTo(index, shiftDown);
        } else if (is("Home") || is("End")) {
            if (callbacks.enqueueKeyIntent) callbacks.enqueueKeyIntent(node->id);
            const auto end = is("End");
            if (commandDown) {
                edit = state_.moveTo(end ? state_.text().size() : 0, shiftDown);
            } else {
                edit = end ? (shiftDown ? state_.extendSelectionEnd() : state_.moveEnd()) : (shiftDown ? state_.extendSelectionHome() : state_.moveHome());
            }
        } else if (is("Enter")) {
            if (callbacks.enqueueKeyIntent) callbacks.enqueueKeyIntent(node->id);
            if (core::TextInputOverlayBuilder::allowsLineBreak(instance) && !commandDown) {
                if (readOnly_) return true;
                edit = state_.insertLineBreak();
            } else {
                edit = state_.submit();
            }
        } else if (!commandDown && !altDown && !key.modifiers.meta && !key.text.empty()) {
            if (readOnly_) return true;
            return insertText(tree, runtimeReady, key.text, callbacks);
        } else {
            return false;
        }

        return applyEdit(*node, edit, callbacks);
    }

    bool InputTextSession::insertText(const core::LayoutTree& tree, bool runtimeReady, std::string_view utf8Text, const core::InputCallbacks& callbacks) {
        const auto* node = activeNode(tree, runtimeReady);
        if (!node || utf8Text.empty()) return false;
        const auto& instance = inputInstance(*node);
        const auto& field = std::get<core::TextFieldModifier>(instance.descriptor.value);
        if (field.readOnly) return false;
        state_.setEditTime(core::monotonicMillis());
        preferredX_.reset();

        core::InputEditResult combined;
        for (std::size_t idx = 0; idx < utf8Text.size();) {
            const auto cp = core::decodeUtf8Codepoint(utf8Text, idx);
            if (cp == U'\r') continue;
            if (cp == U'\n') {
                if (core::TextInputOverlayBuilder::allowsLineBreak(instance)) {
                    const auto step = state_.insertLineBreak();
                    combined.consumed = combined.consumed || step.consumed;
                    combined.textChanged = combined.textChanged || step.textChanged;
                }
                continue;
            }
            const auto step = state_.insertCodepoint(cp);
            combined.consumed = combined.consumed || step.consumed;
            combined.textChanged = combined.textChanged || step.textChanged;
        }
        if (!combined.consumed) return false;
        if (callbacks.enqueueTextInputIntent) callbacks.enqueueTextInputIntent(node->id);
        return applyEdit(*node, combined, callbacks);
    }

    bool InputTextSession::setComposition(const core::LayoutTree& tree, bool runtimeReady, std::string preedit, std::size_t cursorOffsetBytes, std::vector<core::TextCompositionClause> clauses, const core::InputCallbacks& callbacks) {
        const auto* node = activeNode(tree, runtimeReady);
        if (!node) return false;
        const auto& instance = inputInstance(*node);
        const auto& field = std::get<core::TextFieldModifier>(instance.descriptor.value);
        if (field.readOnly) return false;
        preferredX_.reset();
        const auto edit = state_.setComposition(normalizeInsertionText(instance, preedit), cursorOffsetBytes, std::move(clauses));
        if (callbacks.enqueueImeCompositionIntent) callbacks.enqueueImeCompositionIntent(node->id);
        return applyEdit(*node, edit, callbacks, true);
    }

    bool InputTextSession::commitComposition(const core::LayoutTree& tree, bool runtimeReady, std::optional<std::string> committed, const core::InputCallbacks& callbacks) {
        const auto* node = activeNode(tree, runtimeReady);
        if (!node) return false;
        const auto& instance = inputInstance(*node);
        const auto& field = std::get<core::TextFieldModifier>(instance.descriptor.value);
        if (field.readOnly) return false;
        preferredX_.reset();
        std::optional<std::string> normalized;
        if (committed) normalized = normalizeInsertionText(instance, *committed);
        const auto edit = state_.commitComposition(std::move(normalized));
        if (callbacks.enqueueImeCompositionIntent) callbacks.enqueueImeCompositionIntent(node->id);
        return applyEdit(*node, edit, callbacks, true);
    }

    bool InputTextSession::cancelComposition(const core::LayoutTree& tree, bool runtimeReady, const core::InputCallbacks& callbacks, bool notifyPlatform) {
        const auto* node = activeNode(tree, runtimeReady);
        if (!node) return false;
        if (!state_.composing()) return false;
        const auto edit = state_.cancelComposition();
        if (notifyPlatform && onCancelPlatformComposition_) onCancelPlatformComposition_();
        if (callbacks.enqueueImeCompositionIntent) callbacks.enqueueImeCompositionIntent(node->id);
        return applyEdit(*node, edit, callbacks, true);
    }

    void InputTextSession::synchronizePublishedInput(const core::LayoutTree& tree, bool runtimeReady) {
        const auto* node = activeNode(tree, runtimeReady);
        if (!node) {
            abortActiveComposition(true);
            reset();
            return;
        }
        const auto& instance = inputInstance(*node);
        const auto& field = std::get<core::TextFieldModifier>(instance.descriptor.value);
        if (field.readOnly && !readOnly_) abortActiveComposition(true);
        readOnly_ = field.readOnly;
        if (field.value != publishedModelValue_) {
            publishedModelValue_ = field.value;
            // 若当前正在组合输入而外部脚本主动覆写了不同的受控 value，取消未完成组合并同步新值；
            // 若只是上屏/编辑回执回到相同文本，保留光标、选区与撤销栈。
            const auto matchesCommittedOrCurrent = field.value == state_.text();
            if (!matchesCommittedOrCurrent) {
                abortActiveComposition(true);
                state_.replaceExternal(field.value);
                preferredX_.reset();
                dragAnchor_.reset();
            }
        }
        updateViewportForInstance(instance);
    }

    void InputTextSession::updateViewport(const core::LayoutTree& tree, bool runtimeReady) {
        const auto* node = activeNode(tree, runtimeReady);
        if (!node) {
            reset();
            return;
        }
        updateViewportForInstance(inputInstance(*node));
    }

    void InputTextSession::updateViewportForInstance(const core::ModifierInstance& instance) {
        viewportX_ = computeUpdatedViewportX(instance, state_.text(), state_.cursorIndex(), viewportX_, *textLayout_);
        viewportY_ = computeUpdatedViewportY(instance, state_.text(), state_.cursorIndex(), viewportY_, *textLayout_);
    }

    std::optional<core::Rect> InputTextSession::caretBoundsInRoot(const core::LayoutTree& tree, bool runtimeReady) const {
        const auto* node = activeNode(tree, runtimeReady);
        if (!node) return std::nullopt;
        const auto& instance = inputInstance(*node);
        const auto prepared = core::TextInputOverlayBuilder::layout(instance, state_.text(), viewportX_, *textLayout_, viewportY_);
        const auto localCaret = textLayout_->caretRect(*prepared.text, state_.cursorIndex(), {prepared.metrics.textLeft - prepared.metrics.viewportX, prepared.metrics.textTop - prepared.metrics.viewportY});
        return core::nodeContentRectToRoot(tree, node->id, localCaret, focusedModifier_);
    }

    std::vector<core::DrawOp> InputTextSession::buildOverlayOps(const core::LayoutTree& tree, bool runtimeReady) const {
        const auto* node = activeNode(tree, runtimeReady);
        if (!node) return {};
        core::TextInputOverlayState overlay;
        overlay.text = state_.text();
        overlay.cursorIndex = state_.cursorIndex();
        overlay.selectionStart = state_.selectionStart();
        overlay.selectionEnd = state_.selectionEnd();
        overlay.viewportX = viewportX_;
        overlay.viewportY = viewportY_;
        for (const auto& clause : state_.compositionClauses()) {
            overlay.compositionUnderlines.push_back({clause.start, clause.end, clause.target});
        }
        auto receiver = inputInstance(*node);
        receiver.bounds.x = 0.0f;
        receiver.bounds.y = 0.0f;
        return core::TextInputOverlayBuilder{}.build(node->id, receiver, overlay, *textLayout_);
    }

    void InputTextSession::populatePublishedFrame(const core::LayoutTree& tree, bool runtimeReady, core::PublishedFrameContent& content) {
        const auto* node = activeNode(tree, runtimeReady);
        if (!node) {
            content.focusedInputNode.reset();
            content.focusedInputModifier = {};
            content.focusedInputViewportX = 0.0f;
            content.overlayDrawOps.clear();
            return;
        }
        updateViewportForInstance(inputInstance(*node));
        content.focusedInputNode = node->id;
        content.focusedInputModifier = focusedModifier_;
        content.focusedInputViewportX = viewportX_;
        content.overlayDrawOps = buildOverlayOps(tree, runtimeReady);
    }

    std::string InputTextSession::normalizeInsertionText(const core::ModifierInstance& instance, std::string_view text) const {
        std::string normalized;
        normalized.reserve(text.size());
        const auto multiline = core::TextInputOverlayBuilder::allowsLineBreak(instance);
        for (std::size_t i = 0; i < text.size(); ++i) {
            auto ch = text[i];
            if (ch == '\r') {
                if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
                ch = '\n';
            }
            if (static_cast<unsigned char>(ch) < 32u && ch != '\n' && ch != '\t') continue;
            normalized += !multiline && ch == '\n' ? ' ' : ch;
        }
        return normalized;
    }

    bool InputTextSession::applyEdit(const core::LayoutNode& node, const core::InputEditResult& edit, const core::InputCallbacks& callbacks, bool) {
        if (!edit.consumed) return false;
        const auto& instance = inputInstance(node);
        updateViewportForInstance(instance);
        if (callbacks.invalidateNativeState) callbacks.invalidateNativeState(node.id, core::DirtyFlag::Paint);

        if (edit.submitRequested) {
            if (callbacks.invokeStringEvent) {
                callbacks.invokeStringEvent(core::modifierEventSlot(instance.descriptor.value, core::EventSlotKind::InputSubmit), state_.text());
            }
            return true;
        }

        // 组合进行中不向 JS 发送 onValueChange，仅在真正上屏或常规编辑时发送
        if (edit.textChanged && !state_.composing()) {
            if (callbacks.invokeStringEvent) {
                callbacks.invokeStringEvent(core::modifierEventSlot(instance.descriptor.value, core::EventSlotKind::InputUpdate), state_.text());
            }
        }
        return true;
    }
}
