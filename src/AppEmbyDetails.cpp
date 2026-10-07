// Emby information pages. Browsing details is read-only; the explicit play
// actions alone adopt a playback sequence and enter the existing player.
#include "App.hpp"
#include "AppInternal.hpp"
#include "TextEncoding.hpp"

#include <algorithm>
#include <iomanip>
#include <sstream>

namespace quaddeck {
using namespace app_internal;
namespace {
constexpr int kDetailPageSize = 300;
constexpr int kDetailSequenceLimit = 20000;

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
}

std::string App::embyCurrentLibraryId() const {
    if (embyDetails_) return embyDetails_->libraryId;
    if (!embyPages_.empty() && embyPages_.back().kind == EmbyPage::Kind::Search) return {};
    for (auto page = embyPages_.rbegin(); page != embyPages_.rend(); ++page) {
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
    if (embyItemLibraries_.size() >= 20000 && !embyItemLibraries_.contains(item.id)) return;
    embyItemLibraries_[item.id].insert(libraryId);
}

std::string App::embyItemLibraryId(const emby::Item& item) const {
    const auto current = embyCurrentLibraryId();
    if (!current.empty()) return current;
    if (!embyLibraryType(item.parentId).empty()) return item.parentId;
    const auto found = embyItemLibraries_.find(item.id);
    return found != embyItemLibraries_.end() && found->second.size() == 1 ? *found->second.begin() : std::string();
}

void App::embyResolveResumeLibraries() {
    if (!emby_.session().signedIn() || embyViews_.empty()) return;
    for (const auto& item : embyResume_) {
        if (item.type != "Movie" && item.type != "Series") continue;
        if (!embyLibraryType(item.parentId).empty()) { embyRememberLibrary(item, item.parentId); continue; }
        if (embyItemLibraries_.contains(item.id) || !embyLibraryLookups_.insert(item.id).second) continue;
        const auto account = embyAccountSerial_;
        emby_.request("GET", emby::ancestorsPath(item.id, emby_.session().userId), "",
                      [this, item, account](const EmbyClient::Response& response) {
            if (account != embyAccountSerial_) return;
            embyLibraryLookups_.erase(item.id);
            if (response.status == 401) { embySessionExpired(); return; }
            if (!response.ok) return;  // unknown stays on the original browsing path
            const auto ancestors = emby::parseAncestorsArray(response.body);
            if (!ancestors) return;
            const auto library = emby::uniqueAncestorLibraryId(*ancestors, embyViews_);
            if (!library.empty()) embyRememberLibrary(item, library);
        });
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
    embyRequestPlay(item, emby::takeQueue(detail.sourceItems, detail.sourceChosen, detail.sourcePage),
                    action >= 3, action == 1 || action == 4);
}

void App::embyDetailEpisodeAction(int index, int action) {
    if (!embyDetails_ || index < 0 || index >= static_cast<int>(embyDetails_->episodes.size())) return;
    auto& detail = *embyDetails_;
    const auto item = detail.episodes[static_cast<std::size_t>(index)];
    if (!emby::isPlayableVideo(item)) return;
    if (action < 0 || action > 3) return;
    embyRequestPlay(item, emby::takeQueue(detail.episodes, index, "detail-season:" + detail.item.id + "/" + detail.seasonId),
                    action >= 2, action == 1 || action == 3);
}

std::vector<PanelRow> App::buildEmbyDetailRows(float clientWidth) const {
    std::vector<PanelRow> rows;
    if (!embyDetails_) return rows;
    const auto& detail = *embyDetails_;
    const auto& item = detail.item;
    const auto note = [&rows](const std::wstring& label) {
        if (label.empty()) return;
        PanelRow row; row.kind = PanelRowKind::Note; row.label = label; rows.push_back(std::move(row));
    };
    PanelRow navigation;
    navigation.kind = PanelRowKind::Buttons;
    navigation.id = SettingId::EmbyNav;
    navigation.options = {L"\u2190 Back", L"Search\u2026", L"Home", L"Refresh"};
    rows.push_back(std::move(navigation));
    note(L"Play / Resume target: " + embyTargetLabel());
    note(L"Add: new pane, muted. Click a video pane to target it.");
    PanelRow hero;
    hero.kind = PanelRowKind::MediaDetail;
    hero.id = SettingId::EmbyDetailAction;
    auto& media = hero.mediaDetail;
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
    std::vector<std::string> directors, cast;
    for (const auto& person : item.people) {
        if (person.type == "Director") directors.push_back(person.name);
        else if (person.type == "Actor") cast.push_back(person.name);
    }
    if (!directors.empty()) addPart(media.mediaInfo, L"Director  " + joined(directors, 3), L"\n");
    if (!cast.empty()) addPart(media.mediaInfo, L"Cast  " + joined(cast, 6), L"\n");
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
                    item.type == "Movie" ? L"From Beginning" + target : L"",
                    media.overview.empty() ? L"" : detail.expanded ? L"Less" : L"More",
                    item.type == "Movie" ? hasResume(item) ? L"Add & resume" : L"Add to new pane" : L"",
                    item.type == "Movie" && hasResume(item) ? L"Add from beginning" : L""};
    rows.push_back(std::move(hero));
    if (detail.loading) note(L"Loading details\u2026");
    note(detail.error);
    if (item.type != "Series") return rows;
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
        row.options = {std::wstring(hasResume(episode) ? L"Resume" : L"Play") + target, L"From Beginning" + target,
                       hasResume(episode) ? L"Add & resume" : L"Add to new pane"};
        if (hasResume(episode)) row.options.push_back(L"Add from beginning");
        row.buttonMinWidth = 160.0F;
        row.enabled = emby::isPlayableVideo(episode);
        rows.push_back(std::move(row));
    }
    if (detail.childrenLoading) note(L"Loading seasons / episodes\u2026");
    else if (detail.seasons.empty()) note(L"No seasons are available.");
    else if (detail.episodes.empty()) note(L"No episodes are available in this season.");
    note(detail.childrenError);
    const auto held = detail.seasonId.empty() ? detail.seasons.size() : detail.episodes.size();
    if (!detail.childrenLoading && (detail.childrenBlocked || held < static_cast<std::size_t>(detail.childrenTotal))) {
        PanelRow more;
        more.kind = PanelRowKind::Buttons;
        more.id = SettingId::EmbyMore;
        more.options = {detail.childrenBlocked ? L"Retry" : L"Show more"};
        rows.push_back(std::move(more));
    }
    return rows;
}
}  // namespace quaddeck
