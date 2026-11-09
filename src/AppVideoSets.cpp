// App: the video sets -- QuadDeck's own list of decks to play whole
// (VideoSets.hpp). The list is read at start and written whole after each
// change, beside the settings file; it leaves the player only when it is
// exported to a .qdeck file, and a .qdeck file opened or dropped adds its
// sets to it. A set plays as a session used to load -- through
// applySession -- with its videos from their start in its arrangement and
// every other setting as it is. Its Emby items are named by the server
// they came from, and one the server no longer knows by its id is searched
// for by what the set remembers of it.

#include "App.hpp"
#include "AppInternal.hpp"
#include "Diagnostics.hpp"
#include "FilePersistence.hpp"

#include <commdlg.h>

#include <algorithm>
#include <filesystem>
#include <locale>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <utility>

namespace quaddeck {

using namespace app_internal;

namespace {

std::wstring wide(const std::string& text) { return utf8ToWideText(text); }
std::string narrow(const std::wstring& text) { return wideToUtf8Text(text); }

constexpr const wchar_t* kVideoSetsFilter = L"QuadDeck video sets (*.qdeck)\0*.qdeck\0All files\0*.*\0";

std::wstring placeName(std::size_t position) { return L"V" + std::to_wstring(position + 1); }

// What follows a set's end, as the toolbar's drop-down names it.
const wchar_t* videoSetOrderName(PlayOrder order) {
    switch (order) {
    case PlayOrder::PlayOne: return L"Stop at its end";
    case PlayOrder::RepeatOne: return L"Repeat the set";
    case PlayOrder::InOrder: return L"Next, then stop";
    case PlayOrder::RepeatList: return L"Repeat the list";
    default: return L"Shuffle";
    }
}

emby::RefindKey refindKey(const VideoSetVideo& video) {
    return {narrow(video.name), narrow(video.seriesName), narrow(video.type), narrow(video.serverPath),
            video.season, video.episode, video.year};
}

// What the set keeps of an Emby item, as the item's reply says it.
void rememberItem(VideoSetVideo& video, const emby::RefindKey& item) {
    video.name = wide(item.name);
    video.seriesName = wide(item.seriesName);
    video.type = wide(item.type);
    video.serverPath = wide(item.path);
    video.season = item.season;
    video.episode = item.episode;
    video.year = item.year;
}

emby::RefindKey refindKeyOf(const emby::Item& item) {
    return {item.name, item.seriesName, item.type, item.path, item.parentIndexNumber, item.indexNumber,
            item.productionYear};
}

// Why an Emby item cannot be asked for at all with the account signed in:
// none is, or it is another server's. Empty for a file, or an item that can.
std::wstring sessionProblem(const VideoSetVideo& video, const emby::Session& session) {
    if (!emby::isLocator(video.path)) return {};
    const auto locator = emby::parseLocator(video.path);
    if (!locator) return L"not a video QuadDeck can open";
    if (!session.signedIn()) return L"sign in to Emby to play it";
    if (!locator->serverId.empty() && locator->serverId != session.serverId) {
        return video.serverName.empty() ? std::wstring(L"on another Emby server")
                                        : L"on another Emby server: " + video.serverName;
    }
    return {};
}

std::wstring videoTitle(const VideoSetVideo& video) {
    if (!video.title.empty()) return video.title;
    if (emby::isLocator(video.path)) return L"Emby item";
    return std::filesystem::path(video.path).filename().wstring();
}

bool readVideoSetsFile(const std::filesystem::path& path, VideoSetList& list, bool* oldSession = nullptr) {
    std::wstring text;
    if (!readUtf8File(path, text)) return false;
    if (oldSession) *oldSession = text.rfind(L"QDECK", 0) == 0 || text.rfind(L"\xFEFFQDECK", 0) == 0;
    std::wistringstream input(text);
    input.imbue(std::locale::classic());
    return readVideoSets(input, list);
}

}  // namespace

// --- the list and its file -----------------------------------------------------

std::filesystem::path App::videoSetsPath() const {
    const auto settings = settingsPath();
    return settings.empty() ? settings : settings.parent_path() / L"videosets.qsets";
}

void App::loadVideoSets() {
    videoSets_ = {};
    videoSetsReadOnly_ = false;
    const auto path = videoSetsPath();
    std::error_code error;
    if (path.empty() || !std::filesystem::exists(path, error)) return;
    if (!readVideoSetsFile(path, videoSets_)) {
        // Not written over: a newer QuadDeck's list, or one to be mended.
        videoSets_ = {};
        videoSetsReadOnly_ = true;
        appendDiagnostic("Video sets: videosets.qsets could not be read; it is left as it is");
    }
}

void App::saveVideoSets() {
    if (videoSetsReadOnly_) return;
    const auto path = videoSetsPath();
    if (path.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const VideoSetList& list = videoSets_;
    if (!writeUtf8FileAtomically(path, [&](std::wostream& output) { return writeVideoSets(output, list); })) {
        appendDiagnostic("Video sets: videosets.qsets could not be written");
        showNotice(L"Could not save the video sets");
    }
}

std::optional<VideoSet> App::captureVideoSet() const {
    VideoSet set;
    set.layout = layoutMode_;
    const auto session = emby_.session();
    const auto order = panesByPosition();
    std::vector<std::wstring> titles;
    for (const int slot : order) {
        if (slot < 0) continue;
        const auto pane = static_cast<std::size_t>(slot);
        if (paths_[pane].empty() || set.videos.size() >= kMaxPanes) continue;
        VideoSetVideo video;
        video.path = paths_[pane];
        if (embyPanes_[pane]) {
            const auto& state = *embyPanes_[pane];
            video.title = state.title;
            if (state.serverId == session.serverId) video.serverName = wide(session.serverName);
            rememberItem(video, state.refind);
        } else {
            video.title = std::filesystem::path(paths_[pane]).stem().wstring();
        }
        if (expandedPane_ == slot) set.expanded = static_cast<int>(set.videos.size());
        titles.push_back(video.title);
        set.videos.push_back(std::move(video));
    }
    if (set.videos.empty()) return std::nullopt;
    set.name = videoSetDefaultName(titles);
    return set;
}

void App::addVideoSet() {
    auto set = captureVideoSet();
    if (!set) {
        showNotice(L"Open the videos to play together first");
        return;
    }
    if (const int same = findSameVideos(videoSets_, *set); same >= 0) {
        showNotice(L"Already in the video sets: " + videoSets_.sets[static_cast<std::size_t>(same)].name);
        return;
    }
    if (videoSets_.sets.size() >= kVideoSetLimit) {
        showNotice(L"The video sets are full");
        return;
    }
    const std::wstring name = set->name;
    videoSets_.sets.push_back(std::move(*set));
    saveVideoSets();
    showNotice(L"Added to the video sets: " + name + L"  (" + std::to_wstring(videoSets_.sets.size()) + L")");
    if (settingsOpen_ && videoSetsOpen_) checkVideoSets();
}

// --- the face ------------------------------------------------------------------

void App::showVideoSets() {
    controlsLastInteraction_ = GetTickCount64();
    const bool wasOpen = settingsOpen_;
    if (wasOpen) rememberSheetScroll();
    openSheet();
    embyCancelReplacement();
    localCancelReplacement();
    embyBrowserOpen_ = false;
    videoSetsOpen_ = true;
    cancelVideoSetRename();
    restoreSheetScroll();
    checkVideoSets();
}

void App::toggleVideoSets() {
    if (settingsOpen_ && videoSetsOpen_ && !embyBrowserOpen_) {
        closeSettingsPanel();
        return;
    }
    showVideoSets();
}

// A note per video under its set: its place, its title, and what is wrong
// with it, in the warning colour, when something is.
std::wstring App::videoSetProblem(const VideoSetVideo& video, bool* alert) const {
    if (alert) *alert = false;
    if (auto problem = sessionProblem(video, emby_.session()); !problem.empty()) {
        if (alert) *alert = true;
        return problem;
    }
    const auto found = videoSetChecks_.find(video.path);
    const auto state = found == videoSetChecks_.end() ? VideoSetState::Checking : found->second.state;
    switch (state) {
    case VideoSetState::Checking:
    case VideoSetState::Fine:
        // There as far as the check goes, it could still not be played the
        // last time a set opened it.
        if (const auto failed = videoSetFailures_.find(video.path); failed != videoSetFailures_.end()) {
            if (alert) *alert = true;
            return L"could not play: " + failed->second;
        }
        return {};
    case VideoSetState::Found:
        return L"found again on the server under a new id";
    case VideoSetState::Searching:
        return L"not on the server under its id; looking for it\x2026";
    case VideoSetState::FileMissing:
        if (alert) *alert = true;
        return L"file not found";
    case VideoSetState::NotFound: {
        if (alert) *alert = true;
        std::wstring text = L"not on the server any more: deleted, or moved and renamed";
        if (!video.serverPath.empty()) text += L"\nWas " + video.serverPath;
        return text;
    }
    case VideoSetState::Unreachable:
        if (alert) *alert = true;
        return L"Emby did not answer: " + found->second.detail;
    }
    return {};
}

std::vector<PanelRow> App::buildVideoSetRows() const {
    std::vector<PanelRow> rows;
    const auto note = [&](const std::wstring& text, bool alert = false) {
        PanelRow row;
        row.kind = PanelRowKind::Note;
        row.wrapNote = true;
        row.alert = alert;
        row.label = text;
        rows.push_back(std::move(row));
    };
    PanelRow header;
    header.kind = PanelRowKind::Header;
    header.label = L"Video sets";
    const auto count = videoSets_.sets.size();
    if (count > 0) header.value = std::to_wstring(count) + (count == 1 ? L" set" : L" sets");
    header.pinned = true;
    rows.push_back(std::move(header));
    // Each button acts at once; the last opens what follows a set's end.
    PanelRow toolbar;
    toolbar.kind = PanelRowKind::Buttons;
    toolbar.id = SettingId::VideoSetsToolbar;
    toolbar.options = {L"Add what plays", L"Import\x2026", L"Export\x2026",
                       std::wstring(videoSetOrderName(videoSets_.order)) + L" \x25BE"};
    toolbar.buttonMinWidth = 80.0F;
    toolbar.pinned = true;
    rows.push_back(std::move(toolbar));
    if (videoSetsReadOnly_) {
        note(L"The saved video sets could not be read, so nothing here is saved over them.", true);
    }
    if (videoSets_.sets.empty()) {
        note(L"No video sets yet. Open the videos to play together, arranged as they should be, and add them with "
             L"Add what plays (" + shortcut(KeyAction::AddVideoSet) + L"). While a set plays, Previous and Next "
             L"go from set to set.");
        return rows;
    }
    const int playing = videoSetPlaying();
    for (std::size_t index = 0; index < videoSets_.sets.size(); ++index) {
        const auto& set = videoSets_.sets[index];
        const int param = static_cast<int>(index);
        if (videoSetRenaming_ == param) {
            PanelRow field;
            field.kind = PanelRowKind::TextField;
            field.id = SettingId::VideoSetName;
            field.param = param;
            field.label = L"Name  (Enter keeps it, Esc leaves it)";
            field.field.placeholder = set.name;
            fieldDecorate(field, TextFieldId::VideoSetName);
            rows.push_back(std::move(field));
        }
        PanelRow entry;
        entry.kind = PanelRowKind::Buttons;
        entry.id = SettingId::VideoSetEntry;
        entry.param = param;
        entry.label = (playing == param ? L"\x25B6  " : L"") + std::to_wstring(index + 1) + L".  " + set.name;
        entry.value = videoCountText(set.videos.size());
        entry.options = {playing == param ? L"Play again" : L"Play", L"\x22EF"};
        entry.buttonMinWidth = 80.0F;
        rows.push_back(std::move(entry));
        for (std::size_t position = 0; position < set.videos.size(); ++position) {
            const auto& video = set.videos[position];
            bool alert = false;
            const std::wstring problem = videoSetProblem(video, &alert);
            note(placeName(position) + L"  " + videoTitle(video) + (problem.empty() ? L"" : L"\n" + problem),
                 alert);
        }
    }
    return rows;
}

void App::videoSetPanelAction(const PanelRow& row, int part) {
    switch (row.id) {
    case SettingId::VideoSetsToolbar:
        videoSetToolbarAction(part);
        break;
    case SettingId::VideoSetEntry:
        if (row.param < 0 || static_cast<std::size_t>(row.param) >= videoSets_.sets.size()) break;
        if (part == 0) {
            // The set is to be watched, not the sheet over it, also when
            // another plays in its place.
            if (playVideoSet(row.param)) closeSettingsPanel();
        } else if (part == 1) {
            videoSetEntryMenu(row.param);
        }
        break;
    default:
        break;
    }
}

void App::videoSetToolbarAction(int part) {
    if (part == 0) {
        addVideoSet();
    } else if (part == 1) {
        importVideoSetsDialog();
    } else if (part == 2) {
        exportVideoSetsDialog();
    } else if (part == 3) {
        PanelMenu menu;
        for (int index = 0; index < kPlayOrderCount; ++index) {
            const auto order = clampPlayOrder(index);
            menu.option(static_cast<unsigned>(index + 1), videoSetOrderName(order), videoSets_.order == order);
        }
        openPanelMenu(std::move(menu), SettingId::VideoSetsToolbar, part, -1, [this](unsigned command) {
            if (command == 0 || command > static_cast<unsigned>(kPlayOrderCount)) return;
            videoSets_.order = clampPlayOrder(static_cast<int>(command) - 1);
            saveVideoSets();
            showNotice(std::wstring(L"At a video set's end: ") + videoSetOrderName(videoSets_.order));
        });
    }
}

void App::videoSetEntryMenu(int index) {
    const int last = static_cast<int>(videoSets_.sets.size()) - 1;
    if (index < 0 || index > last) return;
    PanelMenu menu;
    menu.option(1, L"Play");
    menu.separator();
    menu.option(2, L"Move up", false, {}, index > 0);
    menu.option(3, L"Move down", false, {}, index < last);
    menu.option(4, L"Rename\x2026");
    menu.separator();
    menu.option(5, L"Delete");
    // Chosen later: the list may have changed under the menu.
    const std::wstring name = videoSets_.sets[static_cast<std::size_t>(index)].name;
    openPanelMenu(std::move(menu), SettingId::VideoSetEntry, 1, index, [this, index, name](unsigned command) {
        if (index < 0 || static_cast<std::size_t>(index) >= videoSets_.sets.size() ||
            videoSets_.sets[static_cast<std::size_t>(index)].name != name) return;
        if (command == 1) {
            if (playVideoSet(index)) closeSettingsPanel();
        } else if (command == 2) {
            moveVideoSet(index, -1);
        } else if (command == 3) {
            moveVideoSet(index, 1);
        } else if (command == 4) {
            beginVideoSetRename(index);
        } else if (command == 5) {
            deleteVideoSet(index);
        }
    });
}

void App::moveVideoSet(int index, int step) {
    const int other = index + (step < 0 ? -1 : 1);
    const int count = static_cast<int>(videoSets_.sets.size());
    if (index < 0 || index >= count || other < 0 || other >= count) return;
    cancelVideoSetRename();
    std::swap(videoSets_.sets[static_cast<std::size_t>(index)], videoSets_.sets[static_cast<std::size_t>(other)]);
    if (videoSetIndex_ == index) videoSetIndex_ = other;
    else if (videoSetIndex_ == other) videoSetIndex_ = index;
    saveVideoSets();
}

void App::deleteVideoSet(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= videoSets_.sets.size()) return;
    cancelVideoSetRename();
    const std::wstring name = videoSets_.sets[static_cast<std::size_t>(index)].name;
    videoSets_.sets.erase(videoSets_.sets.begin() + index);
    // Its videos play on, as videos of no set.
    if (videoSetIndex_ == index) videoSetIndex_ = -1;
    else if (videoSetIndex_ > index) --videoSetIndex_;
    saveVideoSets();
    showNotice(L"Deleted from the video sets: " + name);
}

void App::beginVideoSetRename(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= videoSets_.sets.size()) return;
    videoSetRenaming_ = index;
    videoSetNameField_ = {};
    videoSetNameField_.text = videoSets_.sets[static_cast<std::size_t>(index)].name;
    focusField(TextFieldId::VideoSetName, true);
}

