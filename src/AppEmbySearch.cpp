// App: the Emby browser's Search page and its box. The page is a browser
// page like the others -- AppEmbyBrowser.cpp asks the libraries and lays
// the results out under their sections -- and the box is a text box like
// the sign-in's (AppTextFields.cpp). This file is what makes it a search:
// the page it opens, its clear button, where it searches -- the folder it
// was opened in or every library -- and the pause after which what was
// typed is searched for.

#include "App.hpp"
#include "AppInternal.hpp"
#include "TextEncoding.hpp"

#include <algorithm>

namespace quaddeck {

using namespace app_internal;

namespace {

// Typing that pauses this long is searched for; Enter does not wait. Each
// search asks every library on the one request queue a click also waits
// on, so not one per key.
constexpr unsigned long long kEmbySearchPauseMs = 450;

}  // namespace

void App::embyOpenSearch() {
    if (!emby_.session().signedIn()) return;
    // Ctrl+F while the browser asks which pane to replace drops the
    // question for the search.
    if (embyPendingPlay_) embyCancelReplacement();
    // On the Search page already, or on an information page over it, the
    // box takes the typing again with what it holds selected; from any
    // other page a new Search page opens, empty. Opened in a library, a
    // folder, a show or a season, it searches what is under that page
    // (emby::searchableUnder says which do hold what they list).
    if (embyPages_.size() > 1 && embyPages_.back().kind == EmbyPage::Kind::Search) {
        embyCloseDetails();
    } else {
        EmbyPage page;
        page.kind = EmbyPage::Kind::Search;
        const EmbyPage* from = embyPages_.size() > 1 ? &embyPages_.back() : nullptr;
        if (from &&
            (from->kind == EmbyPage::Kind::Library || from->kind == EmbyPage::Kind::Folder ||
             from->kind == EmbyPage::Kind::Series || from->kind == EmbyPage::Kind::Season) &&
            emby::searchableUnder(embyListPage(*from))) {
            page.scopeId = from->id;
            page.scopeTitle = from->title;
        }
        embyPushPage(std::move(page));
    }
    focusField(TextFieldId::EmbySearch, true);
}

bool App::embySearchShown() const {
    return settingsOpen_ && embyBrowserOpen_ && browserSource_ == BrowserSource::Emby && !embyDetails_ &&
           !embyPendingPlay_ && emby_.session().signedIn() && embyPages_.size() > 1 &&
           embyPages_.back().kind == EmbyPage::Kind::Search;
}

bool App::embySearchTyping() const { return fieldTyping() == TextFieldId::EmbySearch; }

void App::embyClearSearch() {
    if (!embySearchShown()) return;
    fieldCancelComposition();
    embyPages_.back().field = TextFieldState{};
    focusField(TextFieldId::EmbySearch, false);
    embySearchCommit();
}

PanelRow App::embySearchRow() const {
    PanelRow row;
    row.kind = PanelRowKind::TextField;
    row.id = SettingId::EmbySearch;
    const auto& page = embyPages_.back();
    row.field.placeholder = page.searchesScope() ? L"Search titles in " + utf8ToWideText(page.scopeTitle)
                                                 : L"Search titles in every library";
    row.field.magnifier = true;
    row.field.clearable = true;
    fieldDecorate(row, TextFieldId::EmbySearch);
    return row;
}

PanelRow App::embySearchScopeRow() const {
    const auto& page = embyPages_.back();
    PanelRow row;
    row.kind = PanelRowKind::Buttons;
    row.id = SettingId::EmbySearchScope;
    row.options = {L"In " + utf8ToWideText(page.scopeTitle), L"Every library"};
    row.buttonMinWidth = 80.0F;
    row.buttonsOn = {!page.everyLibrary, page.everyLibrary};
    return row;
}

void App::embyChooseSearchScope(int part) {
    if (embyPages_.size() <= 1 || part < 0 || part > 1) return;
    auto& page = embyPages_.back();
    if (page.kind != EmbyPage::Kind::Search || page.scopeId.empty() || page.everyLibrary == (part == 1)) return;
    page.everyLibrary = part == 1;
    // What the box holds is searched for there at once, its pause or not.
    embySearchEditedTick_ = 0;
    page.term = wideToUtf8Text(textFieldTrimmed(page.field.text));
    page.title = page.term;
    embyRequestPage();
}

void App::embySearchEdited() {
    embySearchEditedTick_ = std::max<unsigned long long>(1, GetTickCount64());
    // Emptied, the results go at once rather than after the pause.
    if (textFieldTrimmed(embyPages_.back().field.text).empty()) embySearchCommit();
}

void App::embySearchCommit() {
    embySearchEditedTick_ = 0;
    if (embyPages_.size() <= 1 || embyPages_.back().kind != EmbyPage::Kind::Search) return;
    auto& page = embyPages_.back();
    const std::string term = wideToUtf8Text(textFieldTrimmed(page.field.text));
    if (term == page.term) return;
    page.term = term;
    page.title = term;
    embyRequestPage();
}

void App::embySearchWhenDue(unsigned long long now) {
    // Not while something is still being composed: it is not typed yet.
    if (embySearchEditedTick_ == 0 || (embySearchTyping() && !fieldComposition_.empty())) return;
    if (now < embySearchEditedTick_ + kEmbySearchPauseMs) return;
    embySearchCommit();
}

}  // namespace quaddeck
