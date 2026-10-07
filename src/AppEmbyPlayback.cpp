// App: playing an Emby item into a pane. The explicit target, Add and the
// full-deck replacement, resolving an item to a stream or a photo, and
// neighbour episodes and folder siblings. Replies arrive on the window
// thread through EmbyClient::pump(), as described in AppEmby.cpp.

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

constexpr int kEmbySiblingScan = 4000;
constexpr int kEmbyPhotoWidth = 3840;

std::wstring wide(const std::string& text) { return utf8ToWideText(text); }
std::string narrow(const std::wstring& text) { return wideToUtf8Text(text); }

std::string streamHeaders(const emby::Session& session) {
    return "X-Emby-Token: " + session.token + "\r\nX-Emby-Authorization: " +
           emby::authorizationHeader(session.deviceId) + "\r\n";
}

}  // namespace

// ---------------------------------------------------------------------------
// Playing an item

std::size_t App::embyPlaybackTarget() const {
    if (embyTargetPane_ >= 0 && embyTargetPane_ < static_cast<int>(kMaxPanes) &&
        (paneLogicallyLoaded(static_cast<std::size_t>(embyTargetPane_)) || !paths_[static_cast<std::size_t>(embyTargetPane_)].empty())) {
        return static_cast<std::size_t>(embyTargetPane_);
    }
    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) if (paneLogicallyLoaded(pane) || !paths_[pane].empty()) return pane;
    return 0;
}

void App::embySelectTarget(std::size_t pane) {
    if (pane >= kMaxPanes) return;
    embyTargetPane_ = static_cast<int>(pane);
    if (soloPane_ >= 0) soloPane_ = static_cast<int>(pane);
    controlsLastInteraction_ = GetTickCount64();
    setControlsVisible(true);
}

std::wstring App::embyTargetLabel() const {
    const auto pane = embyPlaybackTarget();
    std::wstring title = embyPanes_[pane] ? embyPanes_[pane]->title : std::filesystem::path(paths_[pane]).filename().wstring();
    if (title.empty()) title = L"empty";
    return L"V" + std::to_wstring(positionForPane(pane) + 1) + L" · " + title;
}

bool App::embyPaneAccountMatches(std::size_t pane) const {
    if (pane >= kMaxPanes || !embyPanes_[pane]) return false;
    const auto& slot = *embyPanes_[pane];
    const auto& session = emby_.session();
    return session.signedIn() && slot.accountSerial == embyAccountSerial_ && slot.serverId == session.serverId &&
           slot.serverUrl == session.serverUrl && slot.userId == session.userId;
}

int App::embyPlayingPane(const std::string& itemId) const {
    if (itemId.empty()) return -1;
    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        if (embyPaneAccountMatches(pane) && embyPanes_[pane]->itemId == itemId &&
            (paneLogicallyLoaded(pane) || !paths_[pane].empty())) return static_cast<int>(pane);
    }
    return -1;
}

void App::embyRequestPlay(const emby::Item& item, const emby::PlaybackQueue& queue, bool add, bool fromBeginning) {
    if (!emby_.session().signedIn() || (!emby::isPlayableVideo(item) && !emby::isPhoto(item))) return;
    if (const int existing = embyPlayingPane(item.id); existing >= 0) {
        embySelectTarget(static_cast<std::size_t>(existing));
        showNotice(L"Already open in " + embyTargetLabel());
        return;
    }
    std::size_t pane = embyPlaybackTarget();
    if (add) {
        pane = kMaxPanes;
        for (std::size_t candidate = 0; candidate < kMaxPanes; ++candidate) {
            if (!paneLogicallyLoaded(candidate) && paths_[candidate].empty() && !embyPanes_[candidate]) { pane = candidate; break; }
        }
        if (pane == kMaxPanes) {
            EmbyPendingPlay pending;
            pending.item = item; pending.queue = queue; pending.fromBeginning = fromBeginning;
            pending.accountSerial = embyAccountSerial_; pending.scroll = settingsScroll_;
            pending.paths = paths_; pending.mediaSerials = subtitleSerial_;
            embyPendingPlay_ = std::move(pending);
            settingsScroll_ = 0.0F;
            panelPressArmed_ = false; pressedPanelKind_ = PanelHitKind::None;
            return;
        }
    }
    embyPlayItem(item, pane, fromBeginning, add);
    if (embyPanes_[pane] && embyPanes_[pane]->itemId == item.id) embyPanes_[pane]->queue = queue;
}