void App::commitVideoSetRename() {
    const int index = videoSetRenaming_;
    if (index < 0) return;
    videoSetRenaming_ = -1;
    blurField();
    if (static_cast<std::size_t>(index) >= videoSets_.sets.size()) return;
    auto name = cleanVideoSetName(videoSetNameField_.text);
    auto& set = videoSets_.sets[static_cast<std::size_t>(index)];
    if (name.empty() || name == set.name) return;
    set.name = std::move(name);
    saveVideoSets();
}

void App::cancelVideoSetRename() {
    if (videoSetRenaming_ < 0) return;
    videoSetRenaming_ = -1;
    blurField();
}

// --- playing a set -----------------------------------------------------------

bool App::playVideoSet(int index, int step, int from) {
    const int count = static_cast<int>(videoSets_.sets.size());
    if (index < 0 || index >= count) return false;
    // A set asked for starts the list going afresh: any set may be tried
    // again but the one stepped from. When none can play, the set playing
    // goes on as it went.
    const int direction = std::exchange(videoSetDirection_, step < 0 ? -1 : 1);
    auto tried = std::exchange(videoSetTried_, std::vector<bool>(static_cast<std::size_t>(count), false));
    if (from >= 0 && from < count) videoSetTried_[static_cast<std::size_t>(from)] = true;
    if (openVideoSet(index)) return true;
    videoSetDirection_ = direction;
    videoSetTried_ = std::move(tried);
    return false;
}

