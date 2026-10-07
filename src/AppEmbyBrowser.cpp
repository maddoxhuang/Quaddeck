// App: the Emby library browser drawn in the settings sheet. Its rows and
// tiles, pages, views, sort and filters, search, pictures, playlists, and
// what activating an item does. Replies arrive on the window thread
// through EmbyClient::pump(), as described in AppEmby.cpp.

#include "App.hpp"
#include "AppInternal.hpp"
#include "Diagnostics.hpp"
#include "FilePersistence.hpp"
#include "TextEncoding.hpp"
#include "resource.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <locale>
#include <set>
#include <sstream>
#include <string_view>
#include <thread>

namespace quaddeck {

using namespace app_internal;

namespace {

constexpr int kEmbyPageSize = 300;
// A playlist is fetched whole, this many entries a request, because it is
// a list to play through; one longer than the limit stops there.
constexpr int kEmbyPlaylistPageSize = 1000;
constexpr int kEmbyPlaylistLimit = 20000;
constexpr int kEmbyEpisodeAutoLimit = 20000;
constexpr int kEmbySearchPerLibrary = 200;
// Tiles at 96 DPI and the widths their pictures are asked for: twice the
// tile, so a 200 % display still gets a sharp picture from one fetch.
constexpr float kEmbyPosterTile = 150.0F;
constexpr float kEmbyThumbTile = 220.0F;
constexpr int kEmbyPosterImageWidth = 300;
constexpr int kEmbyThumbImageWidth = 440;
// Pictures queued at once; scrolling past a line does not queue its
// pictures for good.
constexpr std::size_t kEmbyImageQueue = 24;
constexpr const char* kEmbyListFields = emby::kBrowserItemFields;

std::wstring wide(const std::string& text) { return utf8ToWideText(text); }
std::string narrow(const std::wstring& text) { return wideToUtf8Text(text); }

INT_PTR CALLBACK searchProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dialog, GWLP_USERDATA, lParam);
        auto* term = reinterpret_cast<std::wstring*>(lParam);
        SetDlgItemTextW(dialog, IDC_EMBY_SEARCH_TERM, term->c_str());
        SetFocus(GetDlgItem(dialog, IDC_EMBY_SEARCH_TERM));
        SendDlgItemMessageW(dialog, IDC_EMBY_SEARCH_TERM, EM_SETSEL, 0, -1);
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            auto* term = reinterpret_cast<std::wstring*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
            if (term) *term = dialogText(dialog, IDC_EMBY_SEARCH_TERM);
            EndDialog(dialog, IDOK);
            return TRUE;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    default:
        break;
    }
    return FALSE;
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