void App::embyAddItem(int param) {
    if (embyLoading_ && !embyPages_.empty() && embyPages_.back().kind == EmbyPage::Kind::Search) return;
    const auto& list = param < 0 ? embyResume_ : embyItems_;
    const int index = param < 0 ? -param - 1 : param;
    if (index < 0 || static_cast<std::size_t>(index) >= list.size()) return;
    const auto item = list[static_cast<std::size_t>(index)];
    if (!emby::isPlayableVideo(item)) return;
    embyRequestPlay(item, emby::takeQueue(list, index, param < 0 || embyPages_.empty() ? "resume" : embyPageKey(embyPages_.back())), true);
}

void App::embyCancelReplacement() {
    if (!embyPendingPlay_) return;
    settingsScroll_ = embyPendingPlay_->scroll;
    embyPendingPlay_.reset();
    panelPressArmed_ = false; pressedPanelKind_ = PanelHitKind::None;
}

void App::embyConfirmReplacement(std::size_t pane) {
    if (!embyPendingPlay_ || pane >= kMaxPanes) return;
    auto pending = *embyPendingPlay_;
    embyCancelReplacement();
    if (pending.accountSerial != embyAccountSerial_ || !emby_.session().signedIn()) return;
    if (const int existing = embyPlayingPane(pending.item.id); existing >= 0) {
        embySelectTarget(static_cast<std::size_t>(existing));
        showNotice(L"Already open in " + embyTargetLabel());
        return;
    }
    if (paths_[pane] != pending.paths[pane] || subtitleSerial_[pane] != pending.mediaSerials[pane]) {
        showNotice(L"That pane changed. Add again to choose its replacement.");
        return;
    }
    embyPlayItem(pending.item, pane, pending.fromBeginning, true);
    if (embyPanes_[pane] && embyPanes_[pane]->itemId == pending.item.id) embyPanes_[pane]->queue = std::move(pending.queue);
}