bool App::openVideoSet(int index, std::vector<VideoSetSkip> skipped) {
    const int count = static_cast<int>(videoSets_.sets.size());
    videoSetTried_.resize(static_cast<std::size_t>(count), false);
    const auto heading = [this](int set) {
        return L"Video set " + std::to_wstring(set + 1) + L" \x00B7 " +
               videoSets_.sets[static_cast<std::size_t>(set)].name;
    };
    // A set with an Emby item that cannot be asked for -- none is signed
    // in to, or another server is -- is skipped whole before it opens.
    const auto session = emby_.session();
    while (index >= 0 && index < count) {
        videoSetTried_[static_cast<std::size_t>(index)] = true;
        const auto& candidate = videoSets_.sets[static_cast<std::size_t>(index)];
        std::wstring why = candidate.videos.empty() ? std::wstring(L"it is empty") : std::wstring();
        for (std::size_t position = 0; position < candidate.videos.size() && why.empty(); ++position) {
            const auto& video = candidate.videos[position];
            if (const auto problem = sessionProblem(video, session); !problem.empty()) {
                why = placeName(position) + L" \x00B7 " + videoTitle(video) + L": " + problem;
            }
        }
        if (why.empty()) break;
        appendDiagnostic("Video sets: skipping set " + std::to_string(index + 1) + ": " + narrow(why));
        skipped.push_back({index, std::move(why)});
        index = videoSetSkipTo(videoSets_.order, index, count, videoSetDirection_, videoSetTried_,
                               static_cast<unsigned>(shuffleRandom_()));
    }
    if (index < 0 || index >= count) {
        if (skipped.empty()) return false;
        if (skipped.size() == 1) {
            showNotice(heading(skipped.front().index) + L" cannot play: " + skipped.front().why, 5000);
        } else if (skipped.size() >= static_cast<std::size_t>(count)) {
            showNotice(L"No video set can play; Video sets says why", 5000);
        } else {
            std::wstring numbers;
            for (const auto& skip : skipped) {
                numbers += (numbers.empty() ? L"" : L", ") + std::to_wstring(skip.index + 1);
            }
            showNotice(L"Video sets " + numbers + L" cannot play; Video sets says why", 5000);
        }
        return false;
    }
    const VideoSet set = videoSets_.sets[static_cast<std::size_t>(index)];
    // Everything as it is but the videos, where they start and how they
    // run: each from its start, at its own speed, without offsets or loops.
    SessionState state = captureSession();
    for (auto& pane : state.panes) {
        pane.path.clear();
        pane.delay = pane.adjustment = 0.0;
        pane.rate = 1.0;
        pane.paused = false;
        pane.pausedTime = 0.0;
        pane.repeat = false;
        pane.loopEnabled = false;
        pane.loopA = pane.loopB = 0.0;
    }
    state.timeline = 0.0;
    state.playing = true;
    state.masterLoopEnabled = false;
    state.masterLoopA = state.masterLoopB = 0.0;
    state.layout = set.layout;
    state.expandedPane = -1;
    PaneArray<std::wstring> titles{};
    for (std::size_t position = 0; position < set.videos.size() && position < kMaxPanes; ++position) {
        const auto& video = set.videos[position];
        if (set.expanded == static_cast<int>(position)) state.expandedPane = static_cast<int>(position);
        titles[position] = videoTitle(video);
        state.panes[position].path = video.path;
        // Tried again: what failed it before is said again if it fails.
        videoSetFailures_.erase(video.path);
    }
    appendDiagnostic("Video sets: playing set " + std::to_string(index + 1) + " of " + std::to_string(count) +
                     ", " + std::to_string(std::min(set.videos.size(), kMaxPanes)) + " videos");
    applySession(state);
    videoSetIndex_ = index;
    ++videoSetRun_;
    videoSetFailure_.clear();
    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        videoSetPanePaths_[pane] = state.panes[pane].path;
        // An Emby item starts from its start, as the files do, not where the
        // server says it was left; and is named as the set names it until
        // its reply names it again.
        if (!embyPanes_[pane]) continue;
        embyPanes_[pane]->fromBeginning = true;
        if (!titles[pane].empty()) embyPanes_[pane]->title = titles[pane];
    }
    if (skipped.empty()) {
        showNotice(heading(index));
    } else if (skipped.size() == 1) {
        showNotice(heading(index) + L" \x2014 skipped set " + std::to_wstring(skipped.front().index + 1) + L": " +
                   skipped.front().why, 5000);
    } else {
        showNotice(heading(index) + L" \x2014 skipped " + std::to_wstring(skipped.size()) +
                   L" sets that cannot play; Video sets says why", 5000);
    }
    updateControls();
    updateHoverControls();
    updateTitle();
    return true;
}

