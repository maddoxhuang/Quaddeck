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

// `value` in place of the selection, or at the caret: line breaks and tabs
// become one space each, other control characters go, and what would pass
// `limit` is left off. True when the text changed.
inline bool textFieldInsert(TextFieldState& field, std::wstring_view value, std::size_t limit = kTextFieldLimit) {
    textFieldClamp(field);
    std::wstring clean;
    clean.reserve(value.size());
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
    const std::size_t start = textFieldSelectionStart(field);
    const std::size_t end = textFieldSelectionEnd(field);
    const std::size_t kept = field.text.size() - (end - start);
    const std::size_t room = limit > kept ? limit - kept : 0;
    if (clean.size() > room) {
        clean.resize(room);
        if (!clean.empty() && textFieldHighSurrogate(clean.back())) clean.pop_back();
    }
    if (clean.empty() && start == end) return false;
    field.text.replace(start, end - start, clean);
    field.caret = field.anchor = start + clean.size();
    return true;
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
