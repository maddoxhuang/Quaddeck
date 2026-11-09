#pragma once

// One line of text typed into a field the sheet draws itself -- the Emby
// browser's search: the text, the caret and the other end of a selection,
// as UTF-16 indices that never fall between the halves of a surrogate pair.
// The editing model is independent of a window; App feeds it the
// keys and characters, an input method's text and pointer presses, and the
// painter reads it.

#include <algorithm>
#include <cstddef>
#include <cwctype>
#include <string>
#include <string_view>

namespace quaddeck {

struct TextFieldState {
    std::wstring text;
    std::size_t caret{};
    // The selection's other end; the caret's own index when nothing is
    // selected.
    std::size_t anchor{};
};

// What a field holds at most: a search term, not a document.
inline constexpr std::size_t kTextFieldLimit = 256;

inline bool textFieldHighSurrogate(wchar_t c) { return c >= 0xD800 && c <= 0xDBFF; }
inline bool textFieldLowSurrogate(wchar_t c) { return c >= 0xDC00 && c <= 0xDFFF; }

// `index` within the text and off the middle of a surrogate pair.
inline std::size_t textFieldBoundary(const std::wstring& text, std::size_t index) {
    index = std::min(index, text.size());
    if (index > 0 && index < text.size() && textFieldLowSurrogate(text[index]) &&
        textFieldHighSurrogate(text[index - 1])) {
        --index;
    }
    return index;
}

// One character on or back: a surrogate pair is one.
inline std::size_t textFieldNext(const std::wstring& text, std::size_t index) {
    if (index >= text.size()) return text.size();
    ++index;
    if (index < text.size() && textFieldLowSurrogate(text[index]) && textFieldHighSurrogate(text[index - 1])) ++index;
    return index;
}

inline std::size_t textFieldPrevious(const std::wstring& text, std::size_t index) {
    index = std::min(index, text.size());
    if (index == 0) return 0;
    --index;
    if (index > 0 && textFieldLowSurrogate(text[index]) && textFieldHighSurrogate(text[index - 1])) --index;
    return index;
}

// Ctrl with an arrow or an erase: back over spaces and then the word
// before them, or on over the word and then the spaces after it, as
// Windows' own edit box goes. Text without spaces is one word.
inline std::size_t textFieldWordLeft(const std::wstring& text, std::size_t index) {
    index = std::min(index, text.size());
    while (index > 0 && std::iswspace(text[index - 1])) --index;
    while (index > 0 && !std::iswspace(text[index - 1])) --index;
    return index;
}

inline std::size_t textFieldWordRight(const std::wstring& text, std::size_t index) {
    index = std::min(index, text.size());
    while (index < text.size() && !std::iswspace(text[index])) ++index;
    while (index < text.size() && std::iswspace(text[index])) ++index;
    return index;
}

inline bool textFieldHasSelection(const TextFieldState& field) { return field.caret != field.anchor; }
inline std::size_t textFieldSelectionStart(const TextFieldState& field) { return std::min(field.caret, field.anchor); }
inline std::size_t textFieldSelectionEnd(const TextFieldState& field) { return std::max(field.caret, field.anchor); }

inline std::wstring textFieldSelected(const TextFieldState& field) {
    const std::size_t start = std::min(textFieldSelectionStart(field), field.text.size());
    const std::size_t end = std::min(textFieldSelectionEnd(field), field.text.size());
    return field.text.substr(start, end - start);
}

// Both ends back inside the text, as after the text was set from outside.
inline void textFieldClamp(TextFieldState& field) {
    field.caret = textFieldBoundary(field.text, field.caret);
    field.anchor = textFieldBoundary(field.text, field.anchor);
}

// The caret to `index`; with `extend` the selection's other end stays.
inline void textFieldMoveTo(TextFieldState& field, std::size_t index, bool extend) {
    field.caret = textFieldBoundary(field.text, index);
    if (!extend) field.anchor = field.caret;
}

// Left (-1) or Right (+1). Without Shift a selection collapses to the end
// the arrow points at first, as in Windows' edit box.
inline void textFieldStep(TextFieldState& field, int direction, bool word, bool extend) {
    textFieldClamp(field);
    if (!extend && !word && textFieldHasSelection(field)) {
        textFieldMoveTo(field, direction < 0 ? textFieldSelectionStart(field) : textFieldSelectionEnd(field), false);
        return;
    }
    const std::size_t target = direction < 0
        ? (word ? textFieldWordLeft(field.text, field.caret) : textFieldPrevious(field.text, field.caret))
        : (word ? textFieldWordRight(field.text, field.caret) : textFieldNext(field.text, field.caret));
    textFieldMoveTo(field, target, extend);
}

inline void textFieldSelectAll(TextFieldState& field) {
    field.anchor = 0;
    field.caret = field.text.size();
}

// How many units `limit` leaves for what takes the selection's place, both
// ends within the text.
inline std::size_t textFieldRoom(const TextFieldState& field, std::size_t limit) {
    const std::size_t start = std::min(textFieldSelectionStart(field), field.text.size());
    const std::size_t end = std::min(textFieldSelectionEnd(field), field.text.size());
    const std::size_t kept = field.text.size() - (end - start);
    return limit > kept ? limit - kept : 0;
}

// What of `value` goes into a box with `room` units left: line breaks and
// tabs one space each, other control characters gone, and what would pass
// `room` left off, never half a surrogate pair.
inline std::wstring textFieldKept(std::wstring_view value, std::size_t room) {
    std::wstring clean;
    clean.reserve(std::min(value.size(), room));
    for (std::size_t i = 0; i < value.size(); ++i) {
        const wchar_t c = value[i];
        if (c == L'\r' || c == L'\n' || c == L'\t') {
            // A pasted CR LF is one break.
            if (c == L'\n' && i > 0 && value[i - 1] == L'\r') continue;
            clean += L' ';
            continue;
        }
        if (c < 0x20 || c == 0x7F) continue;
        clean += c;
    }
    if (clean.size() > room) {
        clean.resize(room);
        if (!clean.empty() && textFieldHighSurrogate(clean.back())) clean.pop_back();
    }
    return clean;
}

// `value` in place of the selection, or at the caret, as textFieldKept
// takes it under `limit`. True when the text changed. An empty `value`
// takes the selection out (Cut, Delete); one of which nothing is kept --
// an emoji with one unit of room -- changes nothing, the selection
// staying, where until 2026-11-07 it was taken out for nothing.
inline bool textFieldInsert(TextFieldState& field, std::wstring_view value, std::size_t limit = kTextFieldLimit) {
    textFieldClamp(field);
    const std::wstring clean = textFieldKept(value, textFieldRoom(field, limit));
    const std::size_t start = textFieldSelectionStart(field);
    const std::size_t end = textFieldSelectionEnd(field);
    if (clean.empty() && (start == end || !value.empty())) return false;
    field.text.replace(start, end - start, clean);
    field.caret = field.anchor = start + clean.size();
    return true;
}

// What a paste puts in: the copied text without the line breaks at its
// ends, which a copied line or a triple-clicked paragraph brings along.
// Typed as a space each, as breaks within the text still are, they were a
// password's last character, and the sign-in with it failed.
inline std::wstring textFieldPasted(std::wstring_view text) {
    const auto lineBreak = [](wchar_t c) { return c == L'\r' || c == L'\n'; };
    while (!text.empty() && lineBreak(text.front())) text.remove_prefix(1);
    while (!text.empty() && lineBreak(text.back())) text.remove_suffix(1);
    return std::wstring(text);
}

// The edits of a box's right-click menu, and of their keys (Ctrl+X, Ctrl+C,
// Ctrl+V, Delete, Ctrl+A), as Windows' edit box has them.
enum class TextFieldEdit { Cut, Copy, Paste, Delete, SelectAll };

// Whether `edit` would do anything now, which the menu greys the line of
// when not: Cut and Copy want a selection and not a password's, Delete a
// selection, and Paste something of `pasted` -- the clipboard's text as the
// box takes it (textFieldPasted, or a spin box's digits) -- that goes in
// under `limit` (textFieldKept): not with nothing it keeps, nor in a full
// box with nothing selected, nor where the room left splits the pair of a
// character such as an emoji. A paste of nothing does nothing; until
// 2026-11-07 it erased the selection. Windows' edit box leaves Paste lit in
// a full box, where it does nothing; here it is greyed. Select all wants an
// end to move: the whole text selected backwards, as Shift+Home leaves it,
// still brings the caret to the end, as Ctrl+A always did; selected
// forwards it is greyed.
inline bool textFieldCanEdit(const TextFieldState& field, TextFieldEdit edit, bool masked, std::wstring_view pasted,
                             std::size_t limit = kTextFieldLimit) {
    switch (edit) {
    case TextFieldEdit::Cut:
    case TextFieldEdit::Copy:
        return !masked && textFieldHasSelection(field);
    case TextFieldEdit::Paste:
        return !textFieldKept(pasted, textFieldRoom(field, limit)).empty();
    case TextFieldEdit::Delete:
        return textFieldHasSelection(field);
    case TextFieldEdit::SelectAll:
        // No text has its caret at 0, its end: nothing to select.
        return field.anchor != 0 || field.caret != field.text.size();
    }
    return false;
}

// Does `edit` where it can (textFieldCanEdit); what it copies is left in
// `copied`, for the clipboard, and is empty otherwise. True when the text
// changed. Cut and Delete only run with a selection, which an empty insert
// takes out.
inline bool textFieldApplyEdit(TextFieldState& field, TextFieldEdit edit, bool masked, std::wstring_view pasted,
                               std::wstring& copied, std::size_t limit = kTextFieldLimit) {
    textFieldClamp(field);
    copied.clear();
    if (!textFieldCanEdit(field, edit, masked, pasted, limit)) return false;
    switch (edit) {
    case TextFieldEdit::Cut:
        copied = textFieldSelected(field);
        return textFieldInsert(field, {});
    case TextFieldEdit::Copy:
        copied = textFieldSelected(field);
        return false;
    case TextFieldEdit::Paste:
        return textFieldInsert(field, pasted, limit);
    case TextFieldEdit::Delete:
        return textFieldInsert(field, {});
    case TextFieldEdit::SelectAll:
        textFieldSelectAll(field);
        return false;
    }
    return false;
}

// Backspace (-1) or Delete (+1), a word with Ctrl; the selection alone
// when there is one. True when the text changed.
inline bool textFieldErase(TextFieldState& field, int direction, bool word) {
    textFieldClamp(field);
    if (textFieldHasSelection(field)) return textFieldInsert(field, {});
    const std::size_t from = direction < 0
        ? (word ? textFieldWordLeft(field.text, field.caret) : textFieldPrevious(field.text, field.caret))
        : field.caret;
    const std::size_t to = direction < 0
        ? field.caret
        : (word ? textFieldWordRight(field.text, field.caret) : textFieldNext(field.text, field.caret));
    if (from == to) return false;
    field.text.erase(from, to - from);
    field.caret = field.anchor = from;
    return true;
}

// The text without the spaces around it: what a search asks for.
inline std::wstring textFieldTrimmed(const std::wstring& text) {
    const auto notSpace = [](wchar_t c) { return !std::iswspace(c); };
    const auto first = std::find_if(text.begin(), text.end(), notSpace);
    const auto last = std::find_if(text.rbegin(), text.rend(), notSpace).base();
    return first < last ? std::wstring(first, last) : std::wstring();
}

// What the field shows: the text with an input method's unfinished
// `composition` at the caret, where its own caret `compositionCaret` sits.
inline std::wstring textFieldShown(const TextFieldState& field, const std::wstring& composition) {
    if (composition.empty()) return field.text;
    const std::size_t caret = std::min(field.caret, field.text.size());
    return field.text.substr(0, caret) + composition + field.text.substr(caret);
}

inline std::size_t textFieldShownCaret(const TextFieldState& field, const std::wstring& composition,
                                       std::size_t compositionCaret) {
    const std::size_t caret = std::min(field.caret, field.text.size());
    return composition.empty() ? caret : caret + std::min(compositionCaret, composition.size());
}

// The caret index nearest `x`, measured from the start of the text, where
// `width(n)` is how wide the first n code units are drawn.
template <typename Width>
std::size_t textFieldIndexAt(const std::wstring& text, float x, Width width) {
    if (!(x > 0.0F)) return 0;
    std::size_t before = 0;
    float beforeX = 0.0F;
    for (std::size_t index = textFieldNext(text, 0); index <= text.size() && index > before;
         index = textFieldNext(text, index)) {
        const float at = width(index);
        if (at >= x) return x - beforeX <= at - x ? before : index;
        before = index;
        beforeX = at;
    }
    return text.size();
}

// How far a line wider than its box is moved left so the caret shows:
// nothing while it fits, and never past the caret standing at the box's
// right edge or the text's end there.
inline float textFieldScroll(float caretX, float textWidth, float boxWidth, float caretWidth) {
    if (!(textWidth + caretWidth > boxWidth)) return 0.0F;
    return std::clamp(caretX + caretWidth - boxWidth, 0.0F, textWidth + caretWidth - boxWidth);
}

}  // namespace quaddeck