int App::videoSetPlaying() const {
    if (videoSetIndex_ < 0 || static_cast<std::size_t>(videoSetIndex_) >= videoSets_.sets.size()) return -1;
    bool any = false;
    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        if (paths_[pane].empty()) continue;
        if (paths_[pane] != videoSetPanePaths_[pane]) return -1;
        any = true;
    }
    return any ? videoSetIndex_ : -1;
}

bool App::videoSetPane(std::size_t pane) const {
    return pane < kMaxPanes && !paths_[pane].empty() && videoSetPlaying() >= 0 &&
           paths_[pane] == videoSetPanePaths_[pane];
}

std::wstring App::videoSetPaneName(std::size_t pane) const {
    if (pane >= kMaxPanes) return {};
    // Named as the face names it, by its place in the set -- the pane the
    // set opened it in -- not where it stands now that others may have
    // closed or not yet opened.
    std::size_t position = pane;
    std::wstring title;
    if (videoSetIndex_ >= 0 && static_cast<std::size_t>(videoSetIndex_) < videoSets_.sets.size()) {
        const auto& videos = videoSets_.sets[static_cast<std::size_t>(videoSetIndex_)].videos;
        for (std::size_t index = 0; index < videos.size(); ++index) {
            const std::size_t at = (pane + index) % videos.size();
            if (videos[at].path != videoSetPanePaths_[pane]) continue;
            position = at;
            title = videoTitle(videos[at]);
            break;
        }
    }
    if (title.empty() && embyPanes_[pane]) title = embyPanes_[pane]->title;
    if (title.empty()) title = std::filesystem::path(paths_[pane]).filename().wstring();
    return placeName(position) + L" \x00B7 " + title;
}

