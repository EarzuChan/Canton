#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace canton::core {
    enum class TextSelectionGranularity { Character, Word, VisualLine, All };

    struct TextSelectionRange {
        std::size_t start = 0;
        std::size_t end = 0;
        bool operator==(const TextSelectionRange&) const = default;
    };

    // 预编辑文本内的子句区间：start/end 为相对整个 text() 的 UTF-8 字节下标（调用 setComposition 时也可传相对预编辑串的字节下标，内部统一归一化为全串字节下标）
    struct TextCompositionClause {
        std::size_t start = 0;
        std::size_t end = 0;
        // 对应 Win32 IMM32 ATTR_TARGET_CONVERTED / ATTR_TARGET_NOTCONVERTED，绘制重下划线
        bool target = false;
        bool operator==(const TextCompositionClause&) const = default;
    };

    struct InputEditResult {
        bool consumed = false;
        bool textChanged = false;
        bool submitRequested = false;
    };

    [[nodiscard]] TextSelectionRange wordRangeAt(std::string_view text, std::size_t byteIndex);

    class TextInputState {
       public:
        void begin(std::string value, bool selectAll);
        void reset();
        void replaceExternal(std::string value);

        void setEditTime(double timeMillis) noexcept {
            editTime_ = timeMillis;
        }

        void breakUndoGroup() noexcept;

        [[nodiscard]] bool composing() const noexcept {
            return composition_.has_value();
        }

        [[nodiscard]] std::optional<TextSelectionRange> compositionRange() const noexcept {
            return composition_ ? std::optional<TextSelectionRange>{{composition_->start, composition_->end}} : std::nullopt;
        }

        [[nodiscard]] const std::vector<TextCompositionClause>& compositionClauses() const noexcept {
            return compositionClauses_;
        }

        // 更新预编辑串、段内光标（相对 preedit 起始的 UTF-8 字节偏移）与分段属性（clauses 偏移相对 preedit 起始）。
        // 组合进行中不视为已提交文本变更（textChanged = false），不触发 JS onValueChange。
        InputEditResult setComposition(std::string preedit, std::size_t cursorOffsetBytes, std::vector<TextCompositionClause> clauses = {});
        // 提交组合：若提供 committed 则以该结果串替换当前预编辑区间（若此前未在组合中则直接替换当前选区），整次组合合为单条 Undo。
        InputEditResult commitComposition(std::optional<std::string> committed = std::nullopt);
        // 取消未完成组合并恢复进入组合前的文本与选区快照。
        InputEditResult cancelComposition();

        InputEditResult moveTo(std::size_t index, bool extend);

        [[nodiscard]] const std::string& text() const noexcept {
            return text_;
        }

        [[nodiscard]] const std::string& committedText() const noexcept {
            return committedText_;
        }

        [[nodiscard]] std::size_t cursorIndex() const noexcept {
            return cursorIndex_;
        }

        [[nodiscard]] std::size_t selectionStart() const noexcept {
            return selectionStart_;
        }

        [[nodiscard]] std::size_t selectionEnd() const noexcept {
            return selectionEnd_;
        }

        [[nodiscard]] bool hasSelection() const noexcept {
            return selectionStart_ != selectionEnd_;
        }

        [[nodiscard]] bool changedSinceBegin() const noexcept {
            return text_ != committedText_;
        }

        InputEditResult insertCodepoint(char32_t codepoint);
        InputEditResult insertLineBreak();
        InputEditResult backspace();
        InputEditResult deleteForward();
        InputEditResult deleteWordBackward();
        InputEditResult deleteWordForward();
        InputEditResult moveLeft();
        InputEditResult moveRight();
        InputEditResult moveWordLeft();
        InputEditResult moveWordRight();
        InputEditResult moveHome();
        InputEditResult moveEnd();
        InputEditResult extendSelectionLeft();
        InputEditResult extendSelectionRight();
        InputEditResult extendSelectionWordLeft();
        InputEditResult extendSelectionWordRight();
        InputEditResult extendSelectionHome();
        InputEditResult extendSelectionEnd();
        InputEditResult moveCursorTo(std::size_t index);
        InputEditResult selectAll();
        InputEditResult selectRange(std::size_t anchor, std::size_t active);
        [[nodiscard]] std::string selectedText() const;
        InputEditResult cutSelection();
        InputEditResult replaceSelectionWithText(std::string text, bool typing = false);
        InputEditResult undo();
        InputEditResult redo();
        InputEditResult submit() const noexcept;

       private:
        struct Snapshot {
            std::string text;
            std::size_t cursorIndex = 0;
            std::size_t selectionStart = 0;
            std::size_t selectionEnd = 0;
        };

        struct CompositionSession {
            Snapshot before;
            std::size_t start = 0;
            std::size_t end = 0;
        };

        [[nodiscard]] Snapshot snapshot() const;
        void restoreSnapshot(const Snapshot& snapshot);
        enum class EditKind { Atomic, Typing, Backward, Forward };
        void recordUndoPoint(EditKind kind = EditKind::Atomic);
        void pushUndoSnapshot(const Snapshot& value);
        static bool sameSnapshot(const Snapshot& left, const Snapshot& right);
        [[nodiscard]] std::size_t clampToBoundary(std::size_t index) const;
        void clearSelection();
        bool deleteSelection();

        std::string text_;
        std::string committedText_;
        std::size_t cursorIndex_ = 0;
        std::size_t selectionStart_ = 0;
        std::size_t selectionEnd_ = 0;
        std::vector<Snapshot> undoStack_;
        std::vector<Snapshot> redoStack_;
        std::optional<CompositionSession> composition_;
        std::vector<TextCompositionClause> compositionClauses_;
        EditKind undoGroup_ = EditKind::Atomic;
        double editTime_ = -1;
        double lastEditTime_ = -1;
    };
}
