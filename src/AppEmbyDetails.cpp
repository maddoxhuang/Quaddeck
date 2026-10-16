// Emby information pages, their cast and crew, and a person's page. Browsing
// details is read-only; the explicit play actions alone adopt a playback
// sequence and enter the existing player.
#include "App.hpp"
#include "AppInternal.hpp"
#include "Diagnostics.hpp"
#include "TextEncoding.hpp"

#include <algorithm>
#include <iomanip>
#include <memory>
#include <sstream>

namespace quaddeck {
using namespace app_internal;
namespace {
constexpr int kDetailPageSize = 300;
constexpr int kDetailSequenceLimit = 20000;
// Library lookups out at once. The server's requests go one at a time
// through one queue, so more would not answer sooner, only keep a click
// waiting behind them.
constexpr std::size_t kLibraryLookupsAtOnce = 4;
// Titles whose library is remembered, and those it could not be found for.
constexpr std::size_t kRememberedLibraries = 20000;
// A cast and crew tile at 96 DPI, a portrait like a poster but smaller, and
// how many of them show before "All cast & crew": at least this many, in
// whole lines, so a narrow docked sheet still shows a few faces.
constexpr float kEmbyCastTile = 130.0F;
constexpr std::size_t kEmbyCastShown = 4;

void addPart(std::wstring& text, const std::wstring& part, const wchar_t* separator = L"  \u00B7  ") {
    if (part.empty()) return;
    if (!text.empty()) text += separator;
    text += part;
}

std::wstring joined(const std::vector<std::string>& values, std::size_t limit = 12) {
    std::wstring result;
    for (std::size_t i = 0; i < values.size() && i < limit; ++i) {
        addPart(result, utf8ToWideText(values[i]), L", ");
    }
    return result;
}

std::wstring rating(double value) {
    std::wostringstream text;
    text.imbue(std::locale::classic());
    text << std::fixed << std::setprecision(1) << value;
    return text.str();
}

std::wstring detailEpisodeTitle(const emby::Item& item) {
    std::wstring result = utf8ToWideText(emby::episodeCode(item));
    addPart(result, utf8ToWideText(item.name));
    if (result.empty()) result = L"Episode";
    if (item.runTimeTicks > 0) addPart(result, formatTime(emby::secondsFromTicks(item.runTimeTicks)));
    return result;
}

bool hasResume(const emby::Item& item) {
    const double position = emby::secondsFromTicks(item.positionTicks);
    const double duration = emby::secondsFromTicks(item.runTimeTicks);
    return position > 5.0 && (duration <= 0.0 || position < duration - 5.0);
}

// Under a person's picture: the name, and on the second line the part an
// actor plays, or what the rest of the crew did.
std::wstring creditCaption(const emby::Person& person) {
    std::wstring caption = utf8ToWideText(person.name);
    std::string part = person.role;
    if (part.empty() && !emby::isCast(person)) part = person.type;
    if (part.empty() && person.type == "GuestStar") part = "Guest star";
    if (!part.empty()) caption += L"\n" + utf8ToWideText(part);
    return caption;
}
}

std::string App::embyCurrentLibraryId() const {
    if (embyDetails_) return embyDetails_->libraryId;
    for (auto page = embyPages_.rbegin(); page != embyPages_.rend(); ++page) {
        // A search, the favorites and a person's titles list every library
        // at once: what was opened from them is not known to be in the
        // library beneath.
        if (page->kind == EmbyPage::Kind::Search || page->kind == EmbyPage::Kind::Favorites ||
            page->kind == EmbyPage::Kind::Person) {
            return {};
        }
        if (page->kind == EmbyPage::Kind::Library) return page->id;
    }
    return {};
}

std::string App::embyLibraryType(const std::string& libraryId) const {
    if (libraryId.empty()) return {};
    for (const auto& library : embyViews_) {
        if (library.id == libraryId && (library.type == "CollectionFolder" || library.type == "UserView")) {
            return library.collectionType;
        }
    }
    return {};
}

void App::embyRememberLibrary(const emby::Item& item, const std::string& libraryId) {
    if (item.id.empty() || libraryId.empty()) return;
    if (embyItemLibraries_.size() >= kRememberedLibraries && !embyItemLibraries_.contains(item.id)) return;
    embyItemLibraries_[item.id].insert(libraryId);
}

std::string App::embyItemLibraryId(const emby::Item& item) const {
    const auto current = embyCurrentLibraryId();
    if (!current.empty()) return current;
    if (!embyLibraryType(item.parentId).empty()) return item.parentId;
    const auto found = embyItemLibraries_.find(item.id);
    return found != embyItemLibraries_.end() && found->second.size() == 1 ? *found->second.begin() : std::string();
}

void App::embyResolveResumeLibraries() { embyResolveLibraries(embyResume_); }

void App::embyResolveShownLibraries() {
    if (embyPages_.size() <= 1) embyResolveLibraries(embyResume_);
    else if (embyPages_.back().kind == EmbyPage::Kind::Favorites || embyPages_.back().kind == EmbyPage::Kind::Person) {
        embyResolveLibraries(embyItems_);
    }
}

void App::embyResolveLibraries(const std::vector<emby::Item>& items) {
    if (!emby_.session().signedIn() || embyViews_.empty()) return;
    for (const auto& item : items) {
        if (item.type != "Movie" && item.type != "Series") continue;
        if (!embyLibraryType(item.parentId).empty()) { embyRememberLibrary(item, item.parentId); continue; }
        if (embyItemLibraries_.contains(item.id) || embyLibraryLookups_.contains(item.id) ||
            embyLibraryUnknown_.contains(item.id)) {
            continue;
        }
        // A long favorites list asks a few at a time, so a click queues
        // behind no more than those; the rest keep the original browsing
        // path until an answer frees a slot.
        if (embyLibraryLookups_.size() >= kLibraryLookupsAtOnce) break;
        embyLibraryLookups_.insert(item.id);
        const auto account = embyAccountSerial_;
        emby_.request("GET", emby::ancestorsPath(item.id, emby_.session().userId), "",
                      [this, item, account](const EmbyClient::Response& response) {
            embyLibraryLookupArrived(account, item, response);
        });
    }
}

void App::embyLibraryLookupArrived(std::uint64_t account, const emby::Item& item,
                                   const EmbyClient::Response& response) {
    if (account != embyAccountSerial_) return;
    embyLibraryLookups_.erase(item.id);
    if (response.status == 401) { embySessionExpired(); return; }
    const auto ancestors = response.ok ? emby::parseAncestorsArray(response.body) : std::nullopt;
    const auto library = ancestors ? emby::uniqueAncestorLibraryId(*ancestors, embyViews_) : std::string();
    // Unknown, a title stays on the original browsing path and is not
    // asked about on every reply, where it would hold a slot a title that
    // can be placed needs.
    if (library.empty()) {
        if (embyLibraryUnknown_.size() < kRememberedLibraries) embyLibraryUnknown_.insert(item.id);
    } else {
        embyRememberLibrary(item, library);
    }
    // The slot goes to the next title of the list shown. An answer that
    // could not be kept is not asked for again in a loop.
    if (embyItemLibraries_.contains(item.id) || embyLibraryUnknown_.contains(item.id)) {
        embyResolveShownLibraries();
    }
}

void App::embyToggleLibraryDetails(const std::string& libraryId) {
    const auto library = libraryId.empty() ? embyCurrentLibraryId() : libraryId;
    const auto type = embyLibraryType(library);
    if (type != "movies" && type != "tvshows") return;
    const auto& server = emby_.session().serverId;
    const bool enabled = embyLibraryDetailsEnabled(embyLibraries_, server, library, type);
    if (!setEmbyLibraryDetailsEnabled(embyLibraries_, server, library, !enabled)) {
        showNotice(L"Cannot save another library preference");
        return;
    }
    if (enabled && embyDetails_ && embyDetails_->libraryId == library) embyCloseDetails();
    scheduleAppSettingsSave();
}

bool App::embyTryOpenDetails(const emby::Item& item, int param) {
    if (item.type != "Movie" && item.type != "Series") return false;
    const auto library = embyItemLibraryId(item);
    if (!embyLibraryDetailsEnabled(embyLibraries_, emby_.session().serverId, library, embyLibraryType(library))) {
        return false;
    }
    // Freeze the source list. A queued page/refresh may finish, but cannot
    // change what Back returns to or reorder a sequence behind the detail.
    ++embyRequestSerial_;
    embyLoading_ = embyLoadingMore_ = embyRefreshing_ = embyRefreshQuiet_ = false;
    EmbyDetails detail;
    detail.item = item;
    detail.libraryId = library;
    detail.sourceScroll = settingsScroll_;
    detail.sourceItems = param < 0 ? embyResume_ : embyItems_;
    detail.sourceChosen = param < 0 ? -param - 1 : param;
    detail.sourcePage = param < 0 || embyPages_.empty() ? "resume" : embyPageKey(embyPages_.back());
    embyDetails_ = std::move(detail);
    settingsScroll_ = 0.0F;
    panelPressArmed_ = false;
    pressedPanelKind_ = PanelHitKind::None;
    embyRequestDetails();
    return true;
}

void App::embyCloseDetails() {
    ++embyDetailsSerial_;
    ++embyDetailChildrenSerial_;
    if (!embyDetails_) return;
    embyListScroll_ = embyDetails_->sourceScroll;
    if (settingsOpen_ && embyBrowserOpen_ && browserSource_ == BrowserSource::Emby) {
        settingsScroll_ = embyListScroll_;
    }
    embyDetails_.reset();
    panelPressArmed_ = false;
    pressedPanelKind_ = PanelHitKind::None;
}

void App::embyRequestDetails() {
    if (!embyDetails_ || !emby_.session().signedIn()) return;
    thumbnails_.forgetFailures();
    auto& detail = *embyDetails_;
    detail.loading = true;
    detail.error.clear();
    detail.serial = ++embyDetailsSerial_;
    const auto serial = detail.serial;
    emby_.request("GET", emby::itemDetailsPath(emby_.session().userId, detail.item.id), "",
                  [this, serial](const EmbyClient::Response& response) { embyDetailsArrived(serial, response); });
    if (detail.item.type == "Series") {
        detail.preferredSeasonId = detail.seasonId;
        detail.seasonId.clear();
        detail.episodes.clear();
        embyRequestDetailChildren();
    }
}

void App::embyDetailsArrived(std::uint64_t serial, const EmbyClient::Response& response) {
    if (!embyDetails_ || embyDetails_->serial != serial || embyDetailsSerial_ != serial) return;
    auto& detail = *embyDetails_;
    detail.loading = false;
    if (response.status == 401) { embySessionExpired(); return; }
    const auto item = response.ok ? emby::parseItem(response.body) : std::nullopt;
    if (!item || item->id != detail.item.id || item->type != detail.item.type) {
        detail.error = L"Details could not be loaded. Choose Refresh to try again.";
        return;
    }
    const std::string entryId = detail.item.playlistItemId;
    detail.item = *item;
    detail.item.playlistItemId = entryId;
}

void App::embyRequestDetailChildren(int startIndex) {
    if (!embyDetails_ || embyDetails_->item.type != "Series" || !emby_.session().signedIn()) return;
    auto& detail = *embyDetails_;
    const auto serial = detail.serial;
    const auto childSerial = ++embyDetailChildrenSerial_;
    const auto seasonId = detail.seasonId;
    detail.childrenLoading = true;
    detail.childrenBlocked = false;
    detail.childrenError.clear();
    if (startIndex == 0) {
        detail.playlistAdopted = false;
        if (seasonId.empty()) detail.seasons.clear();
        else detail.episodes.clear();
        detail.childrenTotal = 0;
    }
    const int limit = startIndex < kDetailSequenceLimit
        ? std::min(kDetailPageSize, kDetailSequenceLimit - startIndex) : kDetailPageSize;
    const auto& user = emby_.session().userId;
    const auto path = seasonId.empty()
        ? emby::seasonsPath(detail.item.id, user, emby::kBrowserItemFields, startIndex, limit)
        : emby::episodesPath(detail.item.id, user, seasonId, emby::kBrowserItemFields, startIndex, limit);
    emby_.request("GET", path, "", [this, serial, childSerial, seasonId, startIndex](const EmbyClient::Response& response) {
        embyDetailChildrenArrived(serial, childSerial, seasonId, startIndex, response);
    });
}

void App::embyDetailChildrenArrived(std::uint64_t serial, std::uint64_t childSerial,
                                  const std::string& seasonId, int startIndex,
                                  const EmbyClient::Response& response) {
    if (!embyDetails_ || embyDetails_->serial != serial || serial != embyDetailsSerial_ ||
        childSerial != embyDetailChildrenSerial_ || seasonId != embyDetails_->seasonId) return;
    auto& detail = *embyDetails_;
    detail.childrenLoading = false;
    if (response.status == 401) { embySessionExpired(); return; }
    const auto result = response.ok ? emby::parseItems(response.body) : std::nullopt;
    if (!result) {
        detail.childrenBlocked = true;
        detail.childrenError = L"The season or episode list could not be loaded.";
        return;
    }
    auto& items = seasonId.empty() ? detail.seasons : detail.episodes;
    for (const auto& item : result->items) {
        if (item.type == (seasonId.empty() ? "Season" : "Episode")) items.push_back(item);
    }
    detail.childrenTotal = std::max(result->total, startIndex + static_cast<int>(result->items.size()));
    if (!seasonId.empty()) {
        const std::string page = "detail-season:" + detail.item.id + "/" + seasonId;
        for (std::size_t pane = 0; pane < embyPanes_.size(); ++pane) {
            if (!embyPaneAccountMatches(pane) || embyPanes_[pane]->queue.page != page) continue;
            emby::adoptQueue(embyPanes_[pane]->queue, items, embyPanes_[pane]->itemId, page);
        }
    }
    const int next = startIndex + static_cast<int>(result->items.size());
    if (next < detail.childrenTotal && next < kDetailSequenceLimit && !result->items.empty()) {
        embyRequestDetailChildren(next);
        return;
    }
    if (result->items.empty() && next < detail.childrenTotal) detail.childrenBlocked = true;
    if (seasonId.empty() && !detail.seasons.empty()) {
        auto first = std::find_if(detail.seasons.begin(), detail.seasons.end(),
            [&detail](const emby::Item& item) { return item.id == detail.preferredSeasonId; });
        if (first == detail.seasons.end()) first = std::find_if(detail.seasons.begin(), detail.seasons.end(),
            [](const emby::Item& item) { return item.indexNumber > 0; });
        detail.preferredSeasonId.clear();
        embyChooseDetailSeason(first == detail.seasons.end() ? 0 : static_cast<int>(first - detail.seasons.begin()));
    }
}

void App::embyChooseDetailSeason(int index) {
    if (!embyDetails_ || index < 0 || index >= static_cast<int>(embyDetails_->seasons.size())) return;
    const auto id = embyDetails_->seasons[static_cast<std::size_t>(index)].id;
    if (id == embyDetails_->seasonId && !embyDetails_->childrenBlocked) return;
    embyDetails_->seasonId = id;
    panelPressArmed_ = false;
    pressedPanelKind_ = PanelHitKind::None;
    embyRequestDetailChildren();
}

void App::embyDetailAction(int action) {
    if (!embyDetails_) return;
    auto& detail = *embyDetails_;
    if (action == 2) { detail.expanded = !detail.expanded; return; }
    if (action == 5) {
        embySetFavorite(detail.item.id, utf8ToWideText(emby::displayTitle(detail.item)), !detail.item.favorite);
        return;
    }
    if (detail.item.type == "Series") {
        if (action != 0) return;
        for (std::size_t i = 0; i < panelRows_.size() && i < panelLayout_.rows.size(); ++i) {
            if (panelRows_[i].id == SettingId::EmbyDetailSeason || panelRows_[i].id == SettingId::EmbyDetailEpisode) {
                settingsScroll_ = std::clamp(settingsScroll_ + panelLayout_.rows[i].row.y - panelLayout_.content.y,
                                               0.0F, panelLayout_.maxScroll);
                break;
            }
        }
        return;
    }
    if (action < 0 || action > 4 || !emby::isPlayableVideo(detail.item)) return;
    const auto item = detail.item;
    auto queue = emby::takeQueue(detail.sourceItems, detail.sourceChosen, detail.sourcePage);
    const bool add = action >= 3;
    bool fromBeginning = false;
    if (action == 1 || action == 4) {
        const auto account = embyAccountSerial_;
        const auto serial = detail.serial;
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        AppendMenuW(menu, MF_STRING, 1, hasResume(item) ? L"Resume" : L"Play");
        AppendMenuW(menu, MF_STRING, 2, L"From beginning");
        const auto command = showPanelMenu(menu, SettingId::EmbyDetailAction, action);
        if (!command || command > 2 || account != embyAccountSerial_ || !embyDetails_ ||
            !settingsOpen_ || !embyBrowserOpen_ ||
            embyDetails_->serial != serial || embyDetails_->item.id != item.id) return;
        fromBeginning = command == 2;
    }
    embyRequestPlay(item, std::move(queue), add, fromBeginning);
}

void App::embyDetailEpisodeAction(int index, int action) {
    if (!embyDetails_ || index < 0 || index >= static_cast<int>(embyDetails_->episodes.size())) return;
    auto& detail = *embyDetails_;
    const auto item = detail.episodes[static_cast<std::size_t>(index)];
    if (!emby::isPlayableVideo(item)) return;
    if (action < 0 || action > 4) return;
    auto queue = emby::takeQueue(detail.episodes, index, "detail-season:" + detail.item.id + "/" + detail.seasonId);
    bool add = action >= 2;
    bool fromBeginning = action == 1 || action == 3;
    if (action == 4) {
        const auto account = embyAccountSerial_;
        const auto serial = detail.serial;
        const auto seasonId = detail.seasonId;
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        const std::wstring target = L"Play from beginning \u2192 V" + std::to_wstring(positionForPane(embyPlaybackTarget()) + 1);
        AppendMenuW(menu, MF_STRING, 1, target.c_str());
        AppendMenuW(menu, MF_STRING, 2, L"Add from beginning");
        const auto command = showPanelMenu(menu, SettingId::EmbyDetailEpisode, action, index);
        if (!command || command > 2 || account != embyAccountSerial_ || !embyDetails_ ||
            !settingsOpen_ || !embyBrowserOpen_ ||
            embyDetails_->serial != serial || embyDetails_->seasonId != seasonId) return;
        add = command == 2;
        fromBeginning = true;
    }
    embyRequestPlay(item, std::move(queue), add, fromBeginning);
}

std::vector<PanelRow> App::buildEmbyDetailRows(float clientWidth) const {
    std::vector<PanelRow> rows;
    if (!embyDetails_) return rows;
    const auto& detail = *embyDetails_;
    const auto& item = detail.item;
    const auto note = [&rows](const std::wstring& label, bool alert = false) {
        if (label.empty()) return;
        PanelRow row; row.kind = PanelRowKind::Note; row.label = label;
        row.wrapNote = true; row.alert = alert; rows.push_back(std::move(row));
    };
    PanelRow navigation;
    navigation.kind = PanelRowKind::Buttons;
    navigation.id = SettingId::EmbyNav;
    navigation.options = {L"\u2190 Back", L"Search", L"Favorites", L"Home", L"Refresh"};
    navigation.buttonMinWidth = kEmbyNavButtonWidth;
    // Stays under the header while the page and its episodes scroll.
    navigation.pinned = true;
    rows.push_back(std::move(navigation));
    PanelRow hero;
    hero.kind = PanelRowKind::MediaDetail;
    hero.id = SettingId::EmbyDetailAction;
    auto& media = hero.mediaDetail;
    media.compactActions = true;
    media.title = utf8ToWideText(item.name.empty() ? item.type : item.name);
    if (item.communityRating) addPart(media.metadata, L"\u2605 " + rating(*item.communityRating));
    if (item.criticRating) addPart(media.metadata, L"Critics " + rating(*item.criticRating) + L"%");
    if (item.productionYear > 0) addPart(media.metadata, std::to_wstring(item.productionYear));
    if (item.runTimeTicks > 0) addPart(media.metadata, formatTime(emby::secondsFromTicks(item.runTimeTicks)));
    addPart(media.metadata, utf8ToWideText(item.officialRating));
    addPart(media.metadata, joined(item.genres));
    if (!item.videoInfo.empty()) addPart(media.mediaInfo, L"Video  " + utf8ToWideText(item.videoInfo), L"\n");
    if (!item.audioInfo.empty()) addPart(media.mediaInfo, L"Audio  " + utf8ToWideText(item.audioInfo), L"\n");
    if (!item.studios.empty()) addPart(media.mediaInfo, L"Studio  " + joined(item.studios, 3), L"\n");
    // The cast are the pictures under the synopsis, each opening its person.
    std::vector<std::string> directors;
    for (const auto& person : item.people) {
        if (person.type == "Director") directors.push_back(person.name);
    }
    if (!directors.empty()) addPart(media.mediaInfo, L"Director  " + joined(directors, 3), L"\n");
    const bool technicalDetails = !media.mediaInfo.empty();
    if (!detail.expanded) media.mediaInfo.clear();
    media.overview = utf8ToWideText(item.overview);
    if (!item.taglines.empty()) media.overview = utf8ToWideText(item.taglines.front()) +
        (media.overview.empty() ? L"" : L"\n\n" + media.overview);
    media.expanded = detail.expanded;
    media.posterKey = emby::imageKey(item, 600);
    for (int index = 0; index < static_cast<int>(emby::kMaximumBackdropImages) && media.backdropKey.empty(); ++index) {
        media.backdropKey = emby::backdropImageKey(item, clientWidth > 1200.0F ? 1920 : 1280, index);
    }
    media.logoKey = emby::logoImageKey(item, 800);
    if (item.positionTicks > 0 && item.runTimeTicks > 0) {
        media.progress = std::clamp(static_cast<float>(static_cast<double>(item.positionTicks) / item.runTimeTicks), 0.0F, 1.0F);
        const auto remaining = std::max<std::int64_t>(0, item.runTimeTicks - item.positionTicks);
        media.progressText = formatTime(emby::secondsFromTicks(item.positionTicks)) + L" / " +
            formatTime(emby::secondsFromTicks(item.runTimeTicks)) + L"  \u00B7  " +
            formatTime(emby::secondsFromTicks(remaining)) + L" remaining";
    } else if (item.played) media.progressText = L"Watched";
    const auto target = L" → V" + std::to_wstring(positionForPane(embyPlaybackTarget()) + 1);
    hero.options = {item.type == "Series" ? L"Choose episode" : std::wstring(hasResume(item) ? L"Resume" : L"Play") + target,
                    item.type == "Movie" ? L"\u25BE" : L"",
                    media.overview.empty() && !technicalDetails ? L"" : detail.expanded ? L"Less" : L"More details",
                    item.type == "Movie" ? L"+ Add" : L"",
                    item.type == "Movie" ? L"\u25BE" : L"",
                    item.favorite ? L"♥" : L"♡"};
    hero.on = item.favorite;
    rows.push_back(std::move(hero));
    if (detail.loading) note(L"Loading details\u2026");
    if (!detail.error.empty()) {
        note(detail.error, true);
        PanelRow retry; retry.kind = PanelRowKind::Buttons; retry.id = SettingId::EmbyRetry;
        retry.options = {L"Retry details"}; rows.push_back(std::move(retry));
    }
    const auto appendCast = [&] {
        const auto people = embyDetailPeople();
        if (people.empty()) return;
        PanelRow heading;
        heading.kind = PanelRowKind::Header;
        heading.label = L"Cast & Crew";
        rows.push_back(std::move(heading));
        // As many to a line as the sheet is wide, filling it, as the
        // browser's posters do; the first lines only until asked for all.
        const PanelMetrics metrics;
        const float innerWidth = std::max(0.0F, clientWidth - 2.0F * metrics.pad * uiScale_);
        const int perLine = panelTilesPerLine(innerWidth, kEmbyCastTile, uiScale_, metrics);
        const float tileWidth = panelTileWidthFor(innerWidth, perLine, uiScale_, metrics);
        const int imageWidth = embyTileImageWidth(tileWidth, true, uiScale_);
        const auto line = static_cast<std::size_t>(perLine);
        const std::size_t firstLines = std::max(line, (kEmbyCastShown + line - 1) / line * line);
        const std::size_t shown = detail.castExpanded ? people.size() : std::min(people.size(), firstLines);
        for (std::size_t start = 0; start < shown; start += line) {
            PanelRow tiles;
            tiles.kind = PanelRowKind::Tiles;
            tiles.id = SettingId::EmbyDetailPerson;
            tiles.tileWidth = tileWidth;
            tiles.tileAspect = 2.0F / 3.0F;
            for (std::size_t i = start; i < shown && i < start + line; ++i) {
                tiles.tileParams.push_back(static_cast<int>(i));
                tiles.options.push_back(creditCaption(people[i]));
                tiles.tileKeys.push_back(emby::personImageKey(people[i], imageWidth));
            }
            rows.push_back(std::move(tiles));
        }
        if (people.size() > firstLines) {
            PanelRow all;
            all.kind = PanelRowKind::Buttons;
            all.id = SettingId::EmbyDetailCast;
            all.options = {detail.castExpanded ? std::wstring(L"Fewer")
                                               : L"All cast & crew  (" + std::to_wstring(people.size()) + L")"};
            rows.push_back(std::move(all));
        }
    };
    if (item.type != "Series") {
        appendCast();
        return rows;
    }
    if (!detail.seasons.empty()) {
        PanelRow seasons;
        seasons.kind = PanelRowKind::Choice;
        seasons.id = SettingId::EmbyDetailSeason;
        seasons.label = L"Season";
        seasons.segmentsPerLine = std::clamp(static_cast<int>(clientWidth / (160.0F * uiScale_)), 1, 6);
        for (const auto& season : detail.seasons) {
            if (season.id == detail.seasonId) seasons.selected = static_cast<int>(seasons.options.size());
            seasons.options.push_back(utf8ToWideText(emby::displayTitle(season)));
        }
        rows.push_back(std::move(seasons));
    }
    for (std::size_t i = 0; i < detail.episodes.size(); ++i) {
        const auto& episode = detail.episodes[i];
        PanelRow row;
        row.kind = PanelRowKind::Buttons;
        row.id = SettingId::EmbyDetailEpisode;
        row.param = static_cast<int>(i);
        row.label = detailEpisodeTitle(episode);
        row.options = {std::wstring(hasResume(episode) ? L"Resume" : L"Play") + target,
                       L"", L"+ Add", L"", L"More \u25BE"};
        row.favorite = episode.favorite;
        row.buttonMinWidth = 80.0F;
        row.enabled = emby::isPlayableVideo(episode);
        rows.push_back(std::move(row));
    }
    if (detail.childrenLoading) note(L"Loading seasons / episodes\u2026");
    else if (detail.childrenError.empty() && detail.seasons.empty()) note(L"No seasons are available.");
    else if (detail.childrenError.empty() && detail.episodes.empty()) note(L"No episodes are available in this season.");
    note(detail.childrenError, true);
    const auto held = detail.seasonId.empty() ? detail.seasons.size() : detail.episodes.size();
    if (!detail.childrenLoading && (detail.childrenBlocked || held < static_cast<std::size_t>(detail.childrenTotal))) {
        PanelRow more;
        more.kind = PanelRowKind::Buttons;
        more.id = SettingId::EmbyMore;
        more.options = {detail.childrenBlocked ? L"Retry" : L"Show more"};
        rows.push_back(std::move(more));
    }
    // Choosing what to watch precedes secondary credits on a series page.
    appendCast();
    return rows;
}

// ---------------------------------------------------------------------------
// People

std::vector<emby::Person> App::embyDetailPeople() const {
    return embyDetails_ ? emby::castAndCrew(embyDetails_->item.people) : std::vector<emby::Person>{};
}

void App::embyOpenPerson(int index) {
    if (!embyDetails_ || !emby_.session().signedIn()) return;
    const auto people = embyDetailPeople();
    if (index < 0 || static_cast<std::size_t>(index) >= people.size()) return;
    const auto& person = people[static_cast<std::size_t>(index)];
    if (person.id.empty()) {
        showNotice(L"Emby did not say which of its people " + utf8ToWideText(person.name) + L" is");
        return;
    }
    EmbyPage page;
    page.kind = EmbyPage::Kind::Person;
    page.id = person.id;
    page.title = person.name;
    // What the credit says shows at once; their own item adds the rest.
    page.person.id = person.id;
    page.person.name = person.name;
    page.person.type = "Person";
    page.person.primaryImageTag = person.primaryImageTag;
    embyDetails_->scroll = settingsScroll_;
    page.fromDetails = std::make_shared<const EmbyDetails>(*embyDetails_);
    embyPushPage(std::move(page));
}

void App::embyReturnToDetails(const std::shared_ptr<const EmbyDetails>& details) {
    if (!details) return;
    // The list beneath is asked for as Back always asks for it, but nothing
    // moves the information page: its own place is what shows, and closing
    // it gives the list back the place it was left at (sourceScroll).
    embyPendingScroll_ = -1.0F;
    embyRequestPage();
    embyDetails_ = *details;
    // What it had out when it was left was dropped with it.
    embyDetails_->serial = ++embyDetailsSerial_;
    settingsScroll_ = embyDetails_->scroll;
    panelPressArmed_ = false;
    pressedPanelKind_ = PanelHitKind::None;
    if (embyDetails_->loading || embyDetails_->childrenLoading) embyRequestDetails();
}

void App::embyRequestPerson() {
    if (embyPages_.empty() || embyPages_.back().kind != EmbyPage::Kind::Person || !emby_.session().signedIn()) return;
    const std::string id = embyPages_.back().id;
    emby_.request("GET", emby::itemDetailsPath(emby_.session().userId, id), "",
                  [this, id, account = embyAccountSerial_](const EmbyClient::Response& response) {
        embyPersonArrived(account, id, response);
    });
}

void App::embyPersonArrived(std::uint64_t account, const std::string& personId,
                            const EmbyClient::Response& response) {
    if (account != embyAccountSerial_) return;
    if (!response.ok) {
        // The page's list is asked for beside it and says what went wrong,
        // a 401 included; the person keeps what their credit said.
        if (response.status != 401) appendDiagnostic("Emby: person " + personId + " could not be read: " + response.error);
        return;
    }
    const auto person = emby::parseItem(response.body);
    if (!person || person->id != personId) return;
    // Whichever pages show this person, opened from one film or another.
    for (auto& page : embyPages_) {
        if (page.kind == EmbyPage::Kind::Person && page.id == personId) page.person = *person;
    }
}

PanelRow App::embyPersonRow(const EmbyPage& page) const {
    const auto& person = page.person;
    PanelRow row;
    row.kind = PanelRowKind::MediaDetail;
    row.id = SettingId::EmbyPersonAction;
    auto& media = row.mediaDetail;
    media.title = utf8ToWideText(person.name.empty() ? page.title : person.name);
    const auto born = emby::calendarDate(person.premiereDate);
    const auto died = emby::calendarDate(person.endDate);
    if (!born.empty()) addPart(media.metadata, L"Born " + utf8ToWideText(born));
    if (!person.productionLocations.empty()) addPart(media.metadata, utf8ToWideText(person.productionLocations.front()));
    if (!died.empty()) addPart(media.metadata, L"Died " + utf8ToWideText(died));
    media.overview = utf8ToWideText(person.overview);
    media.expanded = page.expanded;
    media.posterKey = emby::imageKey(person, 600);
    // Nothing to play here: only the biography's More / Less.
    row.options = {L"", L"", media.overview.empty() ? L"" : page.expanded ? L"Less" : L"More"};
    return row;
}

void App::embyPersonAction(int action) {
    if (action != 2 || embyDetails_ || embyPages_.empty() || embyPages_.back().kind != EmbyPage::Kind::Person) return;
    embyPages_.back().expanded = !embyPages_.back().expanded;
}
}  // namespace quaddeck