void App::stepVideoSet(int step) {
    const int current = videoSetPlaying();
    const int next = videoSetStep(videoSets_.order, current, static_cast<int>(videoSets_.sets.size()), step,
                                  static_cast<unsigned>(shuffleRandom_()));
    if (next < 0) {
        showNotice(step > 0 ? L"No video set after this one" : L"No video set before this one");
        return;
    }
    playVideoSet(next, step, current);
}

void App::videoSetFailed(std::uint64_t run, const std::wstring& path, const std::wstring& name,
                         const std::wstring& reason) {
    videoSetFailures_[path] = reason;
    // The first failure of a run skips it; any after it only marks its video.
    if (run != videoSetRun_ || videoSetFailedRun_ == run) return;
    videoSetFailedRun_ = run;
    videoSetFailure_ = name + L": " + reason;
}

bool App::videoSetPaneFailed(std::size_t pane, const std::wstring& reason) {
    if (!videoSetPane(pane)) return false;
    videoSetFailed(videoSetRun_, paths_[pane], videoSetPaneName(pane), reason);
    return true;
}

void App::checkVideoSetSources() {
    if (videoSetFailedRun_ == videoSetRun_ || deviceRecoveryPending_ || renderer_.deviceLost()) return;
    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        if (!sources_[pane] || !videoSetPane(pane)) continue;
        const std::string error = sources_[pane]->error();
        if (error.empty()) continue;
        videoSetPaneFailed(pane, wide(error));
        return;
    }
}