std::vector<PanelRow> App::buildEmbyReplacementRows(float clientWidth) const {
    std::vector<PanelRow> rows;
    if (!embyPendingPlay_) return rows;
    PanelRow heading; heading.kind = PanelRowKind::Header; heading.label = L"All five panes are occupied"; rows.push_back(std::move(heading));
    PanelRow note; note.kind = PanelRowKind::Note; note.wrapNote = true;
    note.label = L"Replace one pane with " + wide(emby::displayTitle(embyPendingPlay_->item)) +
        (embyPendingPlay_->fromBeginning ? L" from the beginning (muted)." : L" (resume saved progress, muted).");
    rows.push_back(std::move(note));
    PanelRow cancel; cancel.kind = PanelRowKind::Buttons; cancel.id = SettingId::EmbyReplaceCancel;
    cancel.options = {L"Cancel — keep all panes"}; rows.push_back(std::move(cancel));
    const float inner = std::max(0.0F, clientWidth - 40.0F * uiScale_);
    const int perLine = panelTilesPerLine(inner, 200.0F, uiScale_);
    const auto order = panesByPosition();
    for (int start = 0; start < static_cast<int>(kMaxPanes); start += perLine) {
        PanelRow row; row.kind = PanelRowKind::Tiles; row.id = SettingId::EmbyReplaceChoice;
        row.tileWidth = panelTileWidthFor(inner, perLine, uiScale_); row.tileAspect = 16.0F / 9.0F;
        for (int position = start; position < std::min(start + perLine, static_cast<int>(kMaxPanes)); ++position) {
            const auto pane = static_cast<std::size_t>(order[static_cast<std::size_t>(position)]);
            const auto& slot = embyPanes_[pane];
            const auto title = slot ? slot->title : std::filesystem::path(paths_[pane]).filename().wstring();
            row.options.push_back(L"V" + std::to_wstring(position + 1) + L" · " + title);
            row.tileKeys.push_back(slot ? slot->imageKey : localThumbnailKey(paths_[pane], 400));
            row.tileParams.push_back(static_cast<int>(pane));
            row.tileBadges.push_back(audioPaneEnabled(pane) ? L"Audio on" : L"Muted");
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

void App::embyPlayItem(const emby::Item& entry, std::size_t pane, bool fromBeginning, bool add, bool selectTarget) {
    const auto& session = emby_.session();
    if (!session.signedIn() || pane >= sources_.size()) return;
    if (const int existing = embyPlayingPane(entry.id); existing >= 0) {
        if (selectTarget) embySelectTarget(static_cast<std::size_t>(existing));
        showNotice(L"Already open in V" + std::to_wstring(positionForPane(static_cast<std::size_t>(existing)) + 1));
        return;
    }
    const bool emptyDeck = !anyPaneLoaded();
    const bool startPlaying = emptyDeck || playbackIntended();
    const bool retainedMute = (embyPanes_[pane] && embyPanes_[pane]->addedMuted) ||
                              (localPanes_[pane] && localPanes_[pane]->addedMuted);
    // openSource reports/stops only this destination. The other sources,
    // master clock, offsets and their selected audio voices are untouched.
    detachPaneSeekBarrier(pane);
    openSource(pane, emby::formatLocator(session.serverId, entry.id));
    // An Emby item plays as itself: no leftover delay or rate from the file
    // that held the pane before it.
    startDelays_[pane] = 0.0;
    playbackRates_[pane] = 1.0;
    audio_.setRate(pane, 1.0F);
    if (embyPanes_[pane]) {
        auto& state = *embyPanes_[pane];
        state.autoplay = true;
        state.startPlaying = startPlaying;
        state.addedMuted = add || retainedMute;
        state.title = wide(emby::displayTitle(entry));
        state.imageKey = emby::imageKey(entry, 400);
        state.photo = emby::isPhoto(entry);
        state.fromBeginning = fromBeginning;
        state.resumeTicks = fromBeginning ? 0 : entry.positionTicks;
        state.seriesId = entry.seriesId;
        state.parentId = entry.parentId;
    }
    if (add) setAudioMask(audioMask_ & ~(1U << pane));
    else if (emptyDeck) setAudioMask(1U << pane);
    if (emptyDeck && !deviceRecoveryPending_) clock_.play();
    if (selectTarget) embySelectTarget(pane);
    if (add) soloPane_ = -1;
    showNotice(L"V" + std::to_wstring(positionForPane(pane) + 1) + L": " + wide(emby::displayTitle(entry)) +
        (add ? L" · muted" : L"") + (startPlaying ? L"" : L" · paused with all panes"));
    layoutHoverControls();
    updateHoverControls();
    updateControls();
    updateTitle();
}

void App::embyBeginResolve(std::size_t pane) {
    const auto locator = emby::parseLocator(paths_[pane]);
    if (!locator) {
        appendDiagnostic("Emby: unreadable locator " + narrow(paths_[pane]));
        paths_[pane].clear();
        return;
    }
    const auto session = emby_.session();
    // Session restore and direct emby:// opens share the same uniqueness
    // rule as browser actions, including items still resolving.
    if (session.signedIn() && (locator->serverId.empty() || locator->serverId == session.serverId)) {
        if (const int existing = embyPlayingPane(locator->itemId); existing >= 0 && existing != static_cast<int>(pane)) {
            paths_[pane].clear();
            embySelectTarget(static_cast<std::size_t>(existing));
            showNotice(L"Already open in " + embyTargetLabel());
            return;
        }
    }
    EmbyPane state;
    state.serial = ++embyPaneSerial_;
    state.itemId = locator->itemId;
    state.serverId = locator->serverId.empty() ? session.serverId : locator->serverId;
    state.serverUrl = session.serverUrl;
    state.userId = session.userId;
    state.accountSerial = embyAccountSerial_;
    state.title = L"Emby " + wide(locator->itemId);
    embyPanes_[pane] = state;
    if (!session.signedIn()) {
        showNotice(L"Sign in to Emby to play this item");
        return;
    }
    if (!locator->serverId.empty() && !session.serverId.empty() && locator->serverId != session.serverId) {
        showNotice(L"This item belongs to another Emby server");
        appendDiagnostic("Emby: locator server " + locator->serverId + " is not the signed-in " + session.serverId);
        return;
    }
    embyPanes_[pane]->resolving = true;
    const std::uint64_t serial = state.serial;
    emby_.request("GET", emby::itemPath(session.userId, locator->itemId), "",
                  [this, serial](const EmbyClient::Response& response) { embyItemArrived(serial, response); });
}

void App::embyItemArrived(std::uint64_t serial, const EmbyClient::Response& response) {
    const auto found = embyPaneWithSerial(serial);
    if (!found) return;
    const std::size_t target = *found;
    if (!response.ok) {
        embyResolveFailed(target, L"Emby: " + wide(response.error));
        if (response.status == 401) embySessionExpired();
        return;
    }
    const auto item = emby::parseItem(response.body);
    if (!item) {
        embyResolveFailed(target, L"Emby sent an item QuadDeck cannot read");
        return;
    }
    auto& state = *embyPanes_[target];
    state.title = wide(emby::displayTitle(*item));
    state.imageKey = emby::imageKey(*item, 400);
    state.runTimeTicks = item->runTimeTicks;
    if (state.seriesId.empty()) state.seriesId = item->seriesId;
    state.seasonId = item->seasonId;
    if (state.parentId.empty()) state.parentId = item->parentId;
    state.photo = state.photo || emby::isPhoto(*item);
    // Where to resume is what the server says now, not what the list said
    // when it was fetched: the video may have been watched further since,
    // here or in another client, or marked as not started. Played may
    // coexist with a new position during a rewatch; keep that position.
    state.resumeTicks = state.fromBeginning ? 0 : item->positionTicks;
    if (state.photo) {
        embyStartPhoto(target, *item);
        return;
    }
    const std::string userId = emby_.session().userId;
    emby_.request("POST", emby::playbackInfoPath(state.itemId, userId), emby::playbackInfoBody(userId),
                  [this, serial, entry = *item](const EmbyClient::Response& reply) {
        const auto again = embyPaneWithSerial(serial);
        if (!again) return;
        const std::size_t pane = *again;
        if (!reply.ok) {
            embyResolveFailed(pane, L"Emby: " + wide(reply.error));
            if (reply.status == 401) embySessionExpired();
            return;
        }
        const auto info = emby::parsePlaybackInfo(reply.body);
        if (!info) {
            embyResolveFailed(pane, L"Emby sent playback information QuadDeck cannot read");
            return;
        }
        embyStartResolved(pane, entry, *info);
    });
}

void App::embyResolveFailed(std::size_t pane, const std::wstring& reason) {
    if (pane < embyPanes_.size() && embyPanes_[pane]) embyPanes_[pane]->resolving = false;
    appendDiagnostic(narrow(reason));
    showNotice(reason);
    closePane(pane);
    updateTitle();
}

void App::embyStartResolved(std::size_t pane, const emby::Item& item, const emby::PlaybackInfo& info) {
    if (!embyPaneAccountMatches(pane)) return;
    const auto* source = emby::directPlaySource(info);
    if (!source || !(source->supportsDirectPlay || source->supportsDirectStream)) {
        std::wstring reason = L"Emby cannot hand this item over as a file";
        if (!info.errorCode.empty()) reason += L" (" + wide(info.errorCode) + L")";
        else if (source && !source->transcodingUrl.empty()) {
            reason += L"; it would need transcoding, which QuadDeck does not do";
        }
        embyResolveFailed(pane, reason);
        return;
    }
    auto& state = *embyPanes_[pane];
    state.resolving = false;
    state.mediaSourceId = source->id;
    state.playSessionId = info.playSessionId;
    state.streams = source->streams;
    if (source->runTimeTicks > 0) state.runTimeTicks = source->runTimeTicks;
    const auto session = emby_.session();
    const std::string streamPath = emby::streamPath(state.itemId, state.mediaSourceId, state.playSessionId);
    SourceOptions options;
    options.httpHeaders = streamHeaders(session);
    // The file comes as it lies on the server's disk, its subtitle streams
    // with it: they are read as the video is, as a local file's are.
    options.readSubtitles = true;
    const double resume = emby::secondsFromTicks(state.resumeTicks);
    const double length = emby::secondsFromTicks(state.runTimeTicks);
    const bool resumed = state.autoplay && resume > 5.0 && (length <= 0.0 || resume < length - 5.0);
    if (state.autoplay) {
        const double target = resumed ? resume : 0.0;
        // A source-local origin under the unchanged master clock. This is
        // also valid when all panes were paused while the request was out.
        syncAdjustments_[pane] = target - (clock_.position() - startDelays_[pane]) * playbackRates_[pane];
        sourcePausedTimes_[pane] = target;
    }
    startSourceStream(pane, wide(session.serverUrl + streamPath), options, source->defaultAudioStreamIndex,
                      clock_.position(), clock_.isPlaying());
    // The stream the server names for this account, or, when it names none
    // -- an account set to show subtitles only over foreign audio with no
    // language given names none for anything -- the player's own choice.
    state.subtitleStream = -1;
    autoChooseSubtitle(pane, source->defaultSubtitleStreamIndex);
    subtitleEmbeddedPending_[pane] = true;
    appendDiagnostic("Emby: playing item " + state.itemId + " (" + emby::displayTitle(item) + ") from " +
                     session.serverUrl + streamPath);
    if (state.autoplay) {
        const auto label = L"V" + std::to_wstring(positionForPane(pane) + 1);
        showNotice(label + (resumed ? L" resumed at " + formatTime(resume) : L" from beginning") +
            (audioPaneEnabled(pane) ? L"" : L" · muted") + (clock_.isPlaying() ? L"" : L" · paused with all panes"));
    }
    state.autoplay = false;
    updateTitle();
    updateControls();
}

void App::embyStartPhoto(std::size_t pane, const emby::Item& item) {
    if (!embyPaneAccountMatches(pane)) return;
    auto& state = *embyPanes_[pane];
    state.resolving = false;
    const auto session = emby_.session();
    // A photo is served as its own primary image; an item without one has
    // only its original file.
    const std::string path = item.primaryImageTag.empty()
        ? emby::downloadPath(item.id)
        : emby::imagePath(item.id, kEmbyPhotoWidth, item.primaryImageTag);
    SourceOptions options;
    options.httpHeaders = streamHeaders(session);
    state.mediaSourceId.clear();  // no playback reports for a still
    startSourceStream(pane, wide(session.serverUrl + path), options, -1, clock_.position(), clock_.isPlaying());
    appendDiagnostic("Emby: showing photo " + item.id + " from " + session.serverUrl + path);
    state.autoplay = false;
    updateTitle();
    updateControls();
}

void App::embyOpenAdjacent(std::size_t pane, int step, bool selectTarget) {
    if (!embyPaneAccountMatches(pane)) return;
    const auto session = emby_.session();
    if (!session.signedIn()) return;
    const auto& state = *embyPanes_[pane];
    // The list the item was chosen from comes first: the browser's order,
    // search results included. Past its ends the server's neighbours take over.
    if (const int playing = embyPlaylistIndex(state.itemId, pane); playing >= 0) {
        const long long next = static_cast<long long>(playing) + step;
        if (next < 0 || next >= static_cast<long long>(state.queue.items.size())) {
            showNotice(step > 0 ? L"Last item in the list" : L"First item in the list");
            return;
        }
        embyPlayFromPlaylist(static_cast<int>(next), pane, selectTarget);
        return;
    }
    std::string path;
    if (!state.seriesId.empty()) {
        path = emby::episodesPath(state.seriesId, session.userId);
    } else if (!state.parentId.empty()) {
        emby::ItemsQuery query;
        query.parentId = state.parentId;
        query.sortBy = "IsFolder,SortName";
        query.limit = kEmbySiblingScan;
        query.fields = "ParentId";
        path = emby::itemsPath(session.userId, query);
    } else {
        showNotice(L"This item has no neighbours to step to");
        return;
    }
    const std::string currentId = state.itemId;
    const std::uint64_t serial = state.serial;
    emby_.request("GET", path, "", [this, serial, currentId, step, selectTarget](const EmbyClient::Response& response) {
        const auto found = embyPaneWithSerial(serial);
        if (!found) return;
        if (!response.ok) {
            showNotice(L"Emby: " + wide(response.error));
            if (response.status == 401) embySessionExpired();
            return;
        }
        auto list = emby::parseItems(response.body);
        if (!list) return;
        std::vector<emby::Item> playable;
        for (auto& entry : list->items) {
            if (emby::isPlayableVideo(entry) || emby::isPhoto(entry)) playable.push_back(std::move(entry));
        }
        const int next = emby::adjacentIndex(playable, currentId, step);
        if (next < 0) {
            showNotice(step > 0 ? L"Last item" : L"First item");
            return;
        }
        const auto item = playable[static_cast<std::size_t>(next)];
        if (const int existing = embyPlayingPane(item.id); existing >= 0) {
            if (selectTarget) embySelectTarget(static_cast<std::size_t>(existing));
            showNotice(L"Already open in V" + std::to_wstring(positionForPane(static_cast<std::size_t>(existing)) + 1));
            return;
        }
        embyPlayItem(item, *found, false, false, selectTarget);
        embyTakePlaylist(playable, next, *found);
    });
}

}  // namespace quaddeck