std::vector<PanelRow> App::buildEmbyRows(float clientWidth) const {
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
        rows.push_back(std::move(row));
    };
    const auto buttons = [&](SettingId id, std::vector<std::wstring> captions) {
        PanelRow row;
        row.kind = PanelRowKind::Buttons;
        row.id = id;
        row.options = std::move(captions);
        rows.push_back(std::move(row));
    };
    const auto toggle = [&](SettingId id, std::wstring label, bool on) {
        PanelRow row;
        row.kind = PanelRowKind::Toggle;
        row.id = id;
        row.label = std::move(label);
        row.on = on;
        rows.push_back(std::move(row));
    };
    const auto choice = [&](SettingId id, std::wstring label, std::vector<std::wstring> options, int selected,
                            int perLine) {
        PanelRow row;
        row.kind = PanelRowKind::Choice;
        row.id = id;
        row.label = std::move(label);
        row.options = std::move(options);
        row.selected = selected;
        row.segmentsPerLine = perLine;
        rows.push_back(std::move(row));
    };
    // The item being played, marked wherever it appears.
    std::set<std::string> nowPlaying;
    for (std::size_t pane = 0; pane < embyPanes_.size(); ++pane) {
        if (embyPaneAccountMatches(pane) && !embyPanes_[pane]->itemId.empty()) nowPlaying.insert(embyPanes_[pane]->itemId);
    }
    const auto item = [&](int param, const emby::Item& entry) {
        PanelRow row;
        row.kind = PanelRowKind::Item;
        row.id = SettingId::EmbyItem;
        row.param = param;
        row.label = itemLabel(entry);
        row.value = itemValue(entry);
        row.on = entry.positionTicks > 0 && !entry.played;
        if (embyHasSubtitles(entry)) row.tag = L"Sub";
        row.current = nowPlaying.contains(entry.id);
        row.addAvailable = emby::isPlayableVideo(entry);
        rows.push_back(std::move(row));
    };
    // A list as tiles: as many per line as the sheet is wide, every line
    // one row so the sheet scrolls by lines and a hit names one tile.
    const bool posters = embyBrowser_.view == 1;
    const PanelMetrics metrics;
    const float innerWidth = std::max(0.0F, clientWidth - 2.0F * metrics.pad * uiScale_);
    // As many tiles as fit a line, then stretched or shrunk to fill it: a
    // docked sheet shows one wide thumbnail or two posters per line rather
    // than one nominal tile and a gap.
    const int perLine = panelTilesPerLine(innerWidth, posters ? kEmbyPosterTile : kEmbyThumbTile, uiScale_, metrics);
    const float tileWidth = panelTileWidthFor(innerWidth, perLine, uiScale_, metrics);
    // Pictures at twice the tile, rounded up to a hundred so a small change
    // of window width does not refetch them all.
    const int imageWidth = std::max(posters ? kEmbyPosterImageWidth : kEmbyThumbImageWidth,
                                    static_cast<int>(std::ceil(tileWidth * uiScale_ * 2.0F / 100.0F)) * 100);
    const auto tiles = [&](const std::vector<emby::Item>& list, int firstParam, int paramStep) {
        for (std::size_t start = 0; start < list.size(); start += static_cast<std::size_t>(perLine)) {
            PanelRow row;
            row.kind = PanelRowKind::Tiles;
            row.id = SettingId::EmbyItem;
            row.tileWidth = tileWidth;
            row.tileAspect = posters ? 2.0F / 3.0F : 16.0F / 9.0F;
            const std::size_t end = std::min(list.size(), start + static_cast<std::size_t>(perLine));
            for (std::size_t i = start; i < end; ++i) {
                const auto& entry = list[i];
                row.options.push_back(itemLabel(entry));
                row.tileKeys.push_back(emby::imageKey(entry, imageWidth));
                row.tileBadges.push_back(tileBadge(entry));
                row.tileMarks.push_back(tileMark(entry));
                row.tileProgress.push_back(progressFraction(entry));
                row.tileTags.push_back(embyHasSubtitles(entry) ? L"Sub" : L"");
                row.tileParams.push_back(firstParam + static_cast<int>(i) * paramStep);
                row.tileAddAvailable.push_back(emby::isPlayableVideo(entry));
                if (nowPlaying.contains(entry.id)) row.selected = static_cast<int>(i - start);
            }
            rows.push_back(std::move(row));
        }
    };
    const auto listing = [&](const std::vector<emby::Item>& list, int firstParam, int paramStep) {
        if (embyBrowser_.view != 0) {
            tiles(list, firstParam, paramStep);
            return;
        }
        for (std::size_t i = 0; i < list.size(); ++i) {
            item(firstParam + static_cast<int>(i) * paramStep, list[i]);
        }
    };

    if (!emby_.session().signedIn()) {
        // The sign-in page: the sheet stays the browser, the dialog takes
        // the address and the password.
        header(L"Emby");
        note(embyStatusLine());
        buttons(SettingId::EmbyAccount, {L"Sign in\x2026"});
        return rows;
    }

    const bool home = embyPages_.size() <= 1;
    std::wstring crumb = L"Emby";
    std::size_t first = 1;
    if (embyPages_.size() > 3) {
        crumb += L" \x203A \x2026";
        first = embyPages_.size() - 2;
    }
    for (std::size_t i = first; i < embyPages_.size(); ++i) {
        const auto& page = embyPages_[i];
        crumb += L" \x203A " + (page.kind == EmbyPage::Kind::Search ? L"\x201C" + wide(page.term) + L"\x201D"
                                                                     : wide(page.title));
    }
    header(crumb);
    // The way back to the settings is the header's switch, so the home
    // page's row starts at Search; the other pages lead with Back.
    if (home) {
        buttons(SettingId::EmbyNav, {L"Search\x2026", L"Sign out", L"Refresh"});
    } else {
        buttons(SettingId::EmbyNav, {L"\x2190 Back", L"Search\x2026", L"Home", L"Refresh"});
    }
    choice(SettingId::EmbyView, L"View", {L"List", L"Posters", L"Thumbnails"}, embyBrowser_.view, 3);
    note(L"Play / Resume target: " + embyTargetLabel());
    note(L"+ Add: new pane, muted; resumes saved progress.");
    note(L"Click a video pane to change the play target.");
    const std::string libraryId = embyCurrentLibraryId();
    const std::string libraryType = embyLibraryType(libraryId);
    if (libraryType == "movies" || libraryType == "tvshows") {
        toggle(SettingId::EmbyLibraryDetails, L"Information pages",
               embyLibraryDetailsEnabled(embyLibraries_, emby_.session().serverId, libraryId, libraryType));
    }
    // Seasons and episodes come in the server's own order; only pages
    // asked through /Items take a sort and a filter.
    if (!home && embyPageAcceptsFilters()) {
        // A playlist has an order of its own, which comes first on its page
        // and is what it opens in.
        const bool playlist = embyPages_.back().kind == EmbyPage::Kind::Playlist;
        const bool ownOrder = playlist && embyPages_.back().ownOrder;
        // Updated says what it goes by, first in the line where a docked
        // sheet still shows it: what the server found or changed last.
        const bool updated = emby::sortKeyFromIndex(embyBrowser_.sort) == emby::SortKey::Updated;
        const std::wstring order = ownOrder ? L"the playlist's own order"
                                 : embyBrowser_.sort == 4 ? L"in no particular order"
                                 : updated ? (embyBrowser_.descending
                                                  ? L"what the server found or changed last comes first"
                                                  : L"what the server found or changed last comes last")
                                 : embyBrowser_.descending ? L"descending  (choose it again to reverse)"
                                                           : L"ascending  (choose it again to reverse)";
        std::vector<std::wstring> sortNames;
        if (playlist) sortNames.push_back(L"Playlist");
        for (int i = 0; i < emby::kSortKeyCount; ++i) sortNames.push_back(emby::sortKeyName(emby::sortKeyFromIndex(i)));
        const int sortCount = static_cast<int>(sortNames.size());
        // All on one line when the sheet is wide; a docked sheet wraps them.
        choice(SettingId::EmbySort, L"Sort by \x2014 " + order + L"   (Ctrl+4 name, 6 size, 7 added, 8 length, 9 random)",
               std::move(sortNames), ownOrder ? 0 : embyBrowser_.sort + (playlist ? 1 : 0),
               panelSegmentsPerLine(innerWidth, uiScale_, sortCount, 64.0F, metrics));
        if (embyPageAcceptsFlat()) {
            // By folder as the files lie on the server, or every video
            // under the page as one list with the folders left out.
            choice(SettingId::EmbyFlat, L"Show", {L"Folders", L"All videos"}, embyBrowser_.flat ? 1 : 0, 2);
        }
        toggle(SettingId::EmbyUnplayed, L"Unplayed only", embyBrowser_.unplayed);
    }
    if (embyLoading_) note(L"Loading\x2026");
    else if (!embyNote_.empty()) note(embyNote_);
    if (home) {
        note(embyStatusLine());
        if (!embyResume_.empty()) {
            header(L"Continue watching");
            listing(embyResume_, -1, -1);
        }
        header(L"Libraries");
    }
    listing(embyItems_, 0, 1);
    if (embyLoading_ && !embyPages_.empty() && embyPages_.back().kind == EmbyPage::Kind::Search) {
        // All libraries must finish before an item's provenance or final
        // position is known. Opening a partial result would retire the
        // remaining replies and leave Back with an incomplete search.
        for (auto& row : rows) if (row.id == SettingId::EmbyItem) row.enabled = false;
    }
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
    return rows;
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
    const bool busy = controlDrag_ != ControlDrag::None || draggingPane_ || pressedBarItem_ >= 0 ||
                      pressedPanelKind_ != PanelHitKind::None;
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
        embySignInDialog();
        return;
    }
    if (part == 0) embyOpenBrowser();
    else embySignOut();
}