void App::skipVideoSet() {
    const std::wstring why = std::exchange(videoSetFailure_, {});
    const int index = videoSetIndex_;
    const int count = static_cast<int>(videoSets_.sets.size());
    if (why.empty() || index < 0 || index >= count) return;
    // Only while the set still holds the deck: each pane what it opened
    // there, or nothing -- the failed video's may have closed.
    for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
        if (!paths_[pane].empty() && paths_[pane] != videoSetPanePaths_[pane]) return;
    }
    appendDiagnostic("Video sets: skipping set " + std::to_string(index + 1) + ": " + narrow(why));
    videoSetTried_.resize(static_cast<std::size_t>(count), false);
    videoSetTried_[static_cast<std::size_t>(index)] = true;
    const int next = videoSetSkipTo(videoSets_.order, index, count, videoSetDirection_, videoSetTried_,
                                    static_cast<unsigned>(shuffleRandom_()));
    if (openVideoSet(next, {{index, why}})) return;
    // None is left to go on to: the deck stops as the failed set left it.
    pausePlaybackAtEnd();
    updateControls();
}

// Held at its end on a deck of its own timelines: an Emby video once
// finishEmbyPanes has held it; a file once its own time has reached its
// length. One that repeats, loops, is paused or still opening has not.
// A video that failed or closed is no longer waited for.
bool App::videoSetPaneEnded(std::size_t pane) const {
    if (pane >= kMaxPanes || paths_[pane].empty()) return true;
    if (embyPanes_[pane]) {
        const auto& slot = *embyPanes_[pane];
        if (slot.photo) return true;
        if (slot.endHandled) return !(linkedBrowserSeekBars() && repeatsWithLinkedVideos(pane));
        if (slot.resolving) return false;
    }
    if (!sources_[pane]) return false;
    if (!sources_[pane]->error().empty()) return true;
    if (embyPanes_[pane] || !sources_[pane]->ready() || sourcePaused_[pane] || sourceLoopEnabled_[pane]) {
        return false;
    }
    const auto timing = paneTiming(pane);
    if (paneRepeats(timing, activeSeekMode()) || timing.duration <= 0.0) return false;
    return mappedSourceTime(pane, clock_.position()) + kSourceEndEpsilon >= timing.duration;
}

void App::finishVideoSet(bool sourceOpening, bool hasRepeatingSource) {
    // A set with a video that could not play gives way at once, playing
    // or not.
    checkVideoSetSources();
    if (!videoSetFailure_.empty()) {
        skipVideoSet();
        return;
    }
    const int index = videoSetPlaying();
    if (index < 0 || !clock_.isPlaying() || seekBarrier_.active()) return;
    if (!perPaneTimelines()) {
        // One timeline: the set has ended where Restart all would act.
        if (masterTimelineEndAction(clock_.position(), duration_, true, sourceOpening,
                                    masterLoopEnabled_ && loopArmed(masterLoopA_, masterLoopB_, duration_),
                                    hasRepeatingSource, false) == MasterTimelineEndAction::None) return;
    } else {
        for (std::size_t pane = 0; pane < kMaxPanes; ++pane) {
            if (!videoSetPaneEnded(pane)) return;
        }
    }
    const int next = playOrderNextIndex(videoSets_.order, index, static_cast<int>(videoSets_.sets.size()),
                                        static_cast<unsigned>(shuffleRandom_()));
    if (next >= 0 && playVideoSet(next)) return;
    pausePlaybackAtEnd();
    if (next < 0) {
        showNotice(videoSets_.order == PlayOrder::PlayOne ? L"End of the video set" : L"End of the video sets");
    }
    updateControls();
}

// --- import and export -------------------------------------------------------

