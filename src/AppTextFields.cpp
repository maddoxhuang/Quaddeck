// App: the one-line text boxes typed into where the sheet draws them -- the
// Emby browser's search and the Emby sign-in's server, user and password.
// One at most has the typing (typingField_). This file is what a box does
// while it has it: the keys it takes, an input method's composition and
// where its windows go, the clipboard, its right-click menu and the caret.
// What a box is for is its page's: a search run after a pause
// (AppEmbySearch.cpp), a sign-in sent on Enter (AppEmby.cpp).

#include "App.hpp"
#include "AppInternal.hpp"

#include <imm.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <string_view>
#include <utility>

namespace quaddeck {

using namespace app_internal;

namespace app_internal {

std::wstring clipboardText(HWND owner) {
    std::wstring text;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(owner)) return text;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* chars = static_cast<const wchar_t*>(GlobalLock(data))) {
            // No further than a box can hold, whatever was copied.
            const std::size_t room = GlobalSize(data) / sizeof(wchar_t);
            text.assign(chars, wcsnlen(chars, std::min(room, kTextFieldLimit * 4)));
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return text;
}

void setClipboardText(HWND owner, const std::wstring& text) {
    if (text.empty() || !OpenClipboard(owner)) return;
    EmptyClipboard();
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* target = GlobalLock(memory)) {
            std::memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            // The clipboard owns it once taken, and not otherwise.
            if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        } else {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

}  // namespace app_internal

namespace {

bool signInField(TextFieldId id) {
    return id == TextFieldId::EmbyServer || id == TextFieldId::EmbyUser || id == TextFieldId::EmbyPassword;
}

// A password is shown as dots, and nothing of it leaves the box.
bool maskedField(TextFieldId id) { return id == TextFieldId::EmbyPassword; }

}  // namespace

TextFieldState* App::fieldState(TextFieldId id) {
    switch (id) {
    case TextFieldId::EmbySearch:
        return embyPages_.size() > 1 && embyPages_.back().kind == EmbyPage::Kind::Search
            ? &embyPages_.back().field : nullptr;
    case TextFieldId::EmbyServer: return &embySignInForm_.server;
    case TextFieldId::EmbyUser: return &embySignInForm_.user;
    case TextFieldId::EmbyPassword: return &embySignInForm_.password;
    case TextFieldId::VideoSetName: return &videoSetNameField_;
    case TextFieldId::None: break;
    }
    return nullptr;
}

const TextFieldState* App::fieldState(TextFieldId id) const { return const_cast<App*>(this)->fieldState(id); }

bool App::fieldShown(TextFieldId id) const {
    if (id == TextFieldId::EmbySearch) return embySearchShown();
    // A sign-in on its way holds the boxes as they were sent.
    if (signInField(id)) return embySignInShown() && !embyLoading_;
    if (id == TextFieldId::VideoSetName) {
        return settingsOpen_ && videoSetsOpen_ && !embyBrowserOpen_ && videoSetRenaming_ >= 0 &&
               static_cast<std::size_t>(videoSetRenaming_) < videoSets_.sets.size();
    }
    return false;
}

TextFieldId App::fieldTyping() const { return fieldShown(typingField_) ? typingField_ : TextFieldId::None; }

TextFieldId App::fieldOfRow(const PanelRow& row) {
    if (row.kind != PanelRowKind::TextField) return TextFieldId::None;
    if (row.id == SettingId::EmbySearch) return TextFieldId::EmbySearch;
    if (row.id == SettingId::VideoSetName) return TextFieldId::VideoSetName;
    if (row.id != SettingId::EmbySignInField) return TextFieldId::None;
    switch (row.param) {
    case 0: return TextFieldId::EmbyServer;
    case 1: return TextFieldId::EmbyUser;
    case 2: return TextFieldId::EmbyPassword;
    default: return TextFieldId::None;
    }
}

TextFieldId App::fieldAt(const PanelHit& hit) const {
    if (hit.kind != PanelHitKind::Field && hit.kind != PanelHitKind::Trailing) return TextFieldId::None;
    if (hit.row < 0 || hit.row >= static_cast<int>(panelRows_.size())) return TextFieldId::None;
    return fieldOfRow(panelRows_[static_cast<std::size_t>(hit.row)]);
}

void App::focusField(TextFieldId id, bool selectAll) {
    if (!fieldShown(id)) return;
    if (typingField_ != id) blurField();
    TextFieldState& field = *fieldState(id);
    textFieldClamp(field);
    if (selectAll) textFieldSelectAll(field);
    typingField_ = id;
    fieldCaretTick_ = GetTickCount64();
    fieldSyncIme();
    fieldPlaceIme();
}

void App::focusFieldAt(TextFieldId id, float x) {
    if (!fieldShown(id)) return;
    focusField(id, false);
    // A press while something is still being composed leaves the caret
    // where the composition stands.
    if (!fieldComposition_.empty()) return;
    const PanelRow* row = nullptr;
    const PanelRowGeometry* geometry = fieldGeometry(id, &row);
    if (!geometry) return;
    TextFieldState& field = *fieldState(id);
    // Measured as the painter draws it: the same box, the same text (dots
    // for a password), the same scroll.
    const OverlayRect box = panelTextFieldTextBox(*row, *geometry, uiScale_);
    const std::wstring shown = maskedField(id) ? std::wstring(field.text.size(), kPanelPasswordDot) : field.text;
    const auto widthOf = [&](std::size_t count) {
        return count == 0 ? 0.0F : overlay_.measureText(shown.substr(0, std::min(count, shown.size())),
                                                       OverlayTextStyle::Body, uiScale_);
    };
    const float scroll = textFieldScroll(widthOf(field.caret), widthOf(shown.size()), box.width,
                                         panelTextFieldCaretWidth(uiScale_));
    textFieldMoveTo(field, textFieldIndexAt(field.text, x - box.x + scroll, widthOf), false);
    fieldCaretTick_ = GetTickCount64();
    fieldPlaceIme();
}

void App::blurField() {
    const TextFieldId id = typingField_;
    if (id == TextFieldId::None) return;
    typingField_ = TextFieldId::None;
    // Unfinished composition goes with the typing; the input method is told
    // so, its answer coming here to a box that no longer takes it.
    fieldCancelComposition();
    if (fieldSystemCaret_) {
        DestroyCaret();
        fieldSystemCaret_ = false;
    }
    fieldSyncIme();
    // What was typed and not yet searched for is searched for now, so the
    // box and the results under it agree when the page is seen again.
    if (id == TextFieldId::EmbySearch && embySearchEditedTick_ != 0) embySearchCommit();
    // A set's new name is kept when the box is left, as by Enter.
    if (id == TextFieldId::VideoSetName) commitVideoSetRename();
}

void App::fieldCancelComposition() {
    if (!fieldComposition_.empty() && window_) {
        if (HIMC context = ImmGetContext(window_)) {
            ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
            ImmReleaseContext(window_, context);
        }
    }
    fieldComposition_.clear();
    fieldCompositionCaret_ = 0;
}

void App::fieldCompleteComposition() {
    if (fieldComposition_.empty()) return;
    // The input method's answer, the finished text, comes back through
    // WM_IME_COMPOSITION (fieldImeComposition) before ImmNotifyIME returns.
    if (window_ && keyboardFocus_) {
        if (HIMC context = ImmGetContext(window_)) {
            ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
            ImmReleaseContext(window_, context);
        }
    }
    if (fieldComposition_.empty()) return;
    // None came -- or there is no input method to ask: what the box shows
    // being composed goes in as typed, and the input method drops its own.
    const TextFieldId id = fieldTyping();
    const std::wstring composed = fieldComposition_;
    fieldCancelComposition();
    if (TextFieldState* field = fieldState(id); field && textFieldInsert(*field, composed)) fieldEdited(id);
}

void App::fieldDecorate(PanelRow& row, TextFieldId id) const {
    if (const TextFieldState* state = fieldState(id)) row.field.edit = *state;
    textFieldClamp(row.field.edit);
    row.field.masked = maskedField(id);
    if (fieldTyping() != id) return;
    row.field.focused = true;
    row.field.composition = fieldComposition_;
    row.field.compositionCaret = fieldCompositionCaret_;
    // Solid just after a key, then blinking at the system's rate, and only
    // while the window has the keyboard.
    const UINT blink = GetCaretBlinkTime();
    const unsigned long long since = GetTickCount64() - fieldCaretTick_;
    row.field.caretShown = keyboardFocus_ && (blink == 0 || blink == INFINITE || (since / blink) % 2 == 0);
}

void App::fieldKey(WPARAM key, bool ctrl, bool shift) {
    const TextFieldId id = fieldTyping();
    if (id == TextFieldId::None) return;
    TextFieldState& field = *fieldState(id);
    textFieldClamp(field);
    // A password's dots say nothing of its words, so Ctrl goes to its ends
    // there, as in Windows' own password box, and nothing of it is copied.
    const bool masked = maskedField(id);
    const bool word = ctrl && !masked;
    const auto toEnd = [&](int direction, bool extend) {
        textFieldMoveTo(field, direction < 0 ? 0 : field.text.size(), extend);
    };
    bool edited = false;
    switch (key) {
    case VK_LEFT:
        if (masked && ctrl) toEnd(-1, shift);
        else textFieldStep(field, -1, word, shift);
        break;
    case VK_RIGHT:
        if (masked && ctrl) toEnd(1, shift);
        else textFieldStep(field, 1, word, shift);
        break;
    case VK_HOME: toEnd(-1, shift); break;
    case VK_END: toEnd(1, shift); break;
    case VK_BACK:
        if (masked && ctrl && !textFieldHasSelection(field)) toEnd(-1, true);
        edited = textFieldErase(field, -1, word);
        break;
    case VK_DELETE:
        // Shift+Delete cuts, as in Windows' edit box; a password's
        // selection is only erased, below.
        if (shift && !ctrl && !masked && textFieldHasSelection(field)) {
            edited = fieldApply(id, TextFieldEdit::Cut);
            break;
        }
        if (masked && ctrl && !textFieldHasSelection(field)) toEnd(1, true);
        edited = textFieldErase(field, 1, word);
        break;
    case VK_INSERT:
        if (ctrl && !shift) fieldApply(id, TextFieldEdit::Copy);
        else if (shift && !ctrl) edited = fieldApply(id, TextFieldEdit::Paste);
        break;
    case VK_RETURN:
        if (id == TextFieldId::EmbySearch) {
            embySearchCommit();
        } else if (id == TextFieldId::VideoSetName) {
            commitVideoSetRename();
            return;
        } else {
            // Any of the sign-in's boxes sends the form, as its button does.
            embySubmitSignIn();
            return;
        }
        break;
    case VK_TAB:
        if (signInField(id)) {
            // Server, user, password and round again; Shift goes back.
            static constexpr TextFieldId order[] = {
                TextFieldId::EmbyServer, TextFieldId::EmbyUser, TextFieldId::EmbyPassword};
            const int at = id == TextFieldId::EmbyServer ? 0 : id == TextFieldId::EmbyUser ? 1 : 2;
            focusField(order[(at + (shift ? 2 : 1)) % 3], true);
            return;
        }
        break;
    case VK_ESCAPE:
        if (id == TextFieldId::VideoSetName) cancelVideoSetRename();
        else blurField();
        return;
    case 'A':
        if (ctrl) fieldApply(id, TextFieldEdit::SelectAll);
        break;
    case 'F':
        // Ctrl+F, which brought the search box, selects what it holds.
        if (ctrl && id == TextFieldId::EmbySearch) textFieldSelectAll(field);
        break;
    case 'C':
        if (ctrl) fieldApply(id, TextFieldEdit::Copy);
        break;
    case 'X':
        if (ctrl) edited = fieldApply(id, TextFieldEdit::Cut);
        break;
    case 'V':
        if (ctrl) edited = fieldApply(id, TextFieldEdit::Paste);
        break;
    default:
        break;
    }
    if (edited) fieldEdited(id);
    fieldCaretTick_ = GetTickCount64();
    fieldPlaceIme();
}

std::wstring App::fieldClipboard(HWND owner) const {
    return clipboardText(owner ? owner : window_);
}

void App::fieldSetClipboard(const std::wstring& text, HWND owner) {
    if (text.empty()) return;
    setClipboardText(owner ? owner : window_, text);
}

bool App::fieldApply(TextFieldId id, TextFieldEdit edit) {
    TextFieldState* state = fieldState(id);
    if (!state) return false;
    // The clipboard is read for a paste only.
    const std::wstring pasted = edit == TextFieldEdit::Paste ? textFieldPasted(fieldClipboard()) : std::wstring();
    std::wstring copied;
    const bool edited = textFieldApplyEdit(*state, edit, maskedField(id), pasted, copied);
    fieldSetClipboard(copied);
    return edited;
}

void App::showFieldMenu(TextFieldId id, POINT local) {
    if (!fieldShown(id)) return;
    // Pressed with the right button, a box takes the typing as with the
    // left, but its caret and selection stay: they are what the menu acts on.
    if (fieldTyping() != id) focusField(id, false);
    // What the input method was composing goes into the box first, finished,
    // rather than being lost with the press (a left press keeps it too); the
    // menu then has the keys, the input method off meanwhile.
    fieldCompleteComposition();
    PanelMenu menu = fieldEditMenu(*fieldState(id), maskedField(id), textFieldPasted(fieldClipboard()));
    openPanelMenuAt(std::move(menu), local, [this, id](unsigned command) {
        // The box went while the menu was up: its page, the sheet, or a
        // sign-in sent.
        if (fieldTyping() != id) return;
        const auto edit = fieldMenuEdit(command);
        if (!edit) return;
        if (fieldApply(id, *edit)) fieldEdited(id);
        fieldCaretTick_ = GetTickCount64();
        fieldPlaceIme();
    }, true);
}

void App::fieldCharacter(wchar_t character) {
    // A character past the first 65536, such as an emoji, comes as two, the
    // halves of its surrogate pair: the first waits for the second, and the
    // two go in together or not at all (textFieldInsert). Until 2026-11-07
    // each went in alone, and at the box's limit the first was left in it
    // without the second, in place of a selected character too.
    const wchar_t high = std::exchange(fieldHighSurrogate_, L'\0');
    const TextFieldId id = fieldTyping();
    if (id == TextFieldId::None) return;
    // Backspace, Tab, Enter, Escape and Ctrl's letters come as characters
    // too, after the keys the box has already acted on.
    if (character < 0x20 || character == 0x7F) return;
    if (textFieldHighSurrogate(character)) {
        fieldHighSurrogate_ = character;
        return;
    }
    const wchar_t pair[2]{high, character};
    std::wstring_view typed(&character, 1);
    if (textFieldLowSurrogate(character)) {
        if (high == L'\0') return;  // half a pair, its first lost
        typed = std::wstring_view(pair, 2);
    }
    if (textFieldInsert(*fieldState(id), typed)) fieldEdited(id);
    fieldCaretTick_ = GetTickCount64();
    fieldPlaceIme();
}

void App::fieldEdited(TextFieldId id) {
    if (id == TextFieldId::EmbySearch) embySearchEdited();
}

void App::fieldsWhenDue(unsigned long long now) {
    // The box went with its page, the sheet or the browser, or a sign-in
    // is on its way.
    if (typingField_ != TextFieldId::None && !fieldShown(typingField_)) blurField();
    embySearchWhenDue(now);
}

const PanelRowGeometry* App::fieldGeometry(TextFieldId id, const PanelRow** row) const {
    if (!fieldShown(id)) return nullptr;
    // The boxes are among the rows above any list.
    const std::size_t count = std::min(panelRows_.size(), panelLayout_.rows.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto kind = panelRows_[i].kind;
        if (kind == PanelRowKind::Item || kind == PanelRowKind::Tiles) break;
        if (fieldOfRow(panelRows_[i]) != id) continue;
        if (!panelLayout_.rows[i].control.visible()) return nullptr;
        if (row) *row = &panelRows_[i];
        return &panelLayout_.rows[i];
    }
    return nullptr;
}

std::optional<OverlayRect> App::fieldCaretBox() {
    const TextFieldId id = fieldTyping();
    const PanelRow* row = nullptr;
    const PanelRowGeometry* geometry = fieldGeometry(id, &row);
    if (!geometry) return std::nullopt;
    const TextFieldState& field = *fieldState(id);
    const OverlayRect box = panelTextFieldTextBox(*row, *geometry, uiScale_);
    PanelTextField drawn;
    drawn.edit = field;
    drawn.composition = fieldComposition_;
    drawn.masked = maskedField(id);
    const std::wstring shown = panelTextFieldShown(drawn);
    const auto widthOf = [&](std::size_t count) {
        return count == 0 ? 0.0F : overlay_.measureText(shown.substr(0, std::min(count, shown.size())),
                                                       OverlayTextStyle::Body, uiScale_);
    };
    const float caretWidth = panelTextFieldCaretWidth(uiScale_);
    const float scroll = textFieldScroll(
        widthOf(textFieldShownCaret(field, fieldComposition_, fieldCompositionCaret_)),
        widthOf(shown.size()), box.width, caretWidth);
    // Where the composition starts: an input method lists its candidates
    // under the text being composed.
    const float height = 20.0F * uiScale_;
    const float x = std::clamp(box.x + widthOf(std::min(field.caret, field.text.size())) - scroll, box.x, box.right());
    return OverlayRect{x, box.y + (box.height - height) * 0.5F, caretWidth, height};
}

void App::fieldPlaceIme() {
    if (!window_ || !keyboardFocus_ || fieldTyping() == TextFieldId::None) return;
    const auto caret = fieldCaretBox();
    if (!caret) return;
    POINT top{static_cast<LONG>(std::lround(caret->x)), static_cast<LONG>(std::lround(caret->y))};
    if (videoWindow_ && videoWindow_ != window_) MapWindowPoints(videoWindow_, window_, &top, 1);
    const LONG height = std::max(1L, static_cast<LONG>(std::lround(caret->height)));
    // A hidden system caret where the drawn one is: some input methods
    // place their windows by it rather than by what they are told below.
    if (!fieldSystemCaret_) fieldSystemCaret_ = CreateCaret(window_, nullptr, 1, height) != FALSE;
    if (fieldSystemCaret_) SetCaretPos(top.x, top.y);
    HIMC context = ImmGetContext(window_);
    if (!context) return;
    LOGFONTW font{};
    font.lfHeight = -std::max(1L, static_cast<LONG>(std::lround(14.0F * uiScale_)));
    font.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(font.lfFaceName, L"Segoe UI");
    ImmSetCompositionFontW(context, &font);
    COMPOSITIONFORM composition{};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = top;
    ImmSetCompositionWindow(context, &composition);
    CANDIDATEFORM candidate{};
    candidate.dwIndex = 0;
    candidate.dwStyle = CFS_CANDIDATEPOS;
    candidate.ptCurrentPos = {top.x, top.y + height + static_cast<LONG>(std::lround(6.0F * uiScale_))};
    ImmSetCandidateWindow(context, &candidate);
    ImmReleaseContext(window_, context);
}

void App::fieldSyncIme() {
    // The input method is on only while a box that composes has the typing.
    // Anywhere else a letter is a shortcut, and an input method in its
    // native mode would take it for a composition, the key coming as
    // VK_PROCESSKEY. A password is typed key by key, never composed, as in
    // Windows' own password box.
    // Off too while a drawn menu has the keys, as over a box's own menu.
    // Turned off only with the keyboard here; on again whenever it was off.
    const TextFieldId typing = fieldTyping();
    const bool off = typing == TextFieldId::None || maskedField(typing) || panelMenu_.has_value();
    if (!window_ || off == fieldImeOff_ || (off && !keyboardFocus_)) return;
    ImmAssociateContextEx(window_, nullptr, off ? 0 : IACE_DEFAULT);
    fieldImeOff_ = off;
}

void App::fieldImeStart() {
    // What is selected gives way to what is being composed.
    const TextFieldId id = fieldTyping();
    TextFieldState& field = *fieldState(id);
    if (textFieldHasSelection(field) && textFieldErase(field, -1, false)) fieldEdited(id);
    fieldComposition_.clear();
    fieldCompositionCaret_ = 0;
    fieldPlaceIme();
}

void App::fieldImeComposition(LPARAM flags) {
    HIMC context = window_ ? ImmGetContext(window_) : nullptr;
    if (!context) return;
    const auto read = [context](DWORD kind) {
        std::wstring value;
        const LONG bytes = ImmGetCompositionStringW(context, kind, nullptr, 0);
        if (bytes > 0) {
            value.resize(static_cast<std::size_t>(bytes) / sizeof(wchar_t));
            ImmGetCompositionStringW(context, kind, value.data(), static_cast<DWORD>(value.size() * sizeof(wchar_t)));
        }
        return value;
    };
    const TextFieldId id = fieldTyping();
    TextFieldState& field = *fieldState(id);
    // A finished part first: some input methods hand one over with the
    // start of the next composition in the same message.
    if (flags & GCS_RESULTSTR) {
        fieldComposition_.clear();
        fieldCompositionCaret_ = 0;
        if (textFieldInsert(field, read(GCS_RESULTSTR))) fieldEdited(id);
    }
    if (flags & GCS_COMPSTR) {
        fieldComposition_ = read(GCS_COMPSTR);
        const LONG cursor = (flags & GCS_CURSORPOS) ? ImmGetCompositionStringW(context, GCS_CURSORPOS, nullptr, 0)
                                                    : static_cast<LONG>(fieldComposition_.size());
        fieldCompositionCaret_ = std::min(static_cast<std::size_t>(std::max(0L, cursor)), fieldComposition_.size());
    } else if (!(flags & GCS_RESULTSTR)) {
        // Neither: the composition was given up.
        fieldComposition_.clear();
        fieldCompositionCaret_ = 0;
    }
    ImmReleaseContext(window_, context);
    fieldCaretTick_ = GetTickCount64();
    fieldPlaceIme();
}

void App::fieldImeEnd() {
    fieldComposition_.clear();
    fieldCompositionCaret_ = 0;
}

void App::fieldLostKeyboard() {
    keyboardFocus_ = false;
    fieldComposition_.clear();
    fieldCompositionCaret_ = 0;
    // A system caret belongs to the window with the keyboard.
    if (fieldSystemCaret_) {
        DestroyCaret();
        fieldSystemCaret_ = false;
    }
}

}  // namespace quaddeck