void App::embyNavigate(int part) {
    if (embyPendingPlay_) { embyCancelReplacement(); return; }
    // The home page's row has no Back, so its parts are one short of the
    // other pages'; the cases below are numbered by the longer row.
    if (embyPages_.size() <= 1) ++part;
    if (embyDetails_) {
        if (part == 0) { embyCloseDetails(); return; }
        if (part == 3) { embyRequestDetails(); return; }
        embyCloseDetails();
        if (part == 2) { embyGoHome(); return; }
    }
    switch (part) {
    case 0:
        if (embyPages_.size() > 1) {
            embyPages_.pop_back();
            // Back to where that page was left, once it has arrived.
            embyPendingScroll_ = embyPages_.back().scroll;
            embyRequestPage();
        }
        break;
    case 1:
        embySearchDialog();
        break;
    case 2:
        // The home page's third button signs out; every other page's goes home.
        if (embyPages_.size() <= 1) embySignOut();
        else embyGoHome();
        break;
    case 3:
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
           kind == EmbyPage::Kind::Series || kind == EmbyPage::Kind::Season;
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
           kind == EmbyPage::Kind::Playlist || kind == EmbyPage::Kind::Search;
}

std::string App::embyPageKey(const EmbyPage& page) {
    switch (page.kind) {
    case EmbyPage::Kind::Library: return "library:" + page.id;
    case EmbyPage::Kind::Folder: return "folder:" + page.id;
    case EmbyPage::Kind::Playlist: return "playlist:" + page.id;
    case EmbyPage::Kind::Series: return "series:" + page.id;
    case EmbyPage::Kind::Season: return "season:" + page.seriesId + "/" + page.id;
    case EmbyPage::Kind::Search: return "search:" + page.term;
    default: return "home";
    }
}

emby::ListPage App::embyListPage(const EmbyPage& page) {
    emby::ListPage list;
    list.kind = page.kind == EmbyPage::Kind::Library ? emby::ListKind::Library
              : page.kind == EmbyPage::Kind::Playlist ? emby::ListKind::Playlist
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
    return emby::itemsPath(userId,
                           emby::listQuery(embyListPage(page), arrangement, startIndex, limit, kEmbyListFields));
}

std::string App::embySearchPath(const std::string& libraryId, const std::string& term) const {
    emby::ItemsQuery query;
    query.parentId = libraryId;
    query.recursive = true;
    query.includeTypes = "Series,Movie,Episode,Video,Photo";
    query.searchTerm = term;
    query.limit = kEmbySearchPerLibrary;
    query.fields = kEmbyListFields;
    query.filters = embyBrowser_.unplayed ? "IsUnplayed" : "";
    return emby::itemsPath(emby_.session().userId, query);
}

void App::embySetView(int view) {
    view = std::clamp(view, 0, 2);
    if (view == embyBrowser_.view) return;
    embyBrowser_.view = view;
    settingsScroll_ = 0.0F;
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
        ++embyRequestSerial_;
        embyItems_.clear();
        embyTotal_ = 0;
        embyLoading_ = embyLoadingMore_ = embyRefreshing_ = embyRefreshQuiet_ = false;
        embyListAskedTick_ = 0;
        embyPendingScroll_ = -1.0F;
    }
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
    for (std::size_t i = 0; i < panelRows_.size() && i < panelLayout_.rows.size(); ++i) {
        const auto& row = panelRows_[i];
        if (row.kind != PanelRowKind::Tiles && row.kind != PanelRowKind::MediaDetail) continue;
        const auto& box = panelLayout_.rows[i].row;
        if (box.bottom() < content.y - margin || box.y > content.bottom() + margin) continue;
        const std::vector<std::string> keys = row.kind == PanelRowKind::MediaDetail
            ? std::vector<std::string>{row.mediaDetail.posterKey, row.mediaDetail.backdropKey, row.mediaDetail.logoKey}
            : row.tileKeys;
        for (const auto& key : keys) {
            if (key.empty() || thumbnails_.contains(key) || embyImagesInFlight_.count(key)) continue;
            if (embyImagesInFlight_.size() >= kEmbyImageQueue) return;
            if (const auto local = parseLocalThumbnailKey(key)) {
                // A local file's picture is the shell's, fetched off this thread.
                embyImagesInFlight_.insert(key);
                localThumbnails_.request(local->path, local->width, std::max(1, local->width * 9 / 16),
                                         [this, key](const LocalThumbnailer::Result& result) {
                    embyImagesInFlight_.erase(key);
                    if (result.cancelled) return;
                    thumbnails_.insert(key, result.bytes);
                });
                continue;
            }
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
    }
}

void App::embyOpenBrowser() {
    localCancelReplacement();
    if (!emby_.session().signedIn()) {
        // The sign-in continuation opens the browser once the server answers.
        embySignInDialog();
        return;
    }
    if (settingsOpen_) rememberSheetScroll();
    if (!settingsOpen_) toggleSettingsPanel();
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

void App::embyPushPage(EmbyPage page) {
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
    if (embyDetails_) { embyRequestDetails(); return; }
    if (embyPages_.empty()) {
        embyGoHome();
        return;
    }
    thumbnails_.forgetFailures();
    // What has subtitles is asked again with it: a file put beside a video
    // shows once the server has found it.
    embySubtitlesAgain_ = true;
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
        const auto& page = embyPages_.back();
        const bool ownOrder = page.kind == EmbyPage::Kind::Playlist && page.ownOrder;
        const bool random = embyPageAcceptsFilters() && !ownOrder &&
                            emby::sortKeyFromIndex(embyBrowser_.sort) == emby::SortKey::Random;
        state.keepsOrder = page.kind != EmbyPage::Kind::Search && !random;
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
    // the time the scroll reaches it.
    if (settingsScroll_ < panelLayout_.maxScroll - panelLayout_.content.height) return;
    embyRequestPage(static_cast<int>(embyItems_.size()));
}

void App::embySearchDialog() {
    if (!emby_.session().signedIn()) return;
    std::wstring term;
    if (!embyPages_.empty() && embyPages_.back().kind == EmbyPage::Kind::Search) {
        term = wide(embyPages_.back().term);
    }
    const INT_PTR result = DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_EMBY_SEARCH), window_,
                                           searchProcedure, reinterpret_cast<LPARAM>(&term));
    if (result != IDOK) return;
    const auto notSpace = [](wchar_t c) { return !std::iswspace(c); };
    term.erase(term.begin(), std::find_if(term.begin(), term.end(), notSpace));
    term.erase(std::find_if(term.rbegin(), term.rend(), notSpace).base(), term.end());
    if (term.empty()) return;
    EmbyPage page;
    page.kind = EmbyPage::Kind::Search;
    page.term = narrow(term);
    page.title = page.term;
    if (!embyPages_.empty() && embyPages_.back().kind == EmbyPage::Kind::Search) embyPages_.pop_back();
    embyPushPage(std::move(page));
}

