#include <core/InputEditing.h>
#include <core/TextMetrics.h>

#include <algorithm>
#include <string>

namespace canton::core {
    namespace {
        constexpr std::size_t maxUndoStackDepth = 128;

        std::string encodeUtf8(char32_t codepoint) {
            if (codepoint <= 0x7fu) {
                return std::string(1, static_cast<char>(codepoint));
            }
            if (codepoint <= 0x7ffu) {
                return {
                    static_cast<char>(0xc0u | ((codepoint >> 6u) & 0x1fu)),
                    static_cast<char>(0x80u | (codepoint & 0x3fu)),
                };
            }
            if (codepoint <= 0xffffu) {
                return {
                    static_cast<char>(0xe0u | ((codepoint >> 12u) & 0x0fu)),
                    static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)),
                    static_cast<char>(0x80u | (codepoint & 0x3fu)),
                };
            }
            return {
                static_cast<char>(0xf0u | ((codepoint >> 18u) & 0x07u)),
                static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3fu)),
                static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)),
                static_cast<char>(0x80u | (codepoint & 0x3fu)),
            };
        }

        bool isSupportedScalar(char32_t codepoint) {
            if (codepoint < 32u || codepoint == 127u) return false;
            if (codepoint > 0x10ffffu) return false;
            return !(codepoint >= 0xd800u && codepoint <= 0xdfffu);
        }

        bool isUtf8Continuation(unsigned char ch) {
            return (ch & 0xc0u) == 0x80u;
        }

        std::size_t clampUtf8(std::string_view text, std::size_t index) {
            if (index >= text.size()) return text.size();
            while (index > 0 && isUtf8Continuation(static_cast<unsigned char>(text[index]))) --index;
            return index;
        }

        std::size_t previousUtf8Boundary(std::string_view text, std::size_t cursor) {
            if (cursor == 0 || text.empty()) return 0;
            auto pos = std::min(cursor - 1, text.size() - 1);
            while (pos > 0 && isUtf8Continuation(static_cast<unsigned char>(text[pos]))) --pos;
            return pos;
        }

        std::size_t nextUtf8Boundary(std::string_view text, std::size_t cursor) {
            if (cursor >= text.size()) return text.size();
            auto pos = cursor + 1;
            while (pos < text.size() && isUtf8Continuation(static_cast<unsigned char>(text[pos]))) ++pos;
            return pos;
        }

        enum class CharClass { LineBreak, Space, Word, Cjk, Punctuation };

        CharClass classifyCodepoint(char32_t cp) {
            if (cp == U'\n' || cp == U'\r') return CharClass::LineBreak;
            if (cp == U' ' || cp == U'\t' || cp == 0x00a0u || cp == 0x3000u) return CharClass::Space;
            if ((cp >= U'a' && cp <= U'z') || (cp >= U'A' && cp <= U'Z') || (cp >= U'0' && cp <= U'9') || cp == U'_' || (cp >= 0x00c0u && cp <= 0x024fu)) {
                return CharClass::Word;
            }
            if ((cp >= 0x4e00u && cp <= 0x9fffu) || (cp >= 0x3400u && cp <= 0x4dbfu) || (cp >= 0x3040u && cp <= 0x30ffu) || (cp >= 0xac00u && cp <= 0xd7afu)) {
                return CharClass::Cjk;
            }
            return CharClass::Punctuation;
        }

        CharClass classAt(std::string_view text, std::size_t boundary) {
            auto idx = boundary;
            return classifyCodepoint(decodeUtf8Codepoint(text, idx));
        }

        std::size_t previousWordBoundary(std::string_view text, std::size_t cursor) {
            auto pos = clampUtf8(text, cursor);
            if (pos == 0) return 0;
            auto prev = previousUtf8Boundary(text, pos);
            if (classAt(text, prev) == CharClass::LineBreak) return prev;
            while (pos > 0) {
                const auto step = previousUtf8Boundary(text, pos);
                if (classAt(text, step) != CharClass::Space) break;
                pos = step;
            }
            if (pos == 0) return 0;
            const auto target = classAt(text, previousUtf8Boundary(text, pos));
            if (target == CharClass::LineBreak) return pos;
            while (pos > 0) {
                const auto step = previousUtf8Boundary(text, pos);
                if (classAt(text, step) != target) break;
                pos = step;
            }
            return pos;
        }

        std::size_t nextWordBoundary(std::string_view text, std::size_t cursor) {
            auto pos = clampUtf8(text, cursor);
            if (pos >= text.size()) return text.size();
            const auto first = classAt(text, pos);
            if (first == CharClass::LineBreak) return nextUtf8Boundary(text, pos);
            if (first != CharClass::Space) {
                while (pos < text.size() && classAt(text, pos) == first) {
                    pos = nextUtf8Boundary(text, pos);
                }
            }
            while (pos < text.size() && classAt(text, pos) == CharClass::Space) {
                pos = nextUtf8Boundary(text, pos);
            }
            return pos;
        }
    }

    TextSelectionRange wordRangeAt(std::string_view text, std::size_t byteIndex) {
        if (text.empty()) return {0, 0};
        auto pos = clampUtf8(text, std::min(byteIndex, text.size()));
        if (pos == text.size()) pos = previousUtf8Boundary(text, pos);
        const auto kind = classAt(text, pos);
        if (kind == CharClass::LineBreak) return {pos, pos};
        auto start = pos;
        while (start > 0) {
            const auto prev = previousUtf8Boundary(text, start);
            if (classAt(text, prev) != kind) break;
            start = prev;
        }
        auto end = nextUtf8Boundary(text, pos);
        while (end < text.size() && classAt(text, end) == kind) {
            end = nextUtf8Boundary(text, end);
        }
        return {start, end};
    }

    void TextInputState::begin(std::string value, bool selectAll) {
        text_ = std::move(value);
        committedText_ = text_;
        cursorIndex_ = text_.size();
        undoStack_.clear();
        redoStack_.clear();
        composition_.reset();
        compositionClauses_.clear();
        breakUndoGroup();
        clearSelection();
        if (selectAll && !text_.empty()) {
            selectionStart_ = 0;
            selectionEnd_ = text_.size();
            cursorIndex_ = selectionEnd_;
        }
    }

    void TextInputState::replaceExternal(std::string value) {
        composition_.reset();
        compositionClauses_.clear();
        breakUndoGroup();
        text_ = std::move(value);
        committedText_ = text_;
        cursorIndex_ = clampToBoundary(cursorIndex_);
        selectionStart_ = clampToBoundary(selectionStart_);
        selectionEnd_ = clampToBoundary(selectionEnd_);
        undoStack_.clear();
        redoStack_.clear();
    }

    void TextInputState::reset() {
        composition_.reset();
        compositionClauses_.clear();
        breakUndoGroup();
        text_.clear();
        committedText_.clear();
        cursorIndex_ = 0;
        undoStack_.clear();
        redoStack_.clear();
        clearSelection();
    }

    InputEditResult TextInputState::setComposition(std::string preedit, std::size_t cursorOffsetBytes, std::vector<TextCompositionClause> clauses) {
        if (!composition_) {
            const auto before = snapshot();
            (void)deleteSelection();
            composition_ = CompositionSession{before, cursorIndex_, cursorIndex_};
            breakUndoGroup();
        }
        text_.replace(composition_->start, composition_->end - composition_->start, preedit);
        composition_->end = composition_->start + preedit.size();
        const auto clampedCursor = clampUtf8(preedit, std::min(cursorOffsetBytes, preedit.size()));
        cursorIndex_ = composition_->start + clampedCursor;
        clearSelection();

        compositionClauses_.clear();
        if (!preedit.empty()) {
            if (clauses.empty()) {
                compositionClauses_.push_back({composition_->start, composition_->end, false});
            } else {
                for (const auto& clause : clauses) {
                    const auto relStart = clampUtf8(preedit, std::min(clause.start, preedit.size()));
                    const auto relEnd = clampUtf8(preedit, std::min(std::max(clause.start, clause.end), preedit.size()));
                    if (relEnd > relStart) {
                        compositionClauses_.push_back({composition_->start + relStart, composition_->start + relEnd, clause.target});
                    }
                }
                if (compositionClauses_.empty()) {
                    compositionClauses_.push_back({composition_->start, composition_->end, false});
                }
            }
        }
        return {true, false, false};
    }

    InputEditResult TextInputState::commitComposition(std::optional<std::string> committed) {
        if (!composition_) {
            if (!committed) return {};
            if (committed->empty() && !hasSelection()) return {true, false, false};
            const auto before = snapshot();
            (void)deleteSelection();
            text_.insert(cursorIndex_, *committed);
            cursorIndex_ += committed->size();
            clearSelection();
            if (text_ != before.text) pushUndoSnapshot(before);
            breakUndoGroup();
            return {true, text_ != before.text, false};
        }

        const auto session = *composition_;
        composition_.reset();
        compositionClauses_.clear();
        if (committed) {
            text_.replace(session.start, session.end - session.start, *committed);
            cursorIndex_ = session.start + committed->size();
            clearSelection();
        } else {
            cursorIndex_ = session.end;
            clearSelection();
        }
        const auto changed = text_ != session.before.text;
        if (changed) pushUndoSnapshot(session.before);
        breakUndoGroup();
        return {true, changed, false};
    }

    InputEditResult TextInputState::cancelComposition() {
        if (!composition_) return {};
        const auto before = composition_->before;
        composition_.reset();
        compositionClauses_.clear();
        restoreSnapshot(before);
        breakUndoGroup();
        return {true, false, false};
    }

    InputEditResult TextInputState::insertCodepoint(char32_t codepoint) {
        if (!isSupportedScalar(codepoint)) return {};
        if (composition_) (void)commitComposition();
        recordUndoPoint(EditKind::Typing);
        (void)deleteSelection();
        const auto text = encodeUtf8(codepoint);
        text_.insert(cursorIndex_, text);
        cursorIndex_ += text.size();
        clearSelection();
        return {true, true, false};
    }

    InputEditResult TextInputState::insertLineBreak() {
        if (composition_) (void)commitComposition();
        recordUndoPoint();
        (void)deleteSelection();
        text_.insert(cursorIndex_, "\n");
        ++cursorIndex_;
        clearSelection();
        return {true, true, false};
    }

    InputEditResult TextInputState::backspace() {
        if (hasSelection()) {
            recordUndoPoint();
            (void)deleteSelection();
            return {true, true, false};
        }
        if (cursorIndex_ == 0 || text_.empty()) return {};
        recordUndoPoint(EditKind::Backward);
        const auto start = previousUtf8Boundary(text_, cursorIndex_);
        text_.erase(start, cursorIndex_ - start);
        cursorIndex_ = start;
        clearSelection();
        return {true, true, false};
    }

    InputEditResult TextInputState::deleteForward() {
        if (hasSelection()) {
            recordUndoPoint();
            (void)deleteSelection();
            return {true, true, false};
        }
        if (cursorIndex_ >= text_.size()) return {};
        recordUndoPoint(EditKind::Forward);
        const auto end = nextUtf8Boundary(text_, cursorIndex_);
        text_.erase(cursorIndex_, end - cursorIndex_);
        clearSelection();
        return {true, true, false};
    }

    InputEditResult TextInputState::deleteWordBackward() {
        if (hasSelection()) {
            recordUndoPoint();
            (void)deleteSelection();
            return {true, true, false};
        }
        if (cursorIndex_ == 0 || text_.empty()) return {};
        recordUndoPoint();
        const auto start = previousWordBoundary(text_, cursorIndex_);
        text_.erase(start, cursorIndex_ - start);
        cursorIndex_ = start;
        clearSelection();
        return {true, true, false};
    }

    InputEditResult TextInputState::deleteWordForward() {
        if (hasSelection()) {
            recordUndoPoint();
            (void)deleteSelection();
            return {true, true, false};
        }
        if (cursorIndex_ >= text_.size()) return {};
        recordUndoPoint();
        const auto end = nextWordBoundary(text_, cursorIndex_);
        text_.erase(cursorIndex_, end - cursorIndex_);
        clearSelection();
        return {true, true, false};
    }

    InputEditResult TextInputState::moveLeft() {
        breakUndoGroup();
        if (hasSelection()) {
            cursorIndex_ = std::min(selectionStart_, selectionEnd_);
            clearSelection();
            return {true, false, false};
        }
        cursorIndex_ = previousUtf8Boundary(text_, cursorIndex_);
        clearSelection();
        return {true, false, false};
    }

    InputEditResult TextInputState::moveRight() {
        breakUndoGroup();
        if (hasSelection()) {
            cursorIndex_ = std::max(selectionStart_, selectionEnd_);
            clearSelection();
            return {true, false, false};
        }
        cursorIndex_ = nextUtf8Boundary(text_, cursorIndex_);
        clearSelection();
        return {true, false, false};
    }

    InputEditResult TextInputState::moveWordLeft() {
        breakUndoGroup();
        const auto base = hasSelection() ? std::min(selectionStart_, selectionEnd_) : cursorIndex_;
        cursorIndex_ = previousWordBoundary(text_, base);
        clearSelection();
        return {true, false, false};
    }

    InputEditResult TextInputState::moveWordRight() {
        breakUndoGroup();
        const auto base = hasSelection() ? std::max(selectionStart_, selectionEnd_) : cursorIndex_;
        cursorIndex_ = nextWordBoundary(text_, base);
        clearSelection();
        return {true, false, false};
    }

    InputEditResult TextInputState::moveHome() {
        breakUndoGroup();
        auto start = cursorIndex_;
        while (start > 0 && text_[start - 1] != '\n') --start;
        cursorIndex_ = start;
        clearSelection();
        return {true, false, false};
    }

    InputEditResult TextInputState::moveEnd() {
        breakUndoGroup();
        auto end = cursorIndex_;
        while (end < text_.size() && text_[end] != '\n') ++end;
        cursorIndex_ = end;
        clearSelection();
        return {true, false, false};
    }

    InputEditResult TextInputState::extendSelectionLeft() {
        const auto anchor = hasSelection() ? selectionStart_ : cursorIndex_;
        const auto active = previousUtf8Boundary(text_, cursorIndex_);
        return selectRange(anchor, active);
    }

    InputEditResult TextInputState::extendSelectionRight() {
        const auto anchor = hasSelection() ? selectionStart_ : cursorIndex_;
        const auto active = nextUtf8Boundary(text_, cursorIndex_);
        return selectRange(anchor, active);
    }

    InputEditResult TextInputState::extendSelectionWordLeft() {
        const auto anchor = hasSelection() ? selectionStart_ : cursorIndex_;
        const auto active = previousWordBoundary(text_, cursorIndex_);
        return selectRange(anchor, active);
    }

    InputEditResult TextInputState::extendSelectionWordRight() {
        const auto anchor = hasSelection() ? selectionStart_ : cursorIndex_;
        const auto active = nextWordBoundary(text_, cursorIndex_);
        return selectRange(anchor, active);
    }

    InputEditResult TextInputState::extendSelectionHome() {
        const auto anchor = hasSelection() ? selectionStart_ : cursorIndex_;
        auto active = cursorIndex_;
        while (active > 0 && text_[active - 1] != '\n') --active;
        return selectRange(anchor, active);
    }

    InputEditResult TextInputState::extendSelectionEnd() {
        const auto anchor = hasSelection() ? selectionStart_ : cursorIndex_;
        auto active = cursorIndex_;
        while (active < text_.size() && text_[active] != '\n') ++active;
        return selectRange(anchor, active);
    }

    InputEditResult TextInputState::moveCursorTo(std::size_t index) {
        breakUndoGroup();
        cursorIndex_ = clampToBoundary(index);
        clearSelection();
        return {true, false, false};
    }

    InputEditResult TextInputState::selectAll() {
        breakUndoGroup();
        selectionStart_ = 0;
        selectionEnd_ = text_.size();
        cursorIndex_ = selectionEnd_;
        return {true, false, false};
    }

    InputEditResult TextInputState::selectRange(std::size_t anchor, std::size_t active) {
        breakUndoGroup();
        selectionStart_ = clampToBoundary(anchor);
        selectionEnd_ = clampToBoundary(active);
        cursorIndex_ = selectionEnd_;
        return {true, false, false};
    }

    std::string TextInputState::selectedText() const {
        if (!hasSelection()) return {};
        const auto start = std::min(selectionStart_, selectionEnd_);
        const auto end = std::max(selectionStart_, selectionEnd_);
        return text_.substr(start, end - start);
    }

    InputEditResult TextInputState::cutSelection() {
        if (!hasSelection()) return {};
        if (composition_) (void)commitComposition();
        recordUndoPoint();
        (void)deleteSelection();
        return {true, true, false};
    }

    InputEditResult TextInputState::replaceSelectionWithText(std::string text, bool typing) {
        if (text.empty() && !hasSelection()) return {};
        if (composition_) (void)commitComposition();
        recordUndoPoint(typing ? EditKind::Typing : EditKind::Atomic);
        (void)deleteSelection();
        text_.insert(cursorIndex_, text);
        cursorIndex_ += text.size();
        clearSelection();
        return {true, true, false};
    }

    InputEditResult TextInputState::undo() {
        if (composing()) return cancelComposition();
        breakUndoGroup();
        if (undoStack_.empty()) return {};
        redoStack_.push_back(snapshot());
        const auto previous = undoStack_.back();
        undoStack_.pop_back();
        restoreSnapshot(previous);
        return {true, true, false};
    }

    InputEditResult TextInputState::redo() {
        if (composing()) return {};
        breakUndoGroup();
        if (redoStack_.empty()) return {};
        undoStack_.push_back(snapshot());
        const auto next = redoStack_.back();
        redoStack_.pop_back();
        restoreSnapshot(next);
        return {true, true, false};
    }

    InputEditResult TextInputState::submit() const noexcept {
        return {true, false, true};
    }

    TextInputState::Snapshot TextInputState::snapshot() const {
        return {text_, cursorIndex_, selectionStart_, selectionEnd_};
    }

    void TextInputState::restoreSnapshot(const Snapshot& snapshot) {
        text_ = snapshot.text;
        cursorIndex_ = clampToBoundary(snapshot.cursorIndex);
        selectionStart_ = clampToBoundary(snapshot.selectionStart);
        selectionEnd_ = clampToBoundary(snapshot.selectionEnd);
    }

    void TextInputState::breakUndoGroup() noexcept {
        undoGroup_ = EditKind::Atomic;
        lastEditTime_ = -1;
    }

    InputEditResult TextInputState::moveTo(std::size_t index, bool extend) {
        const auto anchor = hasSelection() ? selectionStart_ : cursorIndex_;
        return extend ? selectRange(anchor, index) : moveCursorTo(index);
    }

    void TextInputState::pushUndoSnapshot(const Snapshot& value) {
        if (!undoStack_.empty() && sameSnapshot(undoStack_.back(), value)) {
            redoStack_.clear();
            return;
        }
        undoStack_.push_back(value);
        if (undoStack_.size() > maxUndoStackDepth) undoStack_.erase(undoStack_.begin());
        redoStack_.clear();
    }

    void TextInputState::recordUndoPoint(EditKind kind) {
        if (composition_) return;
        const auto current = snapshot();
        const auto grouped = kind != EditKind::Atomic && kind == undoGroup_ && !hasSelection() && editTime_ >= 0 && lastEditTime_ >= 0 && editTime_ >= lastEditTime_ && editTime_ - lastEditTime_ <= 750;
        undoGroup_ = kind;
        lastEditTime_ = editTime_;
        if (grouped) {
            redoStack_.clear();
            return;
        }
        pushUndoSnapshot(current);
    }

    bool TextInputState::sameSnapshot(const Snapshot& left, const Snapshot& right) {
        return left.text == right.text && left.cursorIndex == right.cursorIndex && left.selectionStart == right.selectionStart && left.selectionEnd == right.selectionEnd;
    }

    std::size_t TextInputState::clampToBoundary(std::size_t index) const {
        return clampUtf8(text_, index);
    }

    void TextInputState::clearSelection() {
        selectionStart_ = cursorIndex_;
        selectionEnd_ = cursorIndex_;
    }

    bool TextInputState::deleteSelection() {
        if (!hasSelection()) return false;
        const auto start = std::min(selectionStart_, selectionEnd_);
        const auto end = std::max(selectionStart_, selectionEnd_);
        text_.erase(start, end - start);
        cursorIndex_ = start;
        clearSelection();
        return true;
    }
}
