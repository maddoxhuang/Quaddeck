// App: the Emby library browser drawn in the sheet. Its rows and
// tiles, pages, views, sort and filters, search, pictures, playlists, and
// what activating an item does. Replies arrive on the window thread
// through EmbyClient::pump(), as described in AppEmby.cpp.

#include "App.hpp"
#include "AppInternal.hpp"
#include "Diagnostics.hpp"
#include "FilePersistence.hpp"
#include "TextEncoding.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <locale>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>

namespace quaddeck {

using namespace app_internal;

namespace {

// A list's first page, there quickly to be read.
constexpr int kEmbyPageSize = 300;
// A list is fetched whole without a scroll, this many entries a request
// after its first page -- a playlist's first page too -- because it is a
// list to play through; one longer than the limit stops there.
constexpr int kEmbyWholePageSize = 1000;
constexpr int kEmbyWholeLimit = 20000;
constexpr int kEmbyEpisodeAutoLimit = 20000;
constexpr int kEmbySearchPerLibrary = 200;
// Tiles at 96 DPI and the widths their pictures are asked for: twice the
// tile, so a 200 % display still gets a sharp picture from one fetch.
constexpr float kEmbyPosterTile = 150.0F;
constexpr float kEmbyThumbTile = 220.0F;
// A person's whole filmography is fetched, as a playlist is, so its
// sections are whole before they are read; one longer stops here.
constexpr int kEmbyPersonLimit = 2000;
// Pictures queued at once; scrolling past a line does not queue its
// pictures for good.
constexpr std::size_t kEmbyImageQueue = 24;
// Subtitle questions out at once on the client clicks and reports use.
constexpr std::size_t kSubtitleQuestionsAtOnce = 2;
// Videos one shape question names: more than a screenful of tiles.
constexpr std::size_t kEmbyShapeBatch = 48;
constexpr const char* kEmbyListFields = emby::kBrowserItemFields;

std::wstring wide(const std::string& text) { return utf8ToWideText(text); }

std::wstring searchGroupTitle(emby::SearchGroup group) {
    switch (group) {
    case emby::SearchGroup::Movies: return L"Movies";
    case emby::SearchGroup::Series: return L"Series";
    case emby::SearchGroup::Episodes: return L"Episodes";
    case emby::SearchGroup::Videos: return L"Videos";
    case emby::SearchGroup::Photos: return L"Photos";
    case emby::SearchGroup::Other: break;
    }
    return L"Other";
}

std::wstring itemLabel(const emby::Item& entry) {
    // The same list can hold episodes from several shows or seasons (a
    // search, Continue watching, a playlist). Keep their identity visible.
    if (entry.type == "Episode") {
        // A long series name can fill a narrow dock's single-line label.
        // Put the episode code first so adjacent episodes stay distinct.
        std::string label = emby::episodeCode(entry);
        const auto append = [&label](const std::string& part) {
            if (part.empty()) return;
            if (!label.empty()) label += " \xC2\xB7 ";
            label += part;
        };
        if (label.empty()) {
            // Without a number, the episode's own name is its identifier.
            append(entry.name);
            append(entry.seriesName);
        } else {
            append(entry.seriesName);
            append(entry.name);
        }
        return wide(label.empty() ? "Episode" : label);
    }
    return wide(emby::displayTitle(entry));
}

std::wstring runtimeLabel(const emby::Item& entry) {
    return entry.runTimeTicks > 0 ? formatTime(emby::secondsFromTicks(entry.runTimeTicks)) : L"";
}

// How far a video was watched, 0 for not started or finished.
float progressFraction(const emby::Item& entry) {
    if (entry.played || entry.positionTicks <= 0 || entry.runTimeTicks <= 0) return 0.0F;
    return std::clamp(static_cast<float>(static_cast<double>(entry.positionTicks) /
                                         static_cast<double>(entry.runTimeTicks)), 0.0F, 1.0F);
}

std::wstring entryCount(const emby::Item& entry) {
    if (entry.childCount < 0) return {};
    return std::to_wstring(entry.childCount) + (entry.childCount == 1 ? L" item" : L" items");
}

// Watched state and length side by side: "▶ 40%   23:40", never one
// instead of the other.
std::wstring itemValue(const emby::Item& entry) {
    if (entry.type == "CollectionFolder" || entry.type == "UserView") return wide(entry.collectionType);
    if (emby::isPlaylist(entry)) {
        // How much there is to play: "403 items   22:08:30".
        const std::wstring count = entryCount(entry);
        const std::wstring length = runtimeLabel(entry);
        if (count.empty() && length.empty()) return L"Playlist";
        if (count.empty()) return length;
        if (length.empty()) return count;
        return count + L"   " + length;
    }
    if (entry.type == "Series" || entry.type == "Season") {
        std::wstring value;
        if (entry.type == "Season" && entry.childCount >= 0) {
            value = std::to_wstring(entry.childCount) + (entry.childCount == 1 ? L" episode" : L" episodes");
        }
        if (entry.unplayedCount > 0) {
            if (!value.empty()) value += L"   ";
            value += std::to_wstring(entry.unplayedCount) + L" new";
        } else if (entry.played) {
            if (!value.empty()) value += L"   ";
            value += wide(emby::progressLabel(entry));
        }
        if (value.empty()) value = entry.type == "Season" ? L"Season" : L"Series";
        return value;
    }
    if (emby::isPhoto(entry)) return L"photo";
    if (emby::isPlayableVideo(entry)) {
        const std::wstring progress = wide(emby::progressLabel(entry));
        const std::wstring length = runtimeLabel(entry);
        if (progress.empty()) return length;
        if (length.empty()) return progress;
        return progress + L"   " + length;
    }
    if (emby::isBrowsable(entry)) return L"\x203A";
    return {};
}

// The tile's top-left pill: the watched state alone.
std::wstring tileMark(const emby::Item& entry) {
    if (emby::isPlayableVideo(entry) || ((entry.type == "Series" || entry.type == "Season") && entry.played)) {
        return wide(emby::progressLabel(entry));
    }
    return {};
}

// The tile's top-right pill: the length, a new count, or that it opens
// as a folder; the watched state has its own pill and bar.
std::wstring tileBadge(const emby::Item& entry) {
    if (entry.type == "CollectionFolder" || entry.type == "UserView") return {};
    if (emby::isPhoto(entry)) return L"photo";
    if (emby::isPlayableVideo(entry)) return runtimeLabel(entry);
    if (entry.type == "Series" || entry.type == "Season") {
        if (entry.unplayedCount > 0) return std::to_wstring(entry.unplayedCount) + L" new";
        return entry.type == "Season" ? L"Season" : L"Series";
    }
    if (emby::isPlaylist(entry)) {
        const std::wstring count = entryCount(entry);
        return count.empty() ? L"Playlist" : count;
    }
    if (emby::isBrowsable(entry)) return L"Folder";
    return {};
}

}  // namespace

// ---------------------------------------------------------------------------
// The browser

// The text, marks and pictures of an Item or Tiles row of the list, from the
// items its params name.
void App::fillEmbyRowContent(PanelRow& row, const ListRowMarks& marks) const {
    if (!row.contentPending) return;
    row.contentPending = false;
    row.enabled = marks.embyItemsEnabled;
    const auto entryAt = [&](int param) -> const emby::Item* {
        if (param >= 0) {
            const auto index = static_cast<std::size_t>(param);
            return index < embyItems_.size() ? &embyItems_[index] : nullptr;
        }
        const auto index = static_cast<std::size_t>(-(param + 1));
        return index < embyResume_.size() ? &embyResume_[index] : nullptr;
    };
    // The item being played, marked wherever it appears, in its video's
    // colour; so is every folder it is in, so a page above shows which way
    // it is.
    const auto playingPosition = [&](const emby::Item& entry) { return marks.embyPlace(entry.id); };
    if (row.kind == PanelRowKind::Item) {
        const emby::Item* entry = entryAt(row.param);
        if (!entry) return;
        row.playingPosition = playingPosition(*entry);
        row.current = row.playingPosition >= 0;
        row.addAvailable = emby::isPlayableVideo(*entry);
        row.label = itemLabel(*entry);
        row.value = itemValue(*entry);
        row.on = entry->positionTicks > 0 && !entry->played;
        if (embyHasSubtitles(*entry)) row.tag = L"Sub";
        row.favorite = entry->favorite && emby::listedAmongFavorites(*entry);
        return;
    }
    if (row.kind != PanelRowKind::Tiles) return;
    const int imageWidth = embyTileImageWidth(row.tileWidth, row.tileAspect < 1.0F, uiScale_);
    const std::size_t count = row.tileParams.size();
    row.options.assign(count, std::wstring());
    row.tileKeys.clear();
    row.tileBadges.clear();
    row.tileMarks.clear();
    row.tileProgress.clear();
    row.tileTags.clear();
    row.tileFavorites.clear();
    row.tileAddAvailable.clear();
    row.tilePlaying.clear();
    for (std::size_t part = 0; part < count; ++part) {
        const emby::Item* entry = entryAt(row.tileParams[part]);
        if (!entry) {
            row.tileKeys.emplace_back();
            row.tileBadges.emplace_back();
            row.tileMarks.emplace_back();
            row.tileProgress.push_back(0.0F);
            row.tileTags.emplace_back();
            row.tileFavorites.push_back(false);
            row.tileAddAvailable.push_back(false);
            row.tilePlaying.push_back(-1);
            continue;
        }
        row.tileAddAvailable.push_back(emby::isPlayableVideo(*entry));
        row.tilePlaying.push_back(playingPosition(*entry));
        if (row.tilePlaying.back() >= 0 && row.selected < 0) row.selected = static_cast<int>(part);
        row.options[part] = itemLabel(*entry);
        row.tileKeys.push_back(emby::imageKey(*entry, imageWidth));
        row.tileBadges.push_back(tileBadge(*entry));
        row.tileMarks.push_back(tileMark(*entry));
        row.tileProgress.push_back(progressFraction(*entry));
        row.tileTags.push_back(embyHasSubtitles(*entry) ? L"Sub" : L"");
        row.tileFavorites.push_back(entry->favorite && emby::listedAmongFavorites(*entry));
    }
}

std::vector<PanelRow> App::buildEmbyRows(float clientWidth, std::vector<PanelListRun>* runs) const {
    if (runs) runs->clear();
    if (embyPendingPlay_) return buildEmbyReplacementRows(clientWidth);
    if (embyDetails_) return buildEmbyDetailRows(clientWidth);
    std::vector<PanelRow> rows;
    const auto header = [&](std::wstring text) {
        PanelRow row;
        row.kind = PanelRowKind::Header;
        row.label = std::move(text);
        rows.push_back(std::move(row));
    };
    const auto note = [&](std::wstring text) {
        PanelRow row;
        row.kind = PanelRowKind::Note;
        row.label = std::move(text);
        row.wrapNote = true;
        rows.push_back(std::move(row));
    };
    const auto buttons = [&](SettingId id, std::vector<std::wstring> captions) {
        PanelRow row;
        row.kind = PanelRowKind::Buttons;
        row.id = id;
        row.options = std::move(captions);
        rows.push_back(std::move(row));
    };
    // A list as tiles: as many per line as the sheet is wide, every line
    // one row so the sheet scrolls by lines and a hit names one tile.
    const bool posters = embyShownView() == 1;
    const PanelMetrics metrics;
    const float innerWidth = std::max(0.0F, clientWidth - 2.0F * metrics.pad * uiScale_);
    // As many tiles as fit a line, then stretched or shrunk to fill it: a
    // docked sheet shows one wide thumbnail or two posters per line rather
    // than one nominal tile and a gap.
    const int perLine = panelTilesPerLine(innerWidth, posters ? kEmbyPosterTile : kEmbyThumbTile, uiScale_, metrics);
    const float tileWidth = panelTileWidthFor(innerWidth, perLine, uiScale_, metrics);
    // A list is not made row by row: a run says where its rows go and
    // which items they name, and fillEmbyRowContent gives a row its text,
    // marks and pictures (withListRuns, or once it is near the screen).
    std::vector<PanelListRun> listRuns;
    std::size_t listedRows = 0;
    const auto listing = [&](std::size_t entries, int firstParam, int paramStep) {
        if (entries == 0) return;
        PanelListRun run;
        run.at = rows.size() + listedRows;
        run.entries = entries;
        run.kind = embyBrowser_.view != 0 ? PanelRowKind::Tiles : PanelRowKind::Item;
        run.id = SettingId::EmbyItem;
        run.firstParam = firstParam;
        run.paramStep = paramStep;
        run.perLine = perLine;
        run.tileWidth = tileWidth;
        run.tileAspect = posters ? 2.0F / 3.0F : 16.0F / 9.0F;
        listedRows += run.rowCount();
        listRuns.push_back(run);
    };

    if (!emby_.session().signedIn()) {
        // The sign-in page: the sheet stays the browser and its boxes are
        // typed into where they are drawn. Under the button, what is
        // happening or what went wrong.
        header(L"Sign in");
        note(L"Browse an Emby server's libraries and play them in the panes.");
        rows.back().wrapNote = true;
        rows.push_back(embySignInRow(TextFieldId::EmbyServer));
        rows.push_back(embySignInRow(TextFieldId::EmbyUser));
        rows.push_back(embySignInRow(TextFieldId::EmbyPassword));
        buttons(SettingId::EmbyAccount, {embyLoading_ ? L"Signing in\x2026" : L"Sign in"});
        rows.back().primary = true;
        rows.back().enabled = !embyLoading_;
        if (!embyNote_.empty()) {
            note(embyNote_);
            rows.back().wrapNote = true;
            rows.back().alert = !embyLoading_;
        }
        return rows;
    }

    const bool home = embyPages_.size() <= 1;
    const bool searchPage = !home && embyPages_.back().kind == EmbyPage::Kind::Search;
    const bool searchEmpty = searchPage && embyPages_.back().term.empty();
    const bool personPage = !home && embyPages_.back().kind == EmbyPage::Kind::Person;
    std::wstring crumb = L"Emby";
    std::size_t first = 1;
    if (embyPages_.size() > 3) {
        crumb += L" \x203A \x2026";
        first = embyPages_.size() - 2;
    }
    for (std::size_t i = first; i < embyPages_.size(); ++i) {
        const auto& page = embyPages_[i];
        // The Search page's own box says what it searched for; below a
        // page opened from it, the crumb does.
        const bool top = i + 1 == embyPages_.size();
        crumb += L" \x203A " + (page.kind != EmbyPage::Kind::Search ? wide(page.title)
                                : top || page.term.empty() ? std::wstring(L"Search")
                                : L"\x201C" + wide(page.term) + L"\x201D");
    }
    header(crumb);
    // How many videos the page lists -- folders are not counted -- or, while
    // a long one has come only in part, how many entries it has in all.
    if (!home && !searchPage && !embyItems_.empty()) {
        const auto held = static_cast<int>(embyItems_.size());
        if (embyPageIsPaged() && embyTotal_ > held) {
            rows.back().value = std::to_wstring(embyTotal_) + L" items";
        } else {
            // A flag of each entry, not its type's name: this runs every
            // frame, over lists of thousands.
            rows.back().value = videoCountText(static_cast<std::size_t>(std::count_if(
                embyItems_.begin(), embyItems_.end(), [](const emby::Item& item) { return !item.isFolder; })));
        }
    }
    // The way back to the settings is the header's switch, so the home
    // page's row starts at Search; the other pages lead with Back.
    if (home) {
        buttons(SettingId::EmbyNav, {L"Search", L"Favorites", L"Sign out", L"Refresh"});
    } else {
        buttons(SettingId::EmbyNav, {L"\x2190 Back", L"Search", L"Favorites", L"Home", L"Refresh"});
    }
    rows.back().buttonMinWidth = kEmbyNavButtonWidth;
    if (searchPage) rows.push_back(embySearchRow());
    if (searchPage && !embyPages_.back().scopeId.empty()) rows.push_back(embySearchScopeRow());
    const std::string libraryId = embyCurrentLibraryId();
    const std::string libraryType = embyLibraryType(libraryId);
    const bool arrange = !home && embyPageAcceptsFilters() && !searchEmpty;
    const bool libraryDetails = libraryType == "movies" || libraryType == "tvshows";
    const bool ownOrder = !home && embyPages_.back().kind == EmbyPage::Kind::Playlist && embyPages_.back().ownOrder;
    const auto sortKey = emby::sortKeyFromIndex(embyBrowser_.sort);
    std::wstring sort = ownOrder ? L"Playlist" : emby::sortKeyName(sortKey);
    if (!ownOrder && sortKey != emby::SortKey::Random) sort += embyBrowser_.descending ? L" \u2193" : L" \u2191";
    const bool flatOffered = arrange && embyPageAcceptsFlat();
    // The favorites page lists nothing else.
    const bool favoritesOffered = arrange && embyPages_.back().kind != EmbyPage::Kind::Favorites;
    const std::wstring view = embyBrowser_.view == 0 ? L"List" : posters ? L"Posters" : L"Thumbs";
    // View and Sort open their drop-downs. Each filter is a button of its
    // own that switches it at once, lit while it is on: Unplayed, the
    // favorites under the page, All videos where the page lists folders, a
    // movie or TV library's information pages. One the page does not offer
    // leaves its slot empty, so a part always names the same button
    // (embyToolbarAction).
    buttons(SettingId::EmbyToolbar,
            {view + L" \x25BE", arrange ? sort + L" \x25BE" : L"", arrange ? L"Unplayed" : L"",
             favoritesOffered ? L"\x2665 Favorites" : L"", flatOffered ? L"All videos" : L"",
             libraryDetails ? L"Info pages" : L""});
    rows.back().buttonMinWidth = 80.0F;
    rows.back().buttonsOn = {false, false, arrange && embyBrowser_.unplayed,
                             favoritesOffered && embyBrowser_.favorites, flatOffered && embyBrowser_.flat,
                             libraryDetails && embyLibraryDetailsEnabled(embyLibraries_, emby_.session().serverId,
                                                                         libraryId, libraryType)};
    // Navigation and the compact arrangement stay available while the list scrolls.
    for (auto& row : rows) row.pinned = true;
    if (personPage) rows.push_back(embyPersonRow(embyPages_.back()));
    if (searchEmpty) {
        const auto& page = embyPages_.back();
        note(page.searchesScope()
                 ? L"Type part of a title: everything under " + wide(page.scopeTitle) + L" is searched as you type."
                 : L"Type part of a title: every library is searched as you type.");
    }
    else if (embyLoading_) note(searchPage ? L"Searching\x2026" : L"Loading\x2026");
    else if (!embyNote_.empty()) {
        note(embyNote_);
        rows.back().alert = embyNoteError_;
        if (embyNoteError_) buttons(SettingId::EmbyRetry, {L"Retry"});
    }
    if (home) {
        note(embyStatusLine());
        if (!embyResume_.empty()) {
            header(L"Continue watching");
            listing(embyResume_.size(), -1, -1);
        }
        header(L"Libraries");
    }
    if (searchPage || personPage) {
        // The results under their sections, as the server's own clients
        // show a search and a person's titles; embySearchArrived and
        // embyListParsed put each section's together.
        std::size_t start = 0;
        while (start < embyItems_.size()) {
            const auto group = emby::searchGroup(embyItems_[start]);
            std::size_t end = start + 1;
            while (end < embyItems_.size() && emby::searchGroup(embyItems_[end]) == group) ++end;
            header(searchGroupTitle(group) + L"  \x00B7  " + std::to_wstring(end - start));
            listing(end - start, static_cast<int>(start), 1);
            start = end;
        }
    } else {
        listing(embyItems_.size(), 0, 1);
    }
    // A search still reading leaves its items unpressable: listRowMarks.
    if (embyPageIsPaged() && embyTotal_ > static_cast<int>(embyItems_.size())) {
        const std::wstring count = L"  (" + std::to_wstring(embyItems_.size()) + L" of " +
                                   std::to_wstring(embyTotal_) + L")";
        // On its way, the button says so and takes no press: the same row
        // under the list, so nothing that is being read moves.
        if (embyLoadingMore_ || !embyLoading_) {
            buttons(SettingId::EmbyMore, {(embyLoadingMore_ ? L"Loading more\x2026" : L"Show more") + count});
            rows.back().enabled = !embyLoadingMore_;
        }
    }
    return withListRuns(std::move(rows), std::move(listRuns), runs);
}

void App::embyEdgeHover(POINT local, const RECT& client, bool inside) {
    constexpr ULONGLONG kCloseDelayMs = 500;
    if (!settingsOpen_ || !embyBrowserOpen_) embyBrowserByEdge_ = false;
    bool embyPlaying = false;
    for (const auto& path : paths_) embyPlaying = embyPlaying || emby::isLocator(path);
    // The only video being a local file, the edge brings up its folder.
    const int single = singleLoadedPane();
    const bool localPlaying = single >= 0 && !paths_[static_cast<std::size_t>(single)].empty() &&
                              !emby::isLocator(paths_[static_cast<std::size_t>(single)]);
    // A menu open is the pointer at work too: ours, drawn in the sheet or
    // over the picture, and the system menu Windows opens from the caption,
    // over which the pointer is on another window altogether. So is an
    // input method's composition in the search box: its candidate list is
    // another program's window over the sheet.
    GUITHREADINFO gui{sizeof(GUITHREADINFO)};
    const bool windowsMenu = GetGUIThreadInfo(GetCurrentThreadId(), &gui) &&
                             (gui.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE)) != 0;
    const bool composing = fieldTyping() != TextFieldId::None && !fieldComposition_.empty();
    const bool busy = controlDrag_ != ControlDrag::None || draggingPane_ || pressedBarItem_ >= 0 ||
                      pressedPanelKind_ != PanelHitKind::None || panelMenu_.has_value() || syncPanel_.has_value() ||
                      windowsMenu || composing;
    const float edge = std::max(2.0F, 6.0F * uiScale_);
    // The caption's strip is left out: a pointer heading for Close in the
    // top-right corner is not asking for the list.
    const bool atEdge = inside && static_cast<float>(local.x) >= static_cast<float>(client.right) - edge &&
                        static_cast<float>(local.y) >= captionInset();
    if (!settingsOpen_) {
        // Re-armed once the pointer has been away from the edge, so a
        // browser closed at the edge does not come straight back.
        if (!atEdge) embyEdgeArmed_ = true;
        if (atEdge && embyEdgeArmed_ && !busy &&
            (localPlaying || (embyPlaying && emby_.session().signedIn()))) {
            embyEdgeArmed_ = false;
            if (localPlaying) openLocalList();
            else embyOpenBrowser();
            embyBrowserByEdge_ = settingsOpen_ && embyBrowserOpen_;
            embyEdgeLeftSince_ = 0;
        }
        return;
    }
    if (!embyBrowserByEdge_) return;
    const bool nearSheet = inside && static_cast<float>(local.x) >= panelLayout_.sheet.x - 8.0F * uiScale_;
    // The transport bar is laid out beside the docked browser. Closing the
    // browser under a pointer that went from it to the bar would stretch
    // the bar to the full width, and the rail with it, under the pointer.
    // While the bar is up its band holds the browser as the sheet does.
    const int bandHeight = static_cast<int>(std::lround(BarMetrics{}.bandHeight * uiScale_));
    const bool onBar = inside && dockProgress_ > 0.05F &&
        (hotBarItem_ >= 0 || pointerInDockInteractiveBand(
            local.y, client.bottom, dockProgress_, bandHeight, dp(kControlRevealZone)));
    if (nearSheet || onBar || busy) {
        embyEdgeLeftSince_ = 0;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (embyEdgeLeftSince_ == 0) {
        embyEdgeLeftSince_ = now;
        return;
    }
    if (now - embyEdgeLeftSince_ >= kCloseDelayMs) {
        closeSettingsPanel();
        embyBrowserOpen_ = false;
        embyBrowserByEdge_ = false;
        embyEdgeLeftSince_ = 0;
        embyEdgeArmed_ = !atEdge;
    }
}

void App::embyAccountAction(int part) {
    if (!emby_.session().signedIn()) {
        embySubmitSignIn();
        return;
    }
    if (part == 0) embyOpenBrowser();
    else embySignOut();
}

void App::embyNavigate(int part) {
    if (embyPendingPlay_) { embyCancelReplacement(); return; }
    // The cases below are numbered by the longer row: Back, Search,
    // Favorites, Home, Refresh. The home page's own row has no Back, so its
    // parts are one short; an information page always shows the longer
    // row, also over the home page's Continue watching.
    if (embyDetails_) {
        if (part == 0) { embyCloseDetails(); return; }
        if (part == 4) { embyRequestDetails(); return; }
        embyCloseDetails();
        if (part == 3) { embyGoHome(); return; }
    } else if (embyPages_.size() <= 1) {
        ++part;
    }
    switch (part) {
    case 0:
        if (embyPages_.size() > 1) {
            // A person opened from an information page goes back to it.
            const auto from = std::move(embyPages_.back().fromDetails);
            embyPages_.pop_back();
            if (from) {
                embyReturnToDetails(from);
                break;
            }
            // Back to where that page was left, once it has arrived.
            embyPendingScroll_ = embyPages_.back().scroll;
            embyRequestPage();
        }
        break;
    case 1:
        embyOpenSearch();
        break;
    case 2:
        embyOpenFavorites();
        break;
    case 3:
        // The home page's third button signs out; every other page's goes home.
        if (embyPages_.size() <= 1) embySignOut();
        else embyGoHome();
        break;
    case 4:
        embyRefreshShown();
        break;
    default:
        break;
    }
}

bool App::embyPageIsPaged() const {
    if (embyDetails_) return false;
    if (embyPages_.empty()) return false;
    const auto kind = embyPages_.back().kind;
    return kind == EmbyPage::Kind::Library || kind == EmbyPage::Kind::Folder || kind == EmbyPage::Kind::Playlist ||
           kind == EmbyPage::Kind::Series || kind == EmbyPage::Kind::Season || kind == EmbyPage::Kind::Favorites ||
           kind == EmbyPage::Kind::Person;
}

bool App::embyPageAcceptsFlat() const {
    if (embyDetails_) return false;
    if (embyPages_.empty()) return false;
    const auto& page = embyPages_.back();
    if (page.kind != EmbyPage::Kind::Library && page.kind != EmbyPage::Kind::Folder) return false;
    return emby::listTakesFlat(embyListPage(page));
}

bool App::embyPageAcceptsFilters() const {
    if (embyDetails_) return false;
    if (embyPages_.empty()) return false;
    const auto kind = embyPages_.back().kind;
    // Paging a show's seasons/episodes must not apply the unrelated
    // library's saved sort, Unplayed or Flatten choices to its hierarchy.
    return kind == EmbyPage::Kind::Library || kind == EmbyPage::Kind::Folder ||
           kind == EmbyPage::Kind::Playlist || kind == EmbyPage::Kind::Search ||
           kind == EmbyPage::Kind::Favorites || kind == EmbyPage::Kind::Person;
}

bool App::embyListInServerRandom() const {
    if (!embyPageAcceptsFilters()) return false;
    const auto& page = embyPages_.back();
    // A search merges its replies and keeps their order itself.
    if (page.kind == EmbyPage::Kind::Search || (page.kind == EmbyPage::Kind::Playlist && page.ownOrder)) return false;
    return emby::sortKeyFromIndex(embyBrowser_.sort) == emby::SortKey::Random;
}

std::string App::embyPageKey(const EmbyPage& page) {
    switch (page.kind) {
    case EmbyPage::Kind::Library: return "library:" + page.id;
    case EmbyPage::Kind::Folder: return "folder:" + page.id;
    case EmbyPage::Kind::Playlist: return "playlist:" + page.id;
    case EmbyPage::Kind::Series: return "series:" + page.id;
    case EmbyPage::Kind::Season: return "season:" + page.seriesId + "/" + page.id;
    case EmbyPage::Kind::Search:
        return page.searchesScope() ? "search:" + page.scopeId + ":" + page.term : "search:" + page.term;
    case EmbyPage::Kind::Favorites: return "favorites";
    case EmbyPage::Kind::Person: return "person:" + page.id;
    default: return "home";
    }
}

emby::ListPage App::embyListPage(const EmbyPage& page) {
    emby::ListPage list;
    list.kind = page.kind == EmbyPage::Kind::Library ? emby::ListKind::Library
              : page.kind == EmbyPage::Kind::Playlist ? emby::ListKind::Playlist
              : page.kind == EmbyPage::Kind::Favorites ? emby::ListKind::Favorites
              : page.kind == EmbyPage::Kind::Person ? emby::ListKind::Person
                                                       : emby::ListKind::Folder;
    list.id = page.id;
    list.collectionType = page.collectionType;
    list.itemType = page.itemType;
    list.ownOrder = page.ownOrder;
    return list;
}

std::string App::embyListPath(const EmbyPage& page, int startIndex, int limit) const {
    const auto& userId = emby_.session().userId;
    if (page.kind == EmbyPage::Kind::Series) {
        return emby::seasonsPath(page.id, userId, kEmbyListFields, startIndex, limit);
    }
    if (page.kind == EmbyPage::Kind::Season) {
        if (!page.seriesId.empty()) {
            return emby::episodesPath(page.seriesId, userId, page.id, kEmbyListFields, startIndex, limit);
        }
        // A season opened outside its series can lack SeriesId/ParentId.
        // Its own id still identifies its children; do not make it inert
        // or guess that some other page is its parent series.
        emby::ItemsQuery query;
        query.parentId = page.id;
        query.includeTypes = "Episode";
        // IndexNumber is an Emby core ItemSortBy value. A plain title
        // sort would put a season's episodes in alphabetical order.
        query.sortBy = "IndexNumber,SortName";
        query.sortOrder = "Ascending,Ascending";
        query.fields = kEmbyListFields;
        query.startIndex = startIndex;
        query.limit = limit;
        return emby::itemsPath(userId, query);
    }
    emby::ListArrangement arrangement;
    arrangement.sort = emby::sortKeyFromIndex(embyBrowser_.sort);
    arrangement.descending = embyBrowser_.descending;
    arrangement.unplayed = embyBrowser_.unplayed;
    arrangement.flat = embyBrowser_.flat;
    arrangement.favorites = embyBrowser_.favorites;
    return emby::itemsPath(userId,
                           emby::listQuery(embyListPage(page), arrangement, startIndex, limit, kEmbyListFields));
}

std::vector<std::string> App::embySearchParents(const EmbyPage& page) const {
    // Opened in a folder: what is under it, at any depth, is one request.
    if (page.searchesScope()) return {page.scopeId};
    // The server matches the start of words, and honours a library's
    // "exclude from search" only when asked without a ParentId; asking
    // each library by name finds everything the account can see.
    std::vector<std::string> libraries;
    for (const auto& view : embyViews_) {
        if (view.type == "CollectionFolder" || view.type == "UserView") libraries.push_back(view.id);
    }
    if (libraries.empty()) libraries.push_back({});
    return libraries;
}

std::string App::embySearchPath(const std::string& parentId, const std::string& term) const {
    return emby::itemsPath(emby_.session().userId,
                           emby::searchQuery(parentId, term,
                                             emby::arrangementFilters(embyBrowser_.unplayed, embyBrowser_.favorites),
                                             kEmbySearchPerLibrary, kEmbyListFields));
}

void App::embyToolbarAction(int part) {
    if (!emby_.session().signedIn() || embyDetails_ || embyPages_.empty() || part < 0 || part > 5) return;
    const auto account = embyAccountSerial_;
    const auto pageKey = embyPageKey(embyPages_.back());
    const bool home = embyPages_.size() <= 1;
    const bool searchEmpty = embyPages_.back().kind == EmbyPage::Kind::Search && embyPages_.back().term.empty();
    const bool arrange = !home && embyPageAcceptsFilters() && !searchEmpty;
    const bool playlist = embyPages_.back().kind == EmbyPage::Kind::Playlist;
    const bool ownOrder = playlist && embyPages_.back().ownOrder;
    // The filters, each its own button, switch at once; the row saves them
    // (activatePanelRow). Information pages are only a movie or TV
    // library's, which embyToggleLibraryDetails checks.
    if (part >= 2) {
        if (part == 2 && arrange) embyToggleUnplayed();
        else if (part == 3 && arrange && embyPages_.back().kind != EmbyPage::Kind::Favorites) {
            embyToggleFavoritesFilter();
        } else if (part == 4 && arrange && embyPageAcceptsFlat()) embyToggleFlat();
        else if (part == 5) embyToggleLibraryDetails();
        return;
    }
    if (part == 1 && !arrange) return;
    PanelMenu menu;
    if (part == 0) {
        const int view = embyShownView();
        menu.option(1, L"List", view == 0);
        if (embyPageHasPosters()) menu.option(2, L"Posters", view == 1);
        menu.option(3, L"Thumbnails", view == 2);
    } else {
        if (playlist) menu.option(1, L"Playlist order", ownOrder);
        for (int index = 0; index < emby::kSortKeyCount; ++index) {
            const auto key = emby::sortKeyFromIndex(index);
            menu.option(static_cast<unsigned>(10 + index), emby::sortKeyName(key),
                        !ownOrder && embyBrowser_.sort == index, sortShortcut(key));
        }
    }
    openPanelMenu(std::move(menu), SettingId::EmbyToolbar, part, -1,
                  [this, part, account, pageKey, playlist](unsigned command) {
        // Chosen later: replies may have moved the account or the page on.
        if (!command || account != embyAccountSerial_ || embyDetails_ || embyPages_.empty() ||
            pageKey != embyPageKey(embyPages_.back()) || !settingsOpen_ || !embyBrowserOpen_ ||
            browserSource_ != BrowserSource::Emby) return;
        // The thumbnails shown over the browser's Posters leave it alone when
        // chosen, so the view already shown is no choice at all.
        if (part == 0 && command <= 3) {
            if (static_cast<int>(command) - 1 != embyShownView()) embySetView(static_cast<int>(command) - 1);
        } else if (part == 1) {
            if (command == 1 && playlist) embyChooseSort(0);
            // The checked order chosen again reverses it, as its key does;
            // Random reshuffles.
            else if (command >= 10 && command < 10 + emby::kSortKeyCount) {
                embyChooseSort(static_cast<int>(command) - 10 + (playlist ? 1 : 0));
            }
        }
        scheduleAppSettingsSave();
    });
}

void App::embySetView(int view) {
    view = std::clamp(view, 0, 2);
    if (view == embyBrowser_.view) return;
    embyBrowser_.view = view;
    settingsScroll_ = 0.0F;
}

bool App::embyPageHasPosters() const {
    return emby::libraryHasPosters(embyLibraryType(embyCurrentLibraryId()));
}

int App::embyShownView() const {
    return embyBrowser_.view == 1 && !embyPageHasPosters() ? 2 : embyBrowser_.view;
}

void App::embyChooseSort(int part) {
    if (embyPages_.empty() || embyPages_.back().kind != EmbyPage::Kind::Playlist) {
        embySetSort(part);
        return;
    }
    if (part > 0) {
        embySetSort(part - 1);
        return;
    }
    if (embyPages_.back().ownOrder) return;
    embyPages_.back().ownOrder = true;
    if (embyBrowserOpen_ && browserSource_ == BrowserSource::Emby) {
        embyRequestPage();
        const auto pane = embyPlaybackTarget();
        if (embyPaneAccountMatches(pane)) embyPanes_[pane]->queueReorderSerial = embyRequestSerial_;
    }
}

void App::embySetSort(int sort) {
    if (embyDetails_) embyCloseDetails();
    sort = std::clamp(sort, 0, emby::kSortKeyCount - 1);
    // A playlist shown in its own order takes the order as newly chosen:
    // the first choice sorts by it, only the next one reverses.
    const bool fromOwnOrder = !embyPages_.empty() && embyPages_.back().kind == EmbyPage::Kind::Playlist &&
                              embyPages_.back().ownOrder;
    if (fromOwnOrder) embyPages_.back().ownOrder = false;
    if (sort == embyBrowser_.sort) {
        if (!fromOwnOrder && emby::sortKeyFromIndex(sort) != emby::SortKey::Random) {
            embyBrowser_.descending = !embyBrowser_.descending;
        }
    } else {
        embyBrowser_.sort = sort;
        embyBrowser_.descending = emby::sortDescendsByDefault(emby::sortKeyFromIndex(sort));
    }
    if (embyPages_.size() <= 1) return;
    // Choosing Random again is an explicit reshuffle. Hidden sort keys
    // retire the old page instead: reopening must fetch the new choice
    // once, without moving the scroll of Settings or the local list now.
    if (settingsOpen_ && embyBrowserOpen_ && browserSource_ == BrowserSource::Emby) {
        embyRequestPage();
        const auto pane = embyPlaybackTarget();
        if (embyPaneAccountMatches(pane)) embyPanes_[pane]->queueReorderSerial = embyRequestSerial_;
    } else {
        embyRetireHiddenList();
    }
}

void App::embyRetireHiddenList() {
    ++embyRequestSerial_;
    embyItems_.clear();
    embyTotal_ = 0;
    embyLoading_ = embyLoadingMore_ = embyRefreshing_ = embyRefreshQuiet_ = false;
    embyListAskedTick_ = 0;
    embyPendingScroll_ = -1.0F;
}

void App::sortListBy(int sort) {
    embySetSort(sort);
    const auto key = emby::sortKeyFromIndex(embyBrowser_.sort);
    // The list being played follows at once; the browser, when open, asks
    // the server again and adopts its answer when it lands.
    if (!(embyBrowserOpen_ && browserSource_ == BrowserSource::Local)) embyReorderPlaylist([&](std::vector<emby::Item>& list) {
        if (key == emby::SortKey::Random) std::shuffle(list.begin(), list.end(), shuffleRandom_);
        else emby::sortItems(list, key, embyBrowser_.descending);
    });
    // A local folder follows the same key.
    orderLocalList();
    std::wstring notice = std::wstring(L"Sort: ") + emby::sortKeyName(key);
    if (key != emby::SortKey::Random) notice += embyBrowser_.descending ? L", descending" : L", ascending";
    showNotice(notice);
    scheduleAppSettingsSave();
}

void App::embyAdoptPlaylist() {
    const std::string page = embyPages_.empty() ? std::string() : embyPageKey(embyPages_.back());
    for (std::size_t pane = 0; pane < embyPanes_.size(); ++pane) {
        if (!embyPaneAccountMatches(pane)) continue;
        auto& slot = *embyPanes_[pane];
        const bool explicitOrder = slot.queueReorderSerial != 0 && slot.queueReorderSerial == embyRequestSerial_;
        if (emby::adoptQueue(slot.queue, embyItems_, slot.itemId, page, explicitOrder)) slot.queueReorderSerial = 0;
    }
}

void App::embyTakePlaylist(const std::vector<emby::Item>& list, int chosen, std::size_t pane,
                           const std::string& page) {
    if (pane < embyPanes_.size() && embyPanes_[pane]) embyPanes_[pane]->queue = emby::takeQueue(list, chosen, page);
}

int App::embyPlaylistIndex(const std::string& itemId, std::size_t pane) const {
    return pane < embyPanes_.size() && embyPanes_[pane] ? emby::queueIndex(embyPanes_[pane]->queue, itemId) : -1;
}

void App::embyPlayFromPlaylist(int index, std::size_t pane, bool selectTarget) {
    if (!embyPaneAccountMatches(pane)) return;
    auto queue = embyPanes_[pane]->queue;
    if (index < 0 || index >= static_cast<int>(queue.items.size())) return;
    const emby::Item entry = queue.items[static_cast<std::size_t>(index)];
    const int existing = embyPlayingPane(entry.id);
    if (existing == static_cast<int>(pane)) {
        // Repeated playlist entries are distinct queue positions, even
        // though the item still has just one decoder/reporting session.
        embyPanes_[pane]->queue.cursor = index;
        if (embyPanes_[pane]->endHandled && sources_[pane]) {
            sourcePaused_[pane] = false;
            seekPaneTo(pane, 0.0);
        }
        if (selectTarget) embySelectTarget(pane);
        return;
    }
    if (existing >= 0) {
        if (selectTarget) embySelectTarget(static_cast<std::size_t>(existing));
        showNotice(L"Already open in V" + std::to_wstring(positionForPane(static_cast<std::size_t>(existing)) + 1));
        return;
    }
    queue.cursor = index;
    embyPlayItem(entry, pane, false, false, selectTarget);
    if (embyPanes_[pane] && embyPanes_[pane]->itemId == entry.id) embyPanes_[pane]->queue = std::move(queue);
}

void App::embyReorderPlaylist(const std::function<void(std::vector<emby::Item>&)>& order) {
    const auto pane = embyPlaybackTarget();
    if (embyPaneAccountMatches(pane)) emby::reorderQueue(embyPanes_[pane]->queue, order);
}

void App::embyToggleUnplayed() {
    embyBrowser_.unplayed = !embyBrowser_.unplayed;
    if (embyBrowserOpen_ && embyPageAcceptsFilters()) embyRequestPage();
}

void App::embyToggleFavoritesFilter() {
    embyBrowser_.favorites = !embyBrowser_.favorites;
    if (embyBrowserOpen_ && embyPageAcceptsFilters() && embyPages_.back().kind != EmbyPage::Kind::Favorites) {
        embyRequestPage();
    }
}

bool App::embyListsFavoritesOnly() const {
    // The list under an information page counts too: what is unmarked
    // there leaves it.
    if (embyPages_.empty()) return false;
    const auto kind = embyPages_.back().kind;
    return kind == EmbyPage::Kind::Favorites ||
           (embyBrowser_.favorites && (kind == EmbyPage::Kind::Library || kind == EmbyPage::Kind::Folder ||
                                       kind == EmbyPage::Kind::Playlist || kind == EmbyPage::Kind::Search ||
                                       kind == EmbyPage::Kind::Person));
}

void App::embySetFlat(bool flat) {
    if (flat == embyBrowser_.flat) return;
    embyBrowser_.flat = flat;
    if (embyBrowserOpen_ && embyPageAcceptsFlat()) embyRequestPage();
}

void App::embyToggleFlat() { embySetFlat(!embyBrowser_.flat); }

void App::embyRequestVisibleImages() {
    const bool signedIn = emby_.session().signedIn();
    const auto& content = panelLayout_.content;
    // One line above and below the sheet too, so a scroll finds them there.
    const float margin = content.height;
    const float top = content.y - margin;
    const float bottom = content.bottom() + margin;
    if (signedIn) embyAskVisibleShapes(top, bottom);
    // False once both queues are full.
    const auto requestRow = [&](std::size_t i) {
        const auto& row = panelRows_[i];
        if (row.kind != PanelRowKind::Tiles && row.kind != PanelRowKind::CompactItem &&
            row.kind != PanelRowKind::MediaDetail) return true;
        const auto& box = panelLayout_.rows[i].row;
        if (box.bottom() < top || box.y > bottom) return true;
        const std::vector<std::string> keys = row.kind == PanelRowKind::MediaDetail
            ? std::vector<std::string>{row.mediaDetail.posterKey, row.mediaDetail.backdropKey, row.mediaDetail.logoKey}
            : row.tileKeys;
        for (const auto& key : keys) {
            if (embyImagesInFlight_.size() >= kEmbyImageQueue &&
                localThumbnailsInFlight_.size() >= kLocalThumbnailQueue) return false;
            if (key.empty() || thumbnails_.contains(key)) continue;
            if (const auto local = parseLocalThumbnailKey(key)) {
                // A local file's picture is the shell's, made off this thread
                // and queued apart from the server's (kLocalThumbnailQueue).
                if (localThumbnailsInFlight_.count(key) || localThumbnailsInFlight_.size() >= kLocalThumbnailQueue) {
                    continue;
                }
                localThumbnailsInFlight_.insert(key);
                localThumbnails_.request(local->path, local->width, std::max(1, local->width * 9 / 16),
                                         [this, key](const LocalThumbnailer::Result& result) {
                    localThumbnailsInFlight_.erase(key);
                    if (result.cancelled) return;
                    thumbnails_.insert(key, result.bytes);
                });
                continue;
            }
            if (embyImagesInFlight_.count(key) || embyImagesInFlight_.size() >= kEmbyImageQueue) continue;
            if (!signedIn) continue;
            const auto parsed = emby::parseImageKey(key);
            if (!parsed) continue;
            embyImagesInFlight_.insert(key);
            const auto accountSerial = embyAccountSerial_;
            embyImages_.request("GET", emby::imagePath(*parsed), "",
                                [this, key, accountSerial](const EmbyClient::Response& response) {
                if (accountSerial != embyAccountSerial_) return;
                embyImagesInFlight_.erase(key);
                if (response.error == "Cancelled") return;
                // A failure is kept as no picture: the tile shows its title
                // instead of asking again every frame.
                thumbnails_.insert(key, response.ok ? response.body : std::string());
            });
        }
        return true;
    };
    // The pinned rows, then the content's from the first reaching the band:
    // they follow each other down, so the rest of a long list is not looked at.
    const std::size_t count = std::min(panelRows_.size(), panelLayout_.rows.size());
    const std::size_t pinned = std::min(panelLayout_.pinnedRows, count);
    for (std::size_t i = 0; i < pinned; ++i) {
        if (!requestRow(i)) return;
    }
    for (std::size_t i = std::max(pinned, panelFirstRowReaching(panelLayout_, top));
         i < count && panelLayout_.rows[i].row.y <= bottom; ++i) {
        if (!requestRow(i)) return;
    }
}

// The server takes a video's picture from its frame as stored, so a video
// whose pixels are not square (a 9:16 phone clip stored as 1280x720) has a
// stretched one; the painter draws it in the video's shape once that is
// known (pictureDrawAspect). The videos -- and the children whose pictures
// folders borrow -- of the tiles in the band are asked about once each, a
// screenful at a time, one question out.
void App::embyAskVisibleShapes(float top, float bottom) {
    if (embyShapeQuestionOut_ || !emby_.session().signedIn()) return;
    std::vector<std::string> ids;
    const auto consider = [&](const emby::Item& entry) {
        if (entry.primaryImageTag.empty()) return;
        const bool borrowed = !entry.primaryImageItemId.empty();
        if (!borrowed && !emby::isPlayableVideo(entry)) return;
        const std::string& source = borrowed ? entry.primaryImageItemId : entry.id;
        if (source.empty() || thumbnails_.knowsVideoShape(source) ||
            std::find(ids.begin(), ids.end(), source) != ids.end()) {
            return;
        }
        ids.push_back(source);
    };
    const std::size_t count = std::min(panelRows_.size(), panelLayout_.rows.size());
    for (std::size_t i = panelFirstRowReaching(panelLayout_, top);
         i < count && panelLayout_.rows[i].row.y <= bottom && ids.size() < kEmbyShapeBatch; ++i) {
        const auto& row = panelRows_[i];
        if (row.kind != PanelRowKind::Tiles || row.id != SettingId::EmbyItem) continue;
        for (const int param : row.tileParams) {
            const emby::Item* entry = nullptr;
            if (param >= 0 && static_cast<std::size_t>(param) < embyItems_.size()) {
                entry = &embyItems_[static_cast<std::size_t>(param)];
            } else if (param < 0 && static_cast<std::size_t>(-(param + 1)) < embyResume_.size()) {
                entry = &embyResume_[static_cast<std::size_t>(-(param + 1))];
            }
            if (entry) consider(*entry);
        }
    }
    if (ids.empty()) return;
    if (ids.size() > kEmbyShapeBatch) ids.resize(kEmbyShapeBatch);
    embyShapeQuestionOut_ = true;
    const auto session = emby_.session();
    emby_.request("GET", emby::videoShapesPath(session.userId, ids), "",
                  [this, account = embyAccountSerial_, asked = ids](const EmbyClient::Response& response) {
        embyShapesArrived(account, asked, response);
    });
}

void App::embyShapesArrived(std::uint64_t account, const std::vector<std::string>& asked,
                            const EmbyClient::Response& response) {
    if (account != embyAccountSerial_) return;
    embyShapeQuestionOut_ = false;
    // Called off with a page left: asked again when they show.
    if (response.error == "Cancelled") return;
    const auto list = response.ok ? emby::parseItems(response.body) : std::nullopt;
    if (!list) {
        // Unanswered, their pictures are drawn as they came, and they are
        // not asked about again every frame; a 401 is the list's to report.
        if (response.status != 401) {
            appendDiagnostic("Emby: could not ask the shape of " + std::to_string(asked.size()) +
                             " videos: " + (response.ok ? std::string("unreadable reply") : response.error));
        }
        for (const auto& id : asked) thumbnails_.setVideoShape(id, VideoShape{});
        return;
    }
    for (const auto& item : list->items) {
        if (std::find(asked.begin(), asked.end(), item.id) == asked.end()) continue;
        const VideoShape shape{item.storedAspect, item.displayAspect};
        if (shape.stored > 0.0F && shape.display > 0.0F &&
            std::abs(shape.display / shape.stored - 1.0F) > kVideoShapeTolerance) {
            appendDiagnostic("Emby: item " + item.id + " is stored at " + std::to_string(shape.stored) +
                             " and shown at " + std::to_string(shape.display) + ", its picture drawn so");
        }
        thumbnails_.setVideoShape(item.id, shape);
    }
    // One the reply left out has nothing to say about its shape.
    for (const auto& id : asked) {
        if (!thumbnails_.knowsVideoShape(id)) thumbnails_.setVideoShape(id, VideoShape{});
    }
}

void App::embyOpenBrowser() {
    localCancelReplacement();
    if (!emby_.session().signedIn()) {
        // The browser opens on its home page once the sign-in succeeds.
        embyOpenSignIn();
        return;
    }
    if (settingsOpen_) rememberSheetScroll();
    openSheet();
    embyBrowserOpen_ = true;
    browserSource_ = BrowserSource::Emby;
    // Where the browser was left, not its top: the page shown stays and is
    // asked for again in place where that keeps its order. Opening the
    // sheet is not a request to reshuffle Random or repeat a search.
    restoreSheetScroll();
    if (embyDetails_) return;
    if (embyPages_.empty()) {
        embyGoHome();
    } else if (embyLoading_ || embyLoadingMore_ || embyRefreshing_) {
        // Reopening must not supersede a page already on its way.
        return;
    } else if (embyItems_.empty()) {
        embyRequestPage();
    } else if (embyRefreshState(GetTickCount64()).keepsOrder) {
        embyRequestPage(0, true);
    }
}

void App::embyGoHome() {
    embyCloseDetails();
    embyPages_.clear();
    embyPages_.push_back(EmbyPage{});
    embyPendingScroll_ = -1.0F;
    embyRequestPage();
}

void App::embyOpenFavorites() {
    if (!emby_.session().signedIn()) return;
    // Pressed again on its own page, it is asked for again: what was marked
    // elsewhere since shows.
    if (!embyDetails_ && !embyPages_.empty() && embyPages_.back().kind == EmbyPage::Kind::Favorites) {
        embyRefreshShown();
        return;
    }
    EmbyPage page;
    page.kind = EmbyPage::Kind::Favorites;
    page.title = "Favorites";
    embyPushPage(std::move(page));
}

void App::embyPushPage(EmbyPage page) {
    // Leaving the Search page, what was typed and not yet searched for is
    // its term, so Back finds the box and the results agreeing.
    blurField();
    embyCloseDetails();
    if (!embyPages_.empty()) embyPages_.back().scroll = settingsScroll_;
    embyPages_.push_back(std::move(page));
    embyPendingScroll_ = -1.0F;
    embyRequestPage();
}

void App::embyLoadMore() {
    if (embyDetails_) {
        embyRequestDetailChildren(static_cast<int>(embyDetails_->seasonId.empty()
            ? embyDetails_->seasons.size() : embyDetails_->episodes.size()));
        return;
    }
    embyAutoMoreBlocked_ = false;
    embyRequestPage(static_cast<int>(embyItems_.size()));
}

void App::embyRefreshShown() {
    if (!emby_.session().signedIn()) return;
    // Asked by hand: titles whose library could not be found are asked again.
    embyLibraryUnknown_.clear();
    if (embyDetails_) { embyRequestDetails(); return; }
    if (embyPages_.empty()) {
        embyGoHome();
        return;
    }
    thumbnails_.forgetFailures();
    // What has subtitles is asked again with it: a file put beside a video
    // shows once the server has found it.
    embySubtitlesAgain_ = true;
    // Asked by hand, the list is read again whatever came back.
    embyListReply_.reset();
    // A person's page asks again who they are, too.
    embyRequestPerson();
    // A search is several requests merged and is run again; any other
    // page is asked for in place, the list staying where it is.
    if (embyPages_.back().kind == EmbyPage::Kind::Search || embyItems_.empty()) embyRequestPage();
    else embyRequestPage(0, true);
}

void App::embyWantRefresh(unsigned delayMs) {
    const unsigned long long wanted = GetTickCount64() + delayMs;
    if (embyRefreshWantedTick_ == 0 || wanted < embyRefreshWantedTick_) embyRefreshWantedTick_ = wanted;
}

emby::ListRefreshState App::embyRefreshState(unsigned long long now) const {
    emby::ListRefreshState state;
    state.shown = settingsOpen_ && embyBrowserOpen_ && browserSource_ == BrowserSource::Emby &&
                  emby_.session().signedIn() && !embyPages_.empty() && !embyDetails_;
    state.busy = embyLoading_ || embyLoadingMore_ || embyRefreshing_ || controlDrag_ != ControlDrag::None ||
                 pressedPanelKind_ != PanelHitKind::None || panelPressArmed_;
    if (state.shown) {
        state.keepsOrder = embyPages_.back().kind != EmbyPage::Kind::Search && !embyListInServerRandom();
    }
    state.held = static_cast<int>(embyItems_.size());
    state.now = now;
    state.asked = embyListAskedTick_;
    state.wanted = embyRefreshWantedTick_;
    return state;
}

void App::embyRefreshWhenDue(unsigned long long now) {
    if (!emby::listRefreshDue(embyRefreshState(now))) return;
    embyRequestPage(0, true);
    // Nobody asked for this one: should it fail, the list stays as it is.
    embyRefreshQuiet_ = true;
}

std::string App::embyPressedItem() const {
    if (pressedPanelKind_ == PanelHitKind::None || pressedPanelId_ != SettingId::EmbyItem) return {};
    const bool resume = pressedPanelParam_ < 0;
    const auto& list = resume ? embyResume_ : embyItems_;
    const std::size_t index = static_cast<std::size_t>(resume ? -pressedPanelParam_ - 1 : pressedPanelParam_);
    return index < list.size() ? list[index].id : std::string();
}

void App::embyKeepPressOn(const std::string& item) {
    if (item.empty() || embyPressedItem() == item) return;
    // The list changed under the press: its release would choose whatever
    // has come to stand where the pressed item stood.
    pressedPanelKind_ = PanelHitKind::None;
    pressedPanelRow_ = pressedPanelPart_ = -1;
    pressedPanelId_ = SettingId::None;
    pressedPanelParam_ = -1;
}

void App::embyLoadMoreNearEnd() {
    // A page being asked for again in place is not overtaken by its next
    // page: the newer request would be the only one whose reply counts.
    if (embyRefreshing_) return;
    if (embyLoading_ || embyLoadingMore_ || embyAutoMoreBlocked_ || !embyPageIsPaged()) return;
    if (embyItems_.empty() || embyTotal_ <= static_cast<int>(embyItems_.size())) return;
    if (embyPages_.back().kind == EmbyPage::Kind::Season &&
        embyItems_.size() >= static_cast<std::size_t>(kEmbyEpisodeAutoLimit)) return;
    if (!emby_.session().signedIn()) return;
    // Within a sheet's height of the list's end: the next page is there by
    // the time the scroll reaches it. Or while a video played from this list
    // has yet to come -- sorted again, it may be pages down -- so the
    // scrollbar can mark it without the list being scrolled to it.
    const auto playedFromHereStillToCome = [&] {
        const std::string page = embyPageKey(embyPages_.back());
        for (std::size_t pane = 0; pane < embyPanes_.size(); ++pane) {
            if (!embyPaneAccountMatches(pane)) continue;
            const auto& slot = *embyPanes_[pane];
            if (slot.itemId.empty() || slot.queue.page != page) continue;
            if (std::none_of(embyItems_.begin(), embyItems_.end(),
                             [&](const emby::Item& item) { return item.id == slot.itemId; })) return true;
        }
        return false;
    };
    if (settingsScroll_ < panelLayout_.maxScroll - panelLayout_.content.height && !playedFromHereStillToCome()) return;
    embyRequestPage(static_cast<int>(embyItems_.size()));
}

void App::embyRequestPage(int startIndex, bool inPlace) {
    if (embyPages_.empty()) embyPages_.push_back(EmbyPage{});
    const EmbyPage page = embyPages_.back();
    const auto session = emby_.session();
    if (!session.signedIn()) {
        embyLoading_ = false;
        embyNote_ = L"Not signed in.";
        embyNoteError_ = false;
        return;
    }
    const bool more = startIndex > 0;
    if (!more && !inPlace) {
        embyItems_.clear();
        embyTotal_ = 0;
        settingsScroll_ = 0.0F;
        embyImages_.clearPending();
        // The questions about the page left that are not out yet.
        embyDropSubtitleQuestions();
        embyListReply_.reset();
    }
    embyLoadingMore_ = more;
    embyRefreshing_ = inPlace;
    embyRefreshQuiet_ = false;
    if (!more) {
        embyAutoMoreBlocked_ = false;
        embyListAskedTick_ = GetTickCount64();
        embyRefreshWantedTick_ = 0;
    }
    if (!inPlace) {
        embyNote_.clear();
        embyNoteError_ = false;
        // In place nothing says "Loading", and a further page says so under
        // the list: a row appearing above it and going would move the list
        // that is meant to stay where it is.
        embyLoading_ = !more;
    }
    // In the server's random order a further page asks the length that
    // drew the list instead (below).
    const int pageSize = page.kind == EmbyPage::Kind::Playlist || more ? kEmbyWholePageSize : kEmbyPageSize;
    // In place the whole list shown is asked for again, not its first page.
    int pageLimit = inPlace ? std::max(pageSize, static_cast<int>(embyItems_.size())) : pageSize;
    if (!inPlace && page.kind == EmbyPage::Kind::Season && startIndex < kEmbyEpisodeAutoLimit) {
        pageLimit = std::min(pageLimit, kEmbyEpisodeAutoLimit - startIndex);
    }
    // A further page of the server's random order goes on through the draw
    // the list came from only at the length that drew it.
    if (more) pageLimit = emby::furtherPageLimit(pageLimit, embyListDrawnWith_, embyListInServerRandom());
    embyListAskedWith_ = pageLimit;
    const std::uint64_t serial = ++embyRequestSerial_;
    // A search still out is overtaken: the libraries it had yet to ask,
    // queued on the one worker ahead of this page, are not asked.
    if (embySearchDropped_) embySearchDropped_->store(true);
    embySearchDropped_.reset();
    if (more) {
        for (auto& slot : embyPanes_) {
            if (slot && slot->queueReorderSerial == serial - 1 && slot->queue.page == embyPageKey(page)) {
                slot->queueReorderSerial = serial;
            }
        }
    }
    const auto sortKey = emby::sortKeyFromIndex(embyBrowser_.sort);
    const auto listHandler = [this, serial, startIndex](const EmbyClient::Response& response) {
        embyListArrived(serial, startIndex, response);
    };
    const std::string& userId = session.userId;
    switch (page.kind) {
    case EmbyPage::Kind::Home:
        emby_.request("GET", emby::viewsPath(userId), "",
                      [this, serial, listHandler, account = embyAccountSerial_](const EmbyClient::Response& response) {
            listHandler(response);
            embyViewsArrived(serial, account, response);
        });
        emby_.request("GET", emby::resumePath(userId, 12), "", [this, serial](const EmbyClient::Response& response) {
            if (serial != embyRequestSerial_ || !response.ok) return;
            if (auto list = emby::parseItems(response.body)) {
                const std::string pressed = embyPressedItem();
                embyResume_ = std::move(list->items);
                embyKeepPressOn(pressed);
                embyResolveResumeLibraries();
                embyAskSubtitles(embyResume_, false);
            }
        });
        break;
    case EmbyPage::Kind::Library:
    case EmbyPage::Kind::Folder:
    case EmbyPage::Kind::Playlist:
    case EmbyPage::Kind::Series:
    case EmbyPage::Kind::Season:
    case EmbyPage::Kind::Favorites:
    case EmbyPage::Kind::Person:
        // Who the person is, asked before their titles when the page is
        // opened or come back to; kept fresh, only the titles are asked.
        if (page.kind == EmbyPage::Kind::Person && !more && !inPlace) embyRequestPerson();
        // The path builder keeps the library's arrangement separate from
        // a show's server-ordered seasons and episodes.
        emby_.request("GET", embyListPath(page, startIndex, pageLimit), "",
                      [this, serial, startIndex](EmbyClient::Response& response) {
            embyReceiveList(serial, startIndex, response);
        });
        break;
    case EmbyPage::Kind::Search: {
        // Nothing typed yet: nothing to ask, and the box says so.
        if (page.term.empty()) {
            embyLoading_ = false;
            break;
        }
        const auto parents = embySearchParents(page);
        auto search = std::make_shared<EmbySearch>();
        search->libraries = search->remaining = parents.size();
        search->sortKey = sortKey;
        // A folder's results are in the library the folder is in.
        if (page.searchesScope()) search->folder = page.scopeId;
        const std::string folderLibrary = page.searchesScope() ? embyCurrentLibraryId() : std::string();
        const auto dropped = std::make_shared<std::atomic<bool>>(false);
        embySearchDropped_ = dropped;
        for (const auto& parent : parents) {
            const std::string library = page.searchesScope() ? folderLibrary : parent;
            emby_.request("GET", embySearchPath(parent, page.term), "",
                          [this, serial, search, library](const EmbyClient::Response& response) {
                embySearchArrived(serial, search, library, response);
            }, dropped);
        }
        break;
    }
    }
}

void App::embySearchArrived(std::uint64_t serial, const std::shared_ptr<EmbySearch>& search,
                            const std::string& library, const EmbyClient::Response& response) {
    if (serial != embyRequestSerial_) return;
    if (response.status == 401) {
        embySessionExpired();
        return;
    }
    auto list = response.ok ? emby::parseItems(response.body) : std::nullopt;
    if (list) {
        for (auto& entry : list->items) {
            embyRememberLibrary(entry, library);
            if (search->seen.insert(entry.id).second) embyItems_.push_back(std::move(entry));
        }
    } else {
        // Said in the log by the library's id alone: the words searched for
        // may be a title, and the log goes into bug reports.
        const std::string error = response.ok ? std::string("unreadable reply") : response.error;
        appendDiagnostic("Emby: search in " +
                         (!search->folder.empty() ? "folder " + search->folder
                          : "library " + (library.empty() ? std::string("(all)") : library)) +
                         " failed: " + error);
        if (search->failed++ == 0) search->firstError = error;
    }
    if (--search->remaining != 0) return;
    embyLoading_ = false;
    // The libraries answered one by one; the list is ordered here the way
    // one library's reply would have been, within the sections it is shown
    // in, which is also the order it plays in.
    emby::sortItems(embyItems_, search->sortKey, embyBrowser_.descending);
    emby::groupSearchResults(embyItems_);
    embyAdoptPlaylist();
    embyAskSubtitles(embyItems_, true);
    embyTotal_ = static_cast<int>(embyItems_.size());
    if (search->failed == search->libraries) {
        // Not "nothing found": nothing could be asked.
        EmbyClient::Response failure;
        failure.error = search->firstError;
        embyFailure(failure);
    } else if (search->failed != 0) {
        embyNote_ = std::to_wstring(search->failed) + L" of " + std::to_wstring(search->libraries) +
                    L" libraries could not be searched.";
        embyNoteError_ = true;
    } else {
        embyNote_ = !embyItems_.empty()  ? L"Matches the start of a word in a title."
                  : embyBrowser_.favorites ? L"No favorite's title starts a word with that."
                                           : L"No title starts a word with that.";
        embyNoteError_ = false;
    }
}

void App::embyViewsArrived(std::uint64_t serial, std::uint64_t account, const EmbyClient::Response& response) {
    if (account != embyAccountSerial_ || !response.ok) return;
    if (serial == embyRequestSerial_) {
        embyViews_ = embyItems_;
        // F5 library switches use the view's current row index.
        // A refreshed/reordered Views reply retires any held press.
        panelPressArmed_ = false;
        pressedPanelKind_ = PanelHitKind::None;
    } else if (auto views = emby::parseItems(response.body)) {
        // A page opened before the libraries came -- the favorites, a
        // search -- still needs them to place its titles.
        embyViews_ = std::move(views->items);
    } else {
        return;
    }
    embyResolveShownLibraries();
}

void App::embyListArrived(std::uint64_t serial, int startIndex, const EmbyClient::Response& response) {
    if (serial != embyRequestSerial_) return;
    embyListParsed(serial, startIndex, response,
                   response.ok ? embyParseList(response.body, std::nullopt) : EmbyListParse{});
}

App::EmbyListParse App::embyParseList(std::string body, const std::optional<EmbyListReplyPrint>& shown) {
    EmbyListParse parsed;
    parsed.hash = std::hash<std::string>{}(body);
    parsed.bytes = body.size();
    if (shown && shown->hash == parsed.hash && shown->bytes == parsed.bytes) {
        parsed.unchanged = true;
        parsed.body = std::move(body);
    } else {
        parsed.list = emby::parseItems(body);
    }
    return parsed;
}

void App::embyReceiveList(std::uint64_t serial, int startIndex, EmbyClient::Response& response) {
    if (serial != embyRequestSerial_) return;
    if (!response.ok) {
        embyListArrived(serial, startIndex, response);
        return;
    }
    // Read off this thread: a playlist of 20000 entries asked for again in
    // place is some 10 MB and a third of a second of parsing. What comes back
    // is taken only while it answers the request still out, which keeps the
    // list's Loading and refresh state until then.
    const auto shown = startIndex == 0 && embyRefreshing_ ? embyListReply_ : std::nullopt;
    std::string body = std::move(response.body);
    EmbyClient::Response status = response;
    std::thread([queue = mainQueue_, this, serial, startIndex, shown, status = std::move(status),
                 body = std::move(body)]() mutable {
        auto parsed = embyParseList(std::move(body), shown);
        queue->post([this, serial, startIndex, status = std::move(status), parsed = std::move(parsed)]() mutable {
            embyListParsed(serial, startIndex, status, std::move(parsed));
        });
    }).detach();
}

void App::embyListParsed(std::uint64_t serial, int startIndex, const EmbyClient::Response& response,
                         EmbyListParse parsed) {
    if (serial != embyRequestSerial_) return;
    embyLoading_ = false;
    embyLoadingMore_ = false;
    const bool more = startIndex > 0;
    const bool refreshed = embyRefreshing_;
    const bool quiet = embyRefreshQuiet_;
    embyRefreshing_ = false;
    embyRefreshQuiet_ = false;
    if (!response.ok) {
        // Not asked for again by the scroll that asked for it: a server
        // that is away would be asked every frame.
        embyAutoMoreBlocked_ = more;
        if (quiet && response.status != 401) {
            appendDiagnostic("Emby: the list could not be asked for again: " + response.error);
            return;
        }
        embyFailure(response);
        return;
    }
    // A refresh that brought the bytes the list shown came from leaves the
    // list as it is, while that list is still all it holds; otherwise the
    // reply is read now after all.
    const bool unchanged = parsed.unchanged && refreshed && !more && embyListReply_ &&
                           embyListReply_->hash == parsed.hash && embyListReply_->bytes == parsed.bytes &&
                           embyListReply_->items == embyItems_.size();
    if (parsed.unchanged && !unchanged) parsed.list = emby::parseItems(parsed.body);
    auto& list = parsed.list;
    if (!unchanged && !list) {
        embyAutoMoreBlocked_ = more;
        if (!quiet) {
            embyNote_ = L"The server sent a reply QuadDeck cannot read.";
            embyNoteError_ = true;
        }
        return;
    }
    // Should the server have lost the draw a further page of its random
    // order goes on through -- restarted, or asked the same query from
    // elsewhere -- what that page repeats of the list shown is left out.
    if (more && embyListInServerRandom()) emby::keepUnlisted(list->items, embyItems_);
    // The list is now the draw this request made, at the length it asked.
    if (!more) embyListDrawnWith_ = embyListAskedWith_;
    const bool person = !embyPages_.empty() && embyPages_.back().kind == EmbyPage::Kind::Person;
    // A person's titles are shown under their sections, films first, each
    // in the order asked for; the list plays in that shown order.
    if (person && !unchanged) emby::groupSearchResults(list->items);
    const std::vector<emby::Item>& replied = unchanged ? embyItems_ : list->items;
    const int total = unchanged ? embyListReply_->total : list->total;
    const bool nothingNew = replied.empty();
    if (!unchanged) {
        const std::string libraryId = embyCurrentLibraryId();
        for (const auto& entry : replied) embyRememberLibrary(entry, libraryId);
    }
    const std::string pressed = embyPressedItem();
    // A list looked at anew asks again what has subtitles; one kept fresh
    // unasked asks only about videos it had not shown.
    const bool askAgain = !refreshed || embySubtitlesAgain_;
    embySubtitlesAgain_ = false;
    embyAskSubtitles(replied, askAgain);
    if (more) {
        embyItems_.insert(embyItems_.end(), list->items.begin(), list->items.end());
        // A further page's titles join their sections; those already shown
        // keep their order among themselves, which a queue playing from the
        // page needs (emby::adoptQueue).
        if (person) emby::groupSearchResults(embyItems_);
        if (nothingNew) embyAutoMoreBlocked_ = true;
        // The list now holds more than any one reply.
        embyListReply_.reset();
    } else {
        if (quiet) {
            // The log says once for a page that it is kept fresh, and
            // then whenever the server had news for it.
            const std::string page = embyPages_.empty() ? std::string() : embyPageKey(embyPages_.back());
            const bool news = !unchanged && !emby::sameListing(embyItems_, replied);
            if (news || page != embyRefreshLoggedPage_) {
                appendDiagnostic("Emby: asked again for " + page + ", " + std::to_string(replied.size()) +
                                 (news ? " items, changed on the server" : " items, nothing new"));
            }
            embyRefreshLoggedPage_ = page;
        }
        if (!unchanged) {
            embyItems_ = std::move(list->items);
            embyListReply_ = EmbyListReplyPrint{parsed.hash, parsed.bytes, embyItems_.size(), total};
        }
    }
    embyKeepPressOn(pressed);
    embyTotal_ = std::max(total, static_cast<int>(embyItems_.size()));
    // A page that has come back says nothing of the failure before it.
    embyNoteError_ = false;
    const bool favorites = !embyPages_.empty() && embyPages_.back().kind == EmbyPage::Kind::Favorites;
    if (embyItems_.empty()) {
        embyNote_ = embyBrowser_.unplayed && embyPageAcceptsFilters() ? L"No unplayed videos match these filters."
                  : favorites ? L"No favorites yet. Right-click an item to add it."
                  : embyListsFavoritesOnly() ? L"No favorites here."
                  : person && !embyBrowser_.unplayed ? L"No title in these libraries names this person."
                                                     : L"Nothing here.";
    } else if (refreshed || !embyNote_.empty()) {
        embyNote_.clear();
    }
    // Favorites and a person's titles come from every library: which one
    // holds a movie or a series decides whether it opens an information page.
    if (favorites || person) embyResolveLibraries(embyItems_);
    // A pane playing from this page takes the reply as its list only on
    // emby::adoptQueue's terms: no shorter, every entry it holds still there
    // in its order (new ones may join), reordered only by the sort chosen
    // for that pane. A reply that lacks a held entry -- Unplayed only, an
    // item gone from the server -- leaves the list being played as it was.
    embyAdoptPlaylist();
    if (embyPendingScroll_ >= 0.0F) {
        settingsScroll_ = embyPendingScroll_;
        embyPendingScroll_ = -1.0F;
    }
    // A list is a sequence to play through: the rest is fetched without
    // needing a scroll, the sheet up or not, so Shuffle picks among all of
    // it, In order and Repeat the list reach its end and a season's episode
    // navigation is not cut off at the first 300. A person's titles come
    // whole too, so a later page does not put films above series being
    // read. Past its limit, or after a page that failed or came back empty
    // (embyAutoMoreBlocked_), the scroll or Show more asks for the rest.
    // The home page and a search come in one reply.
    const int held = static_cast<int>(embyItems_.size());
    const auto kind = embyPages_.empty() ? EmbyPage::Kind::Home : embyPages_.back().kind;
    const bool paged = kind != EmbyPage::Kind::Home && kind != EmbyPage::Kind::Search;
    const int wholeLimit = kind == EmbyPage::Kind::Season ? kEmbyEpisodeAutoLimit
                         : person ? kEmbyPersonLimit : kEmbyWholeLimit;
    if (paged && !nothingNew && held < embyTotal_ && held < wholeLimit) embyRequestPage(held);
}

void App::embyAskSubtitles(const std::vector<emby::Item>& items, bool again) {
    const auto session = emby_.session();
    if (!session.signedIn()) return;
    const auto ids = emby::subtitleQuestions(items, embySubtitled_, embySubtitleAsking_, again);
    for (auto& batch : emby::subtitleBatches(ids)) {
        for (const auto& id : batch) embySubtitleAsking_.insert(id);
        embySubtitleQuestions_.push_back(std::move(batch));
    }
    embyAskNextSubtitles();
}

void App::embyAskNextSubtitles() {
    const auto session = emby_.session();
    if (!session.signedIn()) {
        embyDropSubtitleQuestions();
        return;
    }
    while (embySubtitleQuestionsOut_ < kSubtitleQuestionsAtOnce && !embySubtitleQuestions_.empty()) {
        auto batch = std::move(embySubtitleQuestions_.front());
        embySubtitleQuestions_.pop_front();
        ++embySubtitleQuestionsOut_;
        const std::string path = emby::subtitledItemsPath(session.userId, batch);
        emby_.request("GET", path, "",
                      [this, account = embyAccountSerial_, asked = std::move(batch)](const EmbyClient::Response& response) {
            embySubtitlesArrived(account, asked, response);
        });
    }
}

void App::embyDropSubtitleQuestions() {
    // Not asked, so asked again by the next list that shows them.
    for (const auto& batch : embySubtitleQuestions_) {
        for (const auto& id : batch) embySubtitleAsking_.erase(id);
    }
    embySubtitleQuestions_.clear();
}

void App::embySubtitlesArrived(std::uint64_t account, const std::vector<std::string>& asked,
                               const EmbyClient::Response& response) {
    if (account != embyAccountSerial_) return;
    for (const auto& id : asked) embySubtitleAsking_.erase(id);
    if (embySubtitleQuestionsOut_ != 0) --embySubtitleQuestionsOut_;
    embyAskNextSubtitles();
    const auto list = response.ok ? emby::parseItems(response.body) : std::nullopt;
    if (!list) {
        // Unanswered, the videos show no mark and are asked about with
        // the next list that shows them; a 401 is the list's to report.
        if (response.status != 401) {
            appendDiagnostic("Emby: could not ask which of " + std::to_string(asked.size()) +
                             " videos have subtitles: " + (response.ok ? std::string("unreadable reply") : response.error));
        }
        return;
    }
    std::unordered_set<std::string> subtitled;
    for (const auto& item : list->items) subtitled.insert(item.id);
    for (const auto& id : asked) embySubtitled_[id] = subtitled.contains(id);
}

bool App::embyHasSubtitles(const emby::Item& item) const {
    if (!emby::isPlayableVideo(item)) return false;
    const auto known = embySubtitled_.find(item.id);
    return known != embySubtitled_.end() && known->second;
}

// ---------------------------------------------------------------------------
// Favorites

void App::embySetFavorite(const std::string& itemId, const std::wstring& title, bool favorite) {
    const auto session = emby_.session();
    if (!session.signedIn() || itemId.empty()) return;
    // A second change before the first is answered could be applied in
    // either order; the first one's answer comes before another is taken.
    if (!embyFavoriteAsking_.insert(itemId).second) {
        showNotice(L"Emby is still saving the last change to this favorite");
        return;
    }
    emby_.request("POST", emby::favoritePath(session.userId, itemId, favorite), "",
                  [this, account = embyAccountSerial_, itemId, title, favorite](const EmbyClient::Response& response) {
        embyFavoriteArrived(account, itemId, title, favorite, response);
    });
}

void App::embyFavoriteArrived(std::uint64_t account, const std::string& itemId, const std::wstring& title,
                              bool wanted, const EmbyClient::Response& response) {
    if (account != embyAccountSerial_) return;
    embyFavoriteAsking_.erase(itemId);
    if (!response.ok) {
        if (response.status == 401) {
            embySessionExpired();
            return;
        }
        appendDiagnostic("Emby: could not " + std::string(wanted ? "mark" : "unmark") + " item " + itemId +
                         " as a favorite: " + response.error);
        showNotice(L"Emby: the favorite was not changed (" + wide(response.error) + L")");
        return;
    }
    // The reply is the item's user data as the server now holds it; a
    // server that says nothing is taken to have done what it was asked.
    const bool favorite = emby::parseFavoriteReply(response.body).value_or(wanted);
    embyApplyFavorite(itemId, favorite);
    appendDiagnostic("Emby: item " + itemId + (favorite ? " is a favorite" : " is not a favorite"));
    if (favorite != wanted) showNotice(L"Emby did not change the favorite: " + title);
    else showNotice((favorite ? L"\x2665 Added to Emby favorites: " : L"Removed from Emby favorites: ") + title);
    // The favorites page, or a page showing only its favorites, drops what
    // was unmarked, and shows what was marked from its information pages,
    // when it is next kept fresh.
    if (embyListsFavoritesOnly()) embyWantRefresh();
}

void App::embyApplyFavorite(const std::string& itemId, bool favorite) {
    const auto mark = [&](std::vector<emby::Item>& items) {
        for (auto& item : items) {
            if (item.id == itemId) item.favorite = favorite;
        }
    };
    mark(embyItems_);
    mark(embyResume_);
    if (embyDetails_) {
        if (embyDetails_->item.id == itemId) embyDetails_->item.favorite = favorite;
        mark(embyDetails_->sourceItems);
        mark(embyDetails_->seasons);
        mark(embyDetails_->episodes);
    }
    for (auto& slot : embyPanes_) {
        if (!slot) continue;
        if (slot->itemId == itemId) slot->favorite = favorite;
        mark(slot->queue.items);
    }
    if (embyPendingPlay_) {
        if (embyPendingPlay_->item.id == itemId) embyPendingPlay_->item.favorite = favorite;
        mark(embyPendingPlay_->queue.items);
    }
}

std::optional<emby::Item> App::embyItemAtHit(const PanelHit& hit) const {
    if (!settingsOpen_ || !embyBrowserOpen_ || browserSource_ != BrowserSource::Emby || embyPendingPlay_ ||
        !emby_.session().signedIn()) {
        return std::nullopt;
    }
    if (hit.row < 0 || static_cast<std::size_t>(hit.row) >= panelRows_.size()) return std::nullopt;
    const auto& row = panelRows_[static_cast<std::size_t>(hit.row)];
    if (!row.enabled) return std::nullopt;
    // The home page's libraries are listed as items too; the favorites
    // page would never show one marked.
    const auto offered = [](const emby::Item& item) -> std::optional<emby::Item> {
        if (!emby::listedAmongFavorites(item)) return std::nullopt;
        return item;
    };
    if (row.id == SettingId::EmbyItem) {
        const bool onItem = hit.kind == PanelHitKind::Item || hit.kind == PanelHitKind::Add ||
                            hit.kind == PanelHitKind::Tile;
        if (!onItem || embyDetails_) return std::nullopt;
        // A tile outside the line's would read as -1, the first resume item.
        if (row.kind == PanelRowKind::Tiles &&
            (hit.part < 0 || static_cast<std::size_t>(hit.part) >= row.tileParams.size())) {
            return std::nullopt;
        }
        // Resume items count down from -1, the list's up from 0, as the
        // rows were built.
        const int param = panelRowParam(row, hit.part);
        const auto& list = param < 0 ? embyResume_ : embyItems_;
        const std::size_t index = static_cast<std::size_t>(param < 0 ? -param - 1 : param);
        if (index >= list.size()) return std::nullopt;
        return offered(list[index]);
    }
    if (!embyDetails_ || hit.kind != PanelHitKind::Button) return std::nullopt;
    if (row.id == SettingId::EmbyDetailAction) return offered(embyDetails_->item);
    if (row.id == SettingId::EmbyDetailEpisode && row.param >= 0 &&
        static_cast<std::size_t>(row.param) < embyDetails_->episodes.size()) {
        return offered(embyDetails_->episodes[static_cast<std::size_t>(row.param)]);
    }
    return std::nullopt;
}

std::optional<emby::Item> App::embyItemAt(POINT local) const {
    PanelHit hit = panelHitAt(local);
    // An information page's title and its episodes answer anywhere on their
    // rows -- the poster, the synopsis, an episode's name where its heart
    // is -- not only on their buttons, which are all the hit test names.
    const float x = static_cast<float>(local.x);
    const float y = static_cast<float>(local.y);
    if (hit.kind == PanelHitKind::Sheet && embyDetails_ && panelLayout_.content.contains(x, y)) {
        for (std::size_t i = 0; i < panelLayout_.rows.size() && i < panelRows_.size(); ++i) {
            if (!panelLayout_.rows[i].row.contains(x, y)) continue;
            if (panelRows_[i].id == SettingId::EmbyDetailAction || panelRows_[i].id == SettingId::EmbyDetailEpisode) {
                hit = {PanelHitKind::Button, static_cast<int>(i), -1};
            }
            break;
        }
    }
    return embyItemAtHit(hit);
}

void App::showEmbyItemMenu(POINT local) {
    // A copy: the lists can be replaced by a reply while the menu is up.
    const auto item = embyItemAt(local);
    if (!item || item->id.empty()) return;
    const auto account = embyAccountSerial_;
    PanelMenu menu;
    menu.option(CmdEmbyFavorite, item->favorite ? L"Remove from favorites" : L"Add to favorites");
    menu.items.back().enabled = !embyFavoriteAsking_.contains(item->id);
    // A favorite is never deleted: the entry says so rather than leave.
    if (embyDeleteEnabled_ && emby::deletableType(*item)) {
        menu.separator();
        menu.option(CmdEmbyDelete,
                    item->favorite ? L"Delete from server  (a favorite)" : L"Delete from server\x2026", false, {},
                    !item->favorite && !embyPendingDelete_);
    }
    openPanelMenuAt(std::move(menu), local, [this, account, item = *item](unsigned command) {
        // What the menu offered, for the account it was offered under.
        if (account != embyAccountSerial_) return;
        if (command == CmdEmbyFavorite) embySetFavorite(item.id, wide(emby::displayTitle(item)), !item.favorite);
        if (command == CmdEmbyDelete) embyAskDelete(item);
    });
}

void App::embyActivateItem(int param) {
    if (embyLoading_ && !embyPages_.empty() && embyPages_.back().kind == EmbyPage::Kind::Search) return;
    const emby::Item* pointer = nullptr;
    if (param < 0) {
        const std::size_t index = static_cast<std::size_t>(-param - 1);
        if (index < embyResume_.size()) pointer = &embyResume_[index];
    } else if (static_cast<std::size_t>(param) < embyItems_.size()) {
        pointer = &embyItems_[static_cast<std::size_t>(param)];
    }
    if (!pointer) return;
    // A copy: the lists are replaced by the request the choice starts.
    const emby::Item entry = *pointer;
    if (embyTryOpenDetails(entry, param)) return;
    if (emby::isPlayableVideo(entry) || emby::isPhoto(entry)) {
        // The list it was chosen from, as shown, is what Page Up/Down walk:
        // a playlist, a search's results or a sorted folder, not the
        // server's order.
        const bool resume = param < 0;
        embyRequestPlay(entry, emby::takeQueue(resume ? embyResume_ : embyItems_, resume ? -param - 1 : param,
            resume || embyPages_.empty() ? std::string("resume") : embyPageKey(embyPages_.back())));
        return;
    }
    EmbyPage page;
    page.id = entry.id;
    page.title = emby::displayTitle(entry);
    page.itemType = entry.type;
    if (emby::isPlaylist(entry)) {
        page.kind = EmbyPage::Kind::Playlist;
    } else if (entry.type == "Series") {
        page.kind = EmbyPage::Kind::Series;
    } else if (entry.type == "Season") {
        page.kind = EmbyPage::Kind::Season;
        page.seriesId = entry.seriesId.empty() ? entry.parentId : entry.seriesId;
        if (page.seriesId.empty() && !embyPages_.empty() && embyPages_.back().kind == EmbyPage::Kind::Series) {
            page.seriesId = embyPages_.back().id;
        }
    } else if (entry.type == "CollectionFolder" || entry.type == "UserView") {
        page.kind = EmbyPage::Kind::Library;
        page.collectionType = entry.collectionType;
    } else if (emby::isBrowsable(entry)) {
        page.kind = EmbyPage::Kind::Folder;
    } else {
        return;
    }
    embyPushPage(std::move(page));
}

}  // namespace quaddeck