void App::embyRequestPage(int startIndex, bool inPlace) {
    if (embyPages_.empty()) embyPages_.push_back(EmbyPage{});
    const EmbyPage page = embyPages_.back();
    const auto session = emby_.session();
    if (!session.signedIn()) {
        embyLoading_ = false;
        embyNote_ = L"Not signed in.";
        return;
    }
    const bool more = startIndex > 0;
    if (!more && !inPlace) {
        embyItems_.clear();
        embyTotal_ = 0;
        settingsScroll_ = 0.0F;
        embyImages_.clearPending();
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
        // In place nothing says "Loading", and a further page says so under
        // the list: a row appearing above it and going would move the list
        // that is meant to stay where it is.
        embyLoading_ = !more;
    }
    const int pageSize = page.kind == EmbyPage::Kind::Playlist ? kEmbyPlaylistPageSize : kEmbyPageSize;
    // In place the whole list shown is asked for again, not its first page.
    int pageLimit = inPlace ? std::max(pageSize, static_cast<int>(embyItems_.size())) : pageSize;
    if (!inPlace && page.kind == EmbyPage::Kind::Season && startIndex < kEmbyEpisodeAutoLimit) {
        pageLimit = std::min(pageLimit, kEmbyEpisodeAutoLimit - startIndex);
    }
    const std::uint64_t serial = ++embyRequestSerial_;
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
        emby_.request("GET", emby::viewsPath(userId), "", [this, serial, listHandler](const EmbyClient::Response& response) {
            listHandler(response);
            if (serial == embyRequestSerial_ && response.ok) {
                embyViews_ = embyItems_;
                // F5 library switches use the view's current row index.
                // A refreshed/reordered Views reply retires any held press.
                panelPressArmed_ = false;
                pressedPanelKind_ = PanelHitKind::None;
                embyResolveResumeLibraries();
            }
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
        // The path builder keeps the library's arrangement separate from
        // a show's server-ordered seasons and episodes.
        emby_.request("GET", embyListPath(page, startIndex, pageLimit), "", listHandler);
        break;
    case EmbyPage::Kind::Search: {
        // The server matches the start of words, and honours a library's
        // "exclude from search" only when asked without a ParentId; asking
        // each library by name finds everything the account can see.
        std::vector<std::string> libraries;
        for (const auto& view : embyViews_) {
            if (view.type == "CollectionFolder" || view.type == "UserView") libraries.push_back(view.id);
        }
        if (libraries.empty()) libraries.push_back({});
        auto remaining = std::make_shared<std::size_t>(libraries.size());
        auto seen = std::make_shared<std::set<std::string>>();
        for (const auto& library : libraries) {
            emby_.request("GET", embySearchPath(library, page.term), "",
                          [this, serial, remaining, seen, sortKey, library](const EmbyClient::Response& response) {
                if (serial != embyRequestSerial_) return;
                if (response.ok) {
                    if (auto list = emby::parseItems(response.body)) {
                        for (auto& entry : list->items) {
                            embyRememberLibrary(entry, library);
                            if (seen->insert(entry.id).second) embyItems_.push_back(std::move(entry));
                        }
                    }
                } else if (response.status == 401) {
                    embySessionExpired();
                    return;
                }
                if (--*remaining == 0) {
                    embyLoading_ = false;
                    // The libraries answered one by one; the list is ordered
                    // here the way one library's reply would have been.
                    emby::sortItems(embyItems_, sortKey, embyBrowser_.descending);
                    embyAdoptPlaylist();
                    embyAskSubtitles(embyItems_, true);
                    embyTotal_ = static_cast<int>(embyItems_.size());
                    embyNote_ = embyItems_.empty()
                        ? L"No title starts a word with that."
                        : L"Matches the start of a word in a title.";
                }
            });
        }
        break;
    }
    }
}

void App::embyListArrived(std::uint64_t serial, int startIndex, const EmbyClient::Response& response) {
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
    auto list = emby::parseItems(response.body);
    if (!list) {
        embyAutoMoreBlocked_ = more;
        if (!quiet) embyNote_ = L"The server sent a reply QuadDeck cannot read.";
        return;
    }
    const bool nothingNew = list->items.empty();
    const std::string libraryId = embyCurrentLibraryId();
    for (const auto& entry : list->items) embyRememberLibrary(entry, libraryId);
    const std::string pressed = embyPressedItem();
    // A list looked at anew asks again what has subtitles; one kept fresh
    // unasked asks only about videos it had not shown.
    const bool askAgain = !refreshed || embySubtitlesAgain_;
    embySubtitlesAgain_ = false;
    embyAskSubtitles(list->items, askAgain);
    if (more) {
        embyItems_.insert(embyItems_.end(), list->items.begin(), list->items.end());
        if (nothingNew) embyAutoMoreBlocked_ = true;
    } else {
        if (quiet) {
            // The log says once for a page that it is kept fresh, and
            // then whenever the server had news for it.
            const std::string page = embyPages_.empty() ? std::string() : embyPageKey(embyPages_.back());
            const bool news = !emby::sameListing(embyItems_, list->items);
            if (news || page != embyRefreshLoggedPage_) {
                appendDiagnostic("Emby: asked again for " + page + ", " + std::to_string(list->items.size()) +
                                 (news ? " items, changed on the server" : " items, nothing new"));
            }
            embyRefreshLoggedPage_ = page;
        }
        embyItems_ = std::move(list->items);
    }
    embyKeepPressOn(pressed);
    embyTotal_ = std::max(list->total, static_cast<int>(embyItems_.size()));
    // A page that has come back says nothing of the failure before it.
    if (embyItems_.empty()) embyNote_ = L"Nothing here.";
    else if (refreshed) embyNote_.clear();
    // Asked for again in place, the page brings its new entries to the
    // list being played but does not re-order it; asked for anew -- another
    // order, a filter, a further page -- it is the list.
    embyAdoptPlaylist();
    if (embyPendingScroll_ >= 0.0F) {
        settingsScroll_ = embyPendingScroll_;
        embyPendingScroll_ = -1.0F;
    }
    // A playlist or a season is a sequence to play through: fetch the rest
    // without needing a scroll, so paging does not cut episode navigation
    // off at the first 300. Keep the existing bounded-list policy.
    const int held = static_cast<int>(embyItems_.size());
    const bool playlist = !embyPages_.empty() && embyPages_.back().kind == EmbyPage::Kind::Playlist;
    const bool season = !embyPages_.empty() && embyPages_.back().kind == EmbyPage::Kind::Season;
    const int sequenceLimit = season ? kEmbyEpisodeAutoLimit : kEmbyPlaylistLimit;
    if ((playlist || season) && !nothingNew && held < embyTotal_ && held < sequenceLimit) {
        embyRequestPage(held);
    }
}

void App::embyAskSubtitles(const std::vector<emby::Item>& items, bool again) {
    const auto session = emby_.session();
    if (!session.signedIn()) return;
    const auto ids = emby::subtitleQuestions(items, embySubtitled_, embySubtitleAsking_, again);
    for (auto& batch : emby::subtitleBatches(ids)) {
        for (const auto& id : batch) embySubtitleAsking_.insert(id);
        const std::string path = emby::subtitledItemsPath(session.userId, batch);
        emby_.request("GET", path, "",
                      [this, account = embyAccountSerial_, asked = std::move(batch)](const EmbyClient::Response& response) {
            embySubtitlesArrived(account, asked, response);
        });
    }
}

void App::embySubtitlesArrived(std::uint64_t account, const std::vector<std::string>& asked,
                               const EmbyClient::Response& response) {
    if (account != embyAccountSerial_) return;
    for (const auto& id : asked) embySubtitleAsking_.erase(id);
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