void App::importVideoSets(const std::wstring& path) {
    const std::wstring name = std::filesystem::path(path).filename().wstring();
    VideoSetList imported;
    bool oldSession = false;
    if (!readVideoSetsFile(std::filesystem::path(path), imported, &oldSession)) {
        showNotice(oldSession ? name + L" is an old QuadDeck session, which cannot be opened any more"
                              : name + L" is not a list of video sets QuadDeck can read",
                   4000);
        return;
    }
    const auto added = appendVideoSets(videoSets_, imported);
    if (added > 0) saveVideoSets();
    showVideoSets();
    if (added == 0) {
        showNotice(imported.sets.empty() ? name + L" has no video sets"
                                         : L"The video sets are already here: " + name);
    } else {
        showNotice(L"Added " + std::to_wstring(added) + (added == 1 ? L" video set from " : L" video sets from ") +
                   name);
    }
}

void App::importVideoSetsDialog() {
    wchar_t path[MAX_PATH]{};
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = kVideoSetsFilter;
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&dialog)) importVideoSets(path);
}

void App::exportVideoSetsDialog() {
    if (videoSets_.sets.empty()) {
        showNotice(L"There are no video sets to export");
        return;
    }
    wchar_t path[MAX_PATH] = L"Video sets.qdeck";
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = kVideoSetsFilter;
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrDefExt = L"qdeck";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog)) return;
    const VideoSetList& list = videoSets_;
    if (!writeUtf8FileAtomically(std::filesystem::path(path),
                                 [&](std::wostream& output) { return writeVideoSets(output, list); })) {
        MessageBoxW(window_, L"Could not export the video sets.", L"Video sets", MB_ICONERROR);
        return;
    }
    showNotice(L"Exported " + std::to_wstring(list.sets.size()) +
               (list.sets.size() == 1 ? L" video set to " : L" video sets to ") +
               std::filesystem::path(path).filename().wstring());
}

// --- what is there -----------------------------------------------------------

void App::checkVideoSets() {
    const std::uint64_t serial = ++videoSetCheckSerial_;
    videoSetChecks_.clear();
    const auto session = emby_.session();
    std::vector<std::wstring> files;
    std::vector<std::wstring> items;
    std::vector<std::string> ids;
    std::unordered_set<std::wstring> seen;
    for (const auto& set : videoSets_.sets) {
        for (const auto& video : set.videos) {
            if (!seen.insert(video.path).second) continue;
            if (!emby::isLocator(video.path)) {
                files.push_back(video.path);
                videoSetChecks_[video.path] = {};
                continue;
            }
            // Signed out or on another server, the session says so already.
            if (!sessionProblem(video, session).empty()) continue;
            videoSetChecks_[video.path] = {};
            items.push_back(video.path);
            ids.push_back(emby::parseLocator(video.path)->itemId);
        }
    }
    if (!files.empty()) {
        // A file on a share that has gone to sleep can take a while to
        // answer, so not on the window thread.
        std::thread([queue = mainQueue_, this, serial, files] {
            std::vector<bool> present;
            present.reserve(files.size());
            for (const auto& file : files) present.push_back(GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES);
            queue->post([this, serial, files, present] {
                if (serial != videoSetCheckSerial_) return;
                for (std::size_t i = 0; i < files.size(); ++i) {
                    videoSetChecks_[files[i]] = {present[i] ? VideoSetState::Fine : VideoSetState::FileMissing, {}};
                }
            });
        }).detach();
    }
    std::size_t at = 0;
    for (const auto& batch : emby::subtitleBatches(ids)) {
        std::vector<std::wstring> paths(items.begin() + static_cast<std::ptrdiff_t>(at),
                                        items.begin() + static_cast<std::ptrdiff_t>(at + batch.size()));
        at += batch.size();
        emby_.request("GET", emby::itemsByIdPath(session.userId, batch), "",
                      [this, serial, account = embyAccountSerial_, paths](EmbyClient::Response& response) {
            videoSetItemsArrived(serial, account, paths, response);
        });
    }
}

void App::videoSetItemsArrived(std::uint64_t serial, std::uint64_t account, const std::vector<std::wstring>& paths,
                               const EmbyClient::Response& response) {
    if (serial != videoSetCheckSerial_ || account != embyAccountSerial_) return;
    const auto list = response.ok ? emby::parseItems(response.body) : std::nullopt;
    if (!list) {
        const std::wstring detail = response.ok ? std::wstring(L"a reply QuadDeck cannot read") : wide(response.error);
        for (const auto& path : paths) videoSetChecks_[path] = {VideoSetState::Unreachable, detail};
        if (response.status == 401) embySessionExpired();
        return;
    }
    std::unordered_map<std::string, const emby::Item*> listed;
    for (const auto& item : list->items) listed[item.id] = &item;
    bool learned = false;
    for (const auto& path : paths) {
        const auto locator = emby::parseLocator(path);
        const auto found = locator ? listed.find(locator->itemId) : listed.end();
        if (found == listed.end()) {
            videoSetRefind(path);
            continue;
        }
        videoSetChecks_[path] = {VideoSetState::Fine, {}};
        // A set added before its item's reply came knows it now.
        for (auto& set : videoSets_.sets) {
            for (auto& video : set.videos) {
                if (video.path != path || (!video.serverPath.empty() && !video.name.empty())) continue;
                rememberItem(video, refindKeyOf(*found->second));
                learned = true;
            }
        }
    }
    if (learned) saveVideoSets();
}

void App::videoSetRefind(const std::wstring& path,
                          std::function<void(const std::wstring& found, const std::wstring& problem)> done) {
    const VideoSetVideo* saved = nullptr;
    for (const auto& set : videoSets_.sets) {
        for (const auto& video : set.videos) {
            if (video.path == path) saved = &video;
        }
    }
    const std::wstring gone = L"not on the server any more";
    const auto locator = emby::parseLocator(path);
    const auto session = emby_.session();
    if (!locator || !session.signedIn()) {
        if (done) done({}, L"sign in to Emby to play it");
        return;
    }
    const auto key = saved ? refindKey(*saved) : emby::RefindKey{};
    const auto query = emby::refindPath(session.userId, key);
    if (query.empty()) {
        videoSetChecks_[path] = {VideoSetState::NotFound, {}};
        if (done) done({}, gone);
        return;
    }
    videoSetChecks_[path] = {VideoSetState::Searching, {}};
    const std::string serverId = locator->serverId.empty() ? session.serverId : locator->serverId;
    emby_.request("GET", query, "",
                  [this, path, key, serverId, gone, oldId = locator->itemId, account = embyAccountSerial_,
                   done = std::move(done)](EmbyClient::Response& response) {
        if (account != embyAccountSerial_) return;
        if (!response.ok) {
            videoSetChecks_[path] = {VideoSetState::Unreachable, wide(response.error)};
            if (done) done({}, L"Emby did not answer: " + wide(response.error));
            if (response.status == 401) embySessionExpired();
            return;
        }
        const auto list = emby::parseItems(response.body);
        const auto match = list ? emby::refindMatch(list->items, key) : std::nullopt;
        if (!match) {
            videoSetChecks_[path] = {VideoSetState::NotFound, {}};
            appendDiagnostic("Video sets: item " + oldId + " is not on the server, and no item there is it");
            if (done) done({}, gone);
            return;
        }
        const std::wstring fresh = emby::formatLocator(serverId, match->id);
        bool changed = false;
        for (auto& set : videoSets_.sets) {
            for (auto& video : set.videos) {
                if (video.path != path) continue;
                video.path = fresh;
                rememberItem(video, refindKeyOf(*match));
                changed = true;
            }
        }
        videoSetChecks_.erase(path);
        videoSetChecks_[fresh] = {VideoSetState::Found, {}};
        videoSetFailures_.erase(path);
        appendDiagnostic("Video sets: item " + oldId + " found again on the server as " + match->id);
        if (changed) saveVideoSets();
        if (done) done(fresh, {});
    });
}

bool App::videoSetItemGone(std::size_t pane, int status) {
    if ((status != 400 && status != 404) || !videoSetPane(pane)) return false;
    const std::wstring path = paths_[pane];
    const std::uint64_t run = videoSetRun_;
    const std::wstring name = videoSetPaneName(pane);
    const std::wstring title = embyPanes_[pane] ? embyPanes_[pane]->title : std::wstring();
    if (embyPanes_[pane]) embyPanes_[pane]->resolving = false;
    appendDiagnostic("Video sets: the server does not know the item of V" +
                     std::to_string(positionForPane(pane) + 1) + " (HTTP " + std::to_string(status) +
                     "); looking for it");
    closePane(pane);
    showNotice(name + L": not on the server under its id; looking for it\x2026", 4000);
    videoSetRefind(path, [this, pane, run, path, name, title](const std::wstring& fresh, const std::wstring& problem) {
        // Only while the set it was opened for still holds the other panes
        // and nothing has been put in its own.
        if (run != videoSetRun_ || !paths_[pane].empty() || videoSetPanePaths_[pane] != path) return;
        for (std::size_t other = 0; other < kMaxPanes; ++other) {
            if (!paths_[other].empty() && paths_[other] != videoSetPanePaths_[other]) return;
        }
        // Not found, it cannot play, and the set is skipped whole.
        if (fresh.empty()) {
            videoSetFailed(run, path, name, problem);
            return;
        }
        // Found, it opens where it was.
        videoSetPanePaths_[pane] = fresh;
        openSource(pane, fresh, true);
        if (embyPanes_[pane]) {
            embyPanes_[pane]->fromBeginning = true;
            embyPanes_[pane]->title = title;
        }
        showNotice(name + L": found again on the server");
        updateControls();
        updateTitle();
    });
    return true;
}

}  // namespace quaddeck
