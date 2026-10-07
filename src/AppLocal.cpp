// App: the folder of a local video as the F6 list, and Explorer's file
// types. Playing one local file loads its folder -- every video beside it --
// into the same docked sheet the Emby browser uses: the list to pick from,
// what each pane's Page Up/Down and step buttons walk, in the order captured
// when that pane starts playing. Folder listing and length probing run off
// the window thread, as a share that has gone away must not freeze the
// player; both arrive through the main-thread queue.

#include "App.hpp"
#include "AppInternal.hpp"
#include "Diagnostics.hpp"
#include "TextEncoding.hpp"

#include <shlwapi.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iterator>
#include <thread>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

namespace quaddeck {

using namespace app_internal;

namespace {

// Tiles at 96 DPI, as the Emby browser's.
constexpr float kPosterTile = 150.0F;
constexpr float kThumbTile = 220.0F;
// Lengths are delivered as they are read, a few at a time.
constexpr std::size_t kDurationBatch = 8;
constexpr auto kDurationBatchInterval = std::chrono::milliseconds(300);

struct ProbeInterrupt {
    const std::atomic<std::uint64_t>* token{};
    std::uint64_t serial{};
};

int probeInterrupted(void* opaque) {
    const auto* interrupt = static_cast<const ProbeInterrupt*>(opaque);
    return interrupt && interrupt->token->load(std::memory_order_relaxed) != interrupt->serial ? 1 : 0;
}

// A stream the player shows subtitles from: text, with a decoder, as
// VideoSource's textSubtitleCodec reads them.
bool textSubtitleStream(const AVStream* stream) {
    if (!stream->codecpar || stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) return false;
    const AVCodecDescriptor* descriptor = avcodec_descriptor_get(stream->codecpar->codec_id);
    return descriptor && (descriptor->props & AV_CODEC_PROP_TEXT_SUB) != 0 &&
           avcodec_find_decoder(stream->codecpar->codec_id) != nullptr;
}

// How long a subtitle file with no video of its name open waits, from its
// launch's arrival, for a launch bringing that video.
constexpr ULONGLONG kExternalSubtitleWaitMs = 750;

bool isSessionFile(const std::wstring& path) {
    return _wcsicmp(std::filesystem::path(path).extension().c_str(), L".qdeck") == 0;
}

}  // namespace

std::vector<LocalEntry> App::scanLocalFolder(const std::wstring& directory) {
    std::vector<LocalEntry> entries;
    if (directory.empty()) return entries;
    std::vector<std::wstring> subtitles;
    std::error_code code;
    for (std::filesystem::directory_iterator entry(std::filesystem::path(directory), code), end;
         !code && entry != end; entry.increment(code)) {
        if (!entry->is_regular_file(code)) continue;
        const std::wstring extension = entry->path().extension().wstring();
        if (isSubtitleExtension(extension)) {
            subtitles.push_back(entry->path().wstring());
            continue;
        }
        if (!isMediaExtension(extension)) continue;
        LocalEntry item;
        item.path = entry->path().wstring();
        item.size = entry->file_size(code);
        if (code) { item.size = 0; code.clear(); }
        const auto written = entry->last_write_time(code);
        if (code) code.clear();
        else item.written = static_cast<std::int64_t>(written.time_since_epoch().count());
        entries.push_back(std::move(item));
    }
    // Explorer's ordering, so that "(2)" precedes "(10)" as it does on screen.
    std::sort(entries.begin(), entries.end(), [](const LocalEntry& left, const LocalEntry& right) {
        return StrCmpLogicalW(left.path.c_str(), right.path.c_str()) < 0;
    });
    markSubtitleFiles(entries, subtitles);
    return entries;
}

// The container's own idea of its length, in seconds, and whether it holds
// a text subtitle stream; nothing when it has neither or the scan this
// belongs to has been superseded. Opening is all an MP4 or a Matroska file
// needs, its streams included; the rest are looked into a little further.
LocalProbe App::probeLocalFile(const std::wstring& path, const std::atomic<std::uint64_t>& token,
                               std::uint64_t serial) {
    LocalProbe probe;
    AVFormatContext* format = avformat_alloc_context();
    if (!format) return probe;
    ProbeInterrupt interrupt{&token, serial};
    format->interrupt_callback.callback = &probeInterrupted;
    format->interrupt_callback.opaque = &interrupt;
    const std::string utf8 = wideToUtf8Text(path);
    // A failed open frees the context itself.
    if (avformat_open_input(&format, utf8.c_str(), nullptr, nullptr) < 0) return probe;
    if (format->duration == AV_NOPTS_VALUE || format->duration <= 0) {
        format->probesize = 2 * 1024 * 1024;
        format->max_analyze_duration = 2 * AV_TIME_BASE;
        avformat_find_stream_info(format, nullptr);
    }
    if (format->duration != AV_NOPTS_VALUE && format->duration > 0) {
        probe.duration = static_cast<double>(format->duration) / static_cast<double>(AV_TIME_BASE);
    }
    for (unsigned index = 0; index < format->nb_streams; ++index) {
        if (textSubtitleStream(format->streams[index])) {
            probe.subtitleStream = true;
            break;
        }
    }
    avformat_close_input(&format);
    return probe;
}

void App::refreshLocalList(const std::wstring& path, bool force) {
    if (path.empty() || emby::isLocator(path) || path.find(L"://") != std::wstring::npos) return;
    const std::wstring directory = localDirectory(path);
    if (directory.empty()) return;
    const bool sameFolder = sameLocalDirectory(directory, localDirectory_);
    if (!force && sameFolder && (!localEntries_.empty() || localListLoading_)) return;
    if (!sameFolder) {
        // Another folder: nothing of the old one applies, its place in the
        // list included.
        localEntries_.clear();
        localList_.clear();
        localProbes_.clear();
        localListScroll_ = 0.0F;
        if (embyBrowserOpen_ && browserSource_ == BrowserSource::Local) {
            settingsScroll_ = 0.0F;
            panelPressArmed_ = false;
            pressedPanelKind_ = PanelHitKind::None;
        }
    }
    localDirectory_ = directory;
    localListLoading_ = true;
    const std::uint64_t serial = ++localListSerial_;
    localScanToken_.value->store(serial);
    // Files already read are not read again by a refresh.
    std::thread([queue = mainQueue_, token = localScanToken_.value, known = localProbes_, this, serial,
                 directory] {
        auto entries = scanLocalFolder(directory);
        std::vector<std::wstring> unread;
        for (const auto& entry : entries) {
            if (!known.count(entry.path)) unread.push_back(entry.path);
        }
        queue->post([this, serial, directory, entries = std::move(entries)]() mutable {
            applyLocalList(serial, directory, std::move(entries));
        });
        std::vector<std::pair<std::wstring, LocalProbe>> batch;
        auto lastPost = std::chrono::steady_clock::now();
        const auto flush = [&] {
            if (batch.empty()) return;
            queue->post([this, serial, directory, batch = std::move(batch)]() mutable {
                applyLocalProbes(serial, directory, std::move(batch));
            });
            batch.clear();
            lastPost = std::chrono::steady_clock::now();
        };
        for (const auto& file : unread) {
            if (token->load(std::memory_order_relaxed) != serial) return;   // superseded, or the player is gone
            batch.emplace_back(file, probeLocalFile(file, *token, serial));
            if (batch.size() >= kDurationBatch ||
                std::chrono::steady_clock::now() - lastPost >= kDurationBatchInterval) {
                flush();
            }
        }
        if (token->load(std::memory_order_relaxed) == serial) flush();
    }).detach();
}

void App::applyLocalList(std::uint64_t serial, std::wstring directory, std::vector<LocalEntry> entries) {
    if (serial != localListSerial_ || !sameLocalDirectory(directory, localDirectory_)) return;
    localListLoading_ = false;
    localEntries_ = std::move(entries);
    for (auto& entry : localEntries_) {
        if (const auto known = localProbes_.find(entry.path); known != localProbes_.end()) {
            entry.duration = known->second.duration;
            entry.subtitleStream = known->second.subtitleStream;
        }
    }
    orderLocalList();
    appendDiagnostic("Folder list: " + std::to_string(localEntries_.size()) + " videos in " +
                     wideToUtf8Text(localDirectory_));
}

void App::applyLocalProbes(std::uint64_t serial, std::wstring directory,
                           std::vector<std::pair<std::wstring, LocalProbe>> batch) {
    if (serial != localListSerial_ || !sameLocalDirectory(directory, localDirectory_)) return;
    for (auto& [file, probe] : batch) {
        localProbes_[file] = probe;
        for (auto& entry : localEntries_) {
            if (entry.path == file) {
                entry.duration = probe.duration;
                entry.subtitleStream = probe.subtitleStream;
                break;
            }
        }
    }
    // By name, size, date or the session's shuffle this changes nothing; by
    // length the files take their places as their lengths arrive.
    orderLocalList();
}

void App::orderLocalList() {
    const auto previous = std::move(localList_);
    localList_ = localEntries_;
    orderLocalEntries(localList_, emby::sortKeyFromIndex(embyBrowser_.sort), embyBrowser_.descending,
                      sessionSeed_ ^ std::hash<std::wstring>{}(localDirectory_));
    const bool sameOrder = previous.size() == localList_.size() &&
        std::equal(previous.begin(), previous.end(), localList_.begin(), [](const LocalEntry& left, const LocalEntry& right) {
            return left.path == right.path;
        });
    // Lengths arrive asynchronously. A pressed index must not become a
    // different file before release when one of those lengths changes sort.
    if (!sameOrder && settingsOpen_ && embyBrowserOpen_ && browserSource_ == BrowserSource::Local) {
        panelPressArmed_ = false;
        pressedPanelKind_ = PanelHitKind::None;
    }
}

void App::openLocalList() {
    localCancelReplacement();
    embyCancelReplacement();
    if (settingsOpen_) rememberSheetScroll();
    refreshLocalList(paths_[embyPlaybackTarget()]);
    if (!settingsOpen_) toggleSettingsPanel();
    embyBrowserOpen_ = true;
    browserSource_ = BrowserSource::Local;
    // Where the list was left, not its top.
    restoreSheetScroll();
}

void App::localNavigate(int part) {
    if (localPendingPlay_) { localCancelReplacement(); return; }
    switch (part) {
    case 0:
        rememberSheetScroll();
        embyBrowserOpen_ = false;
        restoreSheetScroll();
        break;
    case 1:
        embyOpenBrowser();
        break;
    case 2: {
        refreshLocalList(paths_[embyPlaybackTarget()], true);
        break;
    }
    default:
        break;
    }
}

void App::activateLocalItem(int index, bool add) {
    if (index < 0 || static_cast<std::size_t>(index) >= localList_.size()) return;
    // Copies: opening the file may refresh the browser, and each pane owns
    // the order from which it was started even when another folder is shown.
    const std::wstring file = localList_[static_cast<std::size_t>(index)].path;
    const auto& queue = localList_;
    std::size_t pane = embyPlaybackTarget();
    if (add) {
        pane = kMaxPanes;
        for (std::size_t candidate = 0; candidate < kMaxPanes; ++candidate) {
            if (!paneLogicallyLoaded(candidate) && paths_[candidate].empty() && !embyPanes_[candidate]) {
                pane = candidate;
                break;
            }
        }
        if (pane == kMaxPanes) {
            LocalPendingPlay pending;
            pending.path = file;
            pending.queue = queue;
            pending.scroll = settingsScroll_;
            pending.paths = paths_;
            pending.mediaSerials = subtitleSerial_;
            localPendingPlay_ = std::move(pending);
            settingsScroll_ = 0.0F;
            panelPressArmed_ = false;
            pressedPanelKind_ = PanelHitKind::None;
            return;
        }
    }
    localPlayItem(file, queue, pane, add);
}

void App::localPlayItem(const std::wstring& path, const std::vector<LocalEntry>& queue,
                        std::size_t pane, bool add, bool selectTarget) {
    if (pane >= sources_.size() || path.empty() || emby::isLocator(path) ||
        path.find(L"://") != std::wstring::npos) return;
    // Callers can pass this destination's queue and path. openSource retires
    // both, so preserve the requested media before changing the destination.
    const std::wstring file = path;
    auto capturedQueue = queue;
    // Explorer's sole local video already had a folder sequence. Keep that
    // sequence when the browser adds a second pane, so its EOF no longer
    // depends on whichever folder the browser visits next. A manually
    // opened multi-pane deck retains its existing linked playback policy.
    const int single = singleLoadedPane();
    if (single >= 0 && static_cast<std::size_t>(single) != pane) {
        const auto legacyPane = static_cast<std::size_t>(single);
        const auto& legacyPath = paths_[legacyPane];
        if (!localPanes_[legacyPane] && !embyPanes_[legacyPane] && !legacyPath.empty() &&
            !emby::isLocator(legacyPath) && legacyPath.find(L"://") == std::wstring::npos) {
            LocalPanePlayback legacy;
            if (sameLocalDirectory(localDirectory(legacyPath), localDirectory_) &&
                localEntryIndex(localList_, legacyPath) >= 0) {
                legacy.queue = localList_;
            } else {
                // The old folder can be an unavailable share. Capture its
                // sole known item immediately, then enumerate off the UI
                // thread without changing the folder the browser is showing.
                legacy.queue.push_back(LocalEntry{legacyPath});
                const std::uint64_t serial = ++localQueueLoadSerial_;
                legacy.queueLoadSerial = serial;
                const auto sort = emby::sortKeyFromIndex(embyBrowser_.sort);
                const bool descending = embyBrowser_.descending;
                const std::wstring directory = localDirectory(legacyPath);
                const auto seed = sessionSeed_ ^ std::hash<std::wstring>{}(directory);
                std::thread([queue = mainQueue_, this, path = legacyPath, directory, serial, sort, descending, seed] {
                    auto entries = scanLocalFolder(directory);
                    orderLocalEntries(entries, sort, descending, seed);
                    queue->post([this, path, serial, entries = std::move(entries)]() mutable {
                        localApplyAdoptedQueue(serial, path, std::move(entries));
                    });
                }).detach();
            }
            // Linked single-video playback ignored this saved repeat flag.
            // Adoption must not suddenly wrap its current presentation time.
            legacy.suppressInheritedRepeat = sourceAutoRepeat_[legacyPane];
            localPanes_[legacyPane] = std::move(legacy);
        }
    }
    const bool emptyDeck = !anyPaneLoaded();
    const bool startPlaying = emptyDeck || playbackIntended();
    const bool retainedMute = (localPanes_[pane] && localPanes_[pane]->addedMuted) ||
                              (embyPanes_[pane] && embyPanes_[pane]->addedMuted);
    detachPaneSeekBarrier(pane);
    // Unlike Emby resolution, a local source can enable audio during open.
    // Clear only this bit first so an Add can never start an audible voice.
    if (add) setAudioMask(audioMask_ & ~(1U << pane));
    openSource(pane, file);
    LocalPanePlayback state;
    state.queue = std::move(capturedQueue);
    state.addedMuted = add || retainedMute;
    localPanes_[pane] = std::move(state);
    startDelays_[pane] = 0.0;
    playbackRates_[pane] = 1.0;
    audio_.setRate(pane, 1.0F);
    // The unchanged master clock/barrier carries All pause. A separate
    // pane pause here would survive the viewer's later All Play action.
    sourcePaused_[pane] = false;
    sourcePausedTimes_[pane] = 0.0;
    // A deferred device-recovery open has no decoder for seekPaneTo yet;
    // preserve the same zero origin in that case as in the normal seek.
    syncAdjustments_[pane] = -clock_.position();
    seekPaneTo(pane, 0.0, true);
    if (!add && emptyDeck) setAudioMask(1U << pane);
    if (emptyDeck && !deviceRecoveryPending_) clock_.play();
    if (selectTarget) embySelectTarget(pane);
    if (add) {
        soloPane_ = -1;
        expandedPane_ = retainedExpandedPane(expandedPane_, activePanes());
    }
    showNotice(L"V" + std::to_wstring(positionForPane(pane) + 1) + L": " + localFileName(file) +
               (add ? L" \x2014 muted" : L"") + (startPlaying ? L"" : L" \x2014 paused with all panes"));
    layoutHoverControls();
    updateHoverControls();
    updateControls();
    updateTitle();
}

void App::localApplyAdoptedQueue(std::uint64_t serial, const std::wstring& path,
                               std::vector<LocalEntry> queue) {
    if (serial == 0) return;
    // The request follows a pane swap or device recovery, but replacement
    // retires its serial with the state. It never changes browser contents.
    for (std::size_t pane = 0; pane < localPanes_.size(); ++pane) {
        auto& state = localPanes_[pane];
        if (!state || state->queueLoadSerial != serial || paths_[pane] != path) continue;
        if (localEntryIndex(queue, path) < 0) queue.push_back(LocalEntry{path});
        state->queue = std::move(queue);
        state->queueLoadSerial = 0;
        break;
    }
}

void App::enqueueExternalFiles(std::vector<std::wstring> files, SingleInstance::RequestId id, bool startup) {
    externalOpenBatches_.push_back(ExternalOpenBatch{id, std::move(files), 0, startup, GetTickCount64()});
}

void App::activateExternalWindow() {
    if (externalActivationOverride_) {
        externalActivationOverride_();
        return;
    }
    if (!window_ || !IsWindow(window_)) return;
    if (IsIconic(window_)) ShowWindow(window_, SW_RESTORE);
    else if (!IsWindowVisible(window_)) ShowWindow(window_, SW_SHOW);
    if (!SetForegroundWindow(window_)) {
        FLASHWINFO flash{sizeof(FLASHWINFO), window_, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0};
        FlashWindowEx(&flash);
    }
}

void App::drainExternalRequests() {
    if (!singleInstance_) return;
    for (auto& request : singleInstance_->takeRequests()) {
        enqueueExternalFiles(std::move(request.files), request.id);
        activateExternalWindow();
    }
}

void App::completeExternalBatch() {
    if (externalOpenBatches_.empty()) return;
    const auto& batch = externalOpenBatches_.front();
    if (singleInstance_ && !batch.startup) singleInstance_->completeRequest(batch.id);
    externalOpenBatches_.pop_front();
}

int App::externalLocalPane(const std::wstring& path) const {
    std::wstring normalized, error;
    if (!SingleInstance::normalizeLocalPath(path, normalized, error)) return -1;
    for (std::size_t pane = 0; pane < paths_.size(); ++pane) {
        if (paths_[pane].empty() || embyPanes_[pane] || emby::isLocator(paths_[pane]) ||
            paths_[pane].find(L"://") != std::wstring::npos) continue;
        std::wstring candidate;
        if (SingleInstance::normalizeLocalPath(paths_[pane], candidate, error) &&
            CompareStringOrdinal(normalized.c_str(), -1, candidate.c_str(), -1, TRUE) == CSTR_EQUAL) {
            // A path reserves its pane even before probing or recovery has
            // constructed the decoder. Focusing it never issues another seek.
            return static_cast<int>(pane);
        }
    }
    return -1;
}

void App::focusExternalPane(std::size_t pane) {
    if (pane >= kMaxPanes) return;
    if (soloPane_ >= 0 && soloPane_ != static_cast<int>(pane)) soloPane_ = -1;
    if (expandedPane_ >= 0 && expandedPane_ != static_cast<int>(pane)) expandedPane_ = -1;
    embySelectTarget(pane);
    lastPointerPane_ = static_cast<int>(pane);
    showNotice(L"Already open in " + embyTargetLabel());
    layoutHoverControls();
    updateControls();
}

int App::externalSubtitleVideoPane(const std::wstring& path, std::size_t* length) const {
    const std::wstring directory = localDirectory(path);
    const std::wstring stem = localStem(path);
    const int target = subtitlePane();
    int best = -1;
    std::size_t bestLength = 0;
    for (std::size_t pane = 0; pane < paths_.size(); ++pane) {
        if (!paneLogicallyLoaded(pane) || embyPanes_[pane] || emby::isLocator(paths_[pane]) ||
            paths_[pane].find(L"://") != std::wstring::npos ||
            !sameLocalDirectory(localDirectory(paths_[pane]), directory)) continue;
        const std::wstring video = localStem(paths_[pane]);
        if (!subtitleNameFitsVideo(stem, video)) continue;
        // The longest name it fits, as in its folder; of panes playing that
        // video, the one subtitle keys act on.
        if (video.size() > bestLength || (video.size() == bestLength && static_cast<int>(pane) == target)) {
            best = static_cast<int>(pane);
            bestLength = video.size();
        }
    }
    if (length) *length = bestLength;
    return best;
}

bool App::deferExternalSubtitle(const std::wstring& path, std::size_t openLength) {
    const std::wstring directory = localDirectory(path);
    const std::wstring stem = localStem(path);
    std::size_t bestBatch = 0;
    std::size_t bestIndex = 0;
    std::size_t bestLength = openLength;
    bool found = false;
    auto& front = externalOpenBatches_.front();
    for (std::size_t b = 0; b < externalOpenBatches_.size(); ++b) {
        const auto& batch = externalOpenBatches_[b];
        for (std::size_t i = b == 0 ? front.next + 1 : batch.next; i < batch.files.size(); ++i) {
            const auto& file = batch.files[i];
            if (isSessionFile(file) || isSubtitleFile(file)) continue;
            std::wstring video, error;
            if (!SingleInstance::normalizeLocalPath(file, video, error) ||
                !sameLocalDirectory(localDirectory(video), directory)) continue;
            // Only a name it fits better than any video already open.
            const std::wstring name = localStem(video);
            if (name.size() > bestLength && subtitleNameFitsVideo(stem, name)) {
                bestBatch = b;
                bestIndex = i;
                bestLength = name.size();
                found = true;
            }
        }
    }
    if (!found) return false;
    // Which entries were moved to wait for a video, kept beside the files.
    const auto marks = [](ExternalOpenBatch& entry) -> std::vector<bool>& {
        entry.deferred.resize(entry.files.size());
        return entry.deferred;
    };
    std::wstring subtitle = std::move(front.files[front.next]);
    auto& frontMarks = marks(front);
    front.files.erase(front.files.begin() + static_cast<std::ptrdiff_t>(front.next));
    frontMarks.erase(frontMarks.begin() + static_cast<std::ptrdiff_t>(front.next));
    if (bestBatch == 0) --bestIndex;   // the subtitle was ahead of it
    auto& batch = externalOpenBatches_[bestBatch];
    auto& batchMarks = marks(batch);
    // After the subtitles moved there before it and ahead of those that came
    // with the video, which came later: they go on it in the order they
    // came, the last one shown.
    std::size_t at = bestIndex + 1;
    while (at < batch.files.size() && batchMarks[at]) ++at;
    batch.files.insert(batch.files.begin() + static_cast<std::ptrdiff_t>(at), std::move(subtitle));
    batchMarks.insert(batchMarks.begin() + static_cast<std::ptrdiff_t>(at), true);
    return true;
}

void App::openExternalSubtitle(const std::wstring& path) {
    int pane = externalSubtitleVideoPane(path);
    if (pane < 0) pane = subtitlePane();
    if (pane < 0) {
        showNotice(L"Open a video first, then its subtitles");
        return;
    }
    // An Emby item still being resolved has nothing to show them on, and
    // another pane did not ask for them.
    if (!paneLogicallyLoaded(static_cast<std::size_t>(pane))) {
        showNotice(L"The video is still opening; open its subtitles again once it plays");
        return;
    }
    addSubtitleFile(static_cast<std::size_t>(pane), path);
}

void App::findExternalSubtitleVideo(const std::wstring& requested, const std::wstring& subtitle) {
    const std::uint64_t serial = externalSubtitleLookup_ = ++externalSubtitleSerial_;
    // The folder can be an unavailable share. Its parent path, not the text
    // before the last slash: "D:" alone is that drive's current folder.
    std::thread([queue = mainQueue_, this, serial, requested, subtitle] {
        const auto videos = scanLocalFolder(std::filesystem::path(subtitle).parent_path().wstring());
        const int found = videoForSubtitle(subtitle, videos);
        std::wstring video = found >= 0 ? videos[static_cast<std::size_t>(found)].path : std::wstring();
        queue->post([this, serial, requested, subtitle, video = std::move(video)] {
            applyExternalSubtitleVideo(serial, requested, subtitle, video);
        });
    }).detach();
}

void App::applyExternalSubtitleVideo(std::uint64_t serial, const std::wstring& requested,
                                     const std::wstring& subtitle, const std::wstring& video) {
    if (serial == 0 || serial != externalSubtitleLookup_) return;
    externalSubtitleLookup_ = 0;
    if (externalOpenBatches_.empty()) return;
    auto& batch = externalOpenBatches_.front();
    if (batch.next >= batch.files.size() || batch.files[batch.next] != requested) return;
    // A video opened meanwhile takes the subtitles on the next pass, and
    // one launched meanwhile is the one meant rather than the folder's.
    if (anyPaneLoaded() || deferExternalSubtitle(subtitle, 0)) return;
    if (video.empty()) {
        showNotice(L"No video beside " + localFileName(requested) + L" has its name");
        ++batch.next;
        return;
    }
    // As if it had come with the launch: the deck opens it, and the
    // subtitles go on it.
    if (!batch.deferred.empty()) {
        batch.deferred.resize(batch.files.size());
        batch.deferred.insert(batch.deferred.begin() + static_cast<std::ptrdiff_t>(batch.next), false);
    }
    batch.files.insert(batch.files.begin() + static_cast<std::ptrdiff_t>(batch.next), video);
}

void App::loadExternalLocalQueue(std::size_t pane, const std::wstring& path,
                                emby::SortKey sort, bool descending) {
    if (pane >= localPanes_.size() || !localPanes_[pane] || paths_[pane] != path) return;
    const std::uint64_t serial = ++localQueueLoadSerial_;
    localPanes_[pane]->queueLoadSerial = serial;
    const std::wstring directory = localDirectory(path);
    const auto seed = sessionSeed_ ^ std::hash<std::wstring>{}(directory);
    // The folder can be an unavailable share. Its late result follows this
    // pane's media serial/path without replacing the browser's current page.
    std::thread([queue = mainQueue_, this, path, directory, serial, sort, descending, seed] {
        auto entries = scanLocalFolder(directory);
        orderLocalEntries(entries, sort, descending, seed);
        queue->post([this, path, serial, entries = std::move(entries)]() mutable {
            localApplyAdoptedQueue(serial, path, std::move(entries));
        });
    }).detach();
}

// An empty deck -- a fresh launch, or a window whose videos were all closed
// -- opens the launch the way the command line always has: one deck of up to
// five in the saved Linked/Independent mode with the saved audio panes, then
// a synchronized start. Several files given to one launch (dropped on the
// executable, Send to) are meant to be compared, not added one by one.
// Subtitle files in the launch are left at the end of it, in their order,
// for the passes after: by then its videos are open, and one of a later
// launch can still take them.
void App::openExternalDeck() {
    auto& batch = externalOpenBatches_.front();
    std::vector<std::wstring> files;
    // Those moved here from earlier launches first, as they came first.
    std::vector<std::wstring> subtitles;
    std::vector<std::wstring> ownSubtitles;
    std::size_t leftOut = 0;
    for (; batch.next < batch.files.size(); ++batch.next) {
        const auto& requested = batch.files[batch.next];
        if (isSessionFile(requested)) {
            showNotice(L"Sessions cannot be added as videos. Use Ctrl+O to load a session.");
            continue;
        }
        std::wstring path, error;
        if (!SingleInstance::normalizeLocalPath(requested, path, error)) {
            showNotice(L"Could not add this file: " + error);
            continue;
        }
        if (isSubtitleFile(path)) {
            const bool moved = batch.next < batch.deferred.size() && batch.deferred[batch.next];
            (moved ? subtitles : ownSubtitles).push_back(std::move(path));
        } else if (files.size() < kMaxPanes) {
            files.push_back(std::move(path));
        } else {
            ++leftOut;
        }
    }
    subtitles.insert(subtitles.end(), std::make_move_iterator(ownSubtitles.begin()),
                     std::make_move_iterator(ownSubtitles.end()));
    if (!files.empty()) {
        loadFiles(files);
        // loadFiles leaves the deck paused at zero; this is Space's start.
        togglePlayback();
    }
    if (leftOut > 0) {
        showNotice(L"Five videos fit at once; " + std::to_wstring(leftOut) +
                   (leftOut == 1 ? L" file was not opened." : L" files were not opened."));
    }
    if (subtitles.empty()) {
        completeExternalBatch();
        return;
    }
    batch.files.insert(batch.files.end(), std::make_move_iterator(subtitles.begin()),
                       std::make_move_iterator(subtitles.end()));
}

void App::processExternalFiles() {
    // Modal timers also tick while a menu/common dialog is open. Wait until
    // its owner is enabled and no press/drag or replacement choice is armed.
    GUITHREADINFO gui{sizeof(GUITHREADINFO)};
    const bool menuOpen = GetGUIThreadInfo(GetCurrentThreadId(), &gui) &&
                          (gui.flags & (GUI_INMENUMODE | GUI_POPUPMENUMODE));
    if (externalFilesProcessing_ || externalSubtitleLookup_ || localPendingPlay_ || embyPendingPlay_ ||
        contextMenuOpen_ || menuOpen || (window_ && !IsWindowEnabled(window_)) ||
        controlDrag_ != ControlDrag::None || panelPressArmed_ || draggingPane_ || pressedBarItem_ >= 0 ||
        pressedChipPane_ >= 0) return;
    externalFilesProcessing_ = true;
    struct ProcessingGuard {
        bool& processing;
        ~ProcessingGuard() { processing = false; }
    } guard{externalFilesProcessing_};
    // A long packet consisting of duplicates must not monopolize a frame.
    for (unsigned processed = 0; processed < 16 && !externalOpenBatches_.empty(); ++processed) {
        auto& batch = externalOpenBatches_.front();
        if (batch.next >= batch.files.size()) {
            completeExternalBatch();
            continue;
        }
        const auto& requested = batch.files[batch.next];
        // Into an empty deck the launch's videos open as one deck. A launch
        // with subtitles and no video waits for the video the first of them
        // belongs to, from its folder.
        const bool videoLeft = std::any_of(
            batch.files.begin() + static_cast<std::ptrdiff_t>(batch.next), batch.files.end(),
            [](const std::wstring& file) { return !isSessionFile(file) && !isSubtitleFile(file); });
        if (!anyPaneLoaded() && videoLeft) {
            openExternalDeck();
            continue;
        }
        if (isSessionFile(requested)) {
            showNotice(L"Sessions cannot be added as videos. Use Ctrl+O to load a session.");
            ++batch.next;
            continue;
        }
        std::wstring path, error;
        if (!SingleInstance::normalizeLocalPath(requested, path, error)) {
            showNotice(L"Could not add this file: " + error);
            ++batch.next;
            continue;
        }
        if (isSubtitleFile(path)) {
            // A video of its name on its way -- later in this launch, or in a
            // launch behind it -- takes it, unless one open fits it as well.
            std::size_t openLength = 0;
            const int openPane = externalSubtitleVideoPane(path, &openLength);
            if (deferExternalSubtitle(path, openLength)) continue;
            // Explorer starts one launch per file selected, and the video's
            // can reach the player a little after its subtitles'. Worth
            // waiting for when no open video fits, or only one with a
            // shorter name than another could have: with "Show.mkv" open,
            // "Show.S01E01.chs.ass" is likely "Show.S01E01.mkv"'s.
            const std::wstring stem = localStem(path);
            const auto lastDot = stem.rfind(L'.');
            const bool betterPossible = openPane < 0 || (lastDot != std::wstring::npos && lastDot > openLength);
            if (betterPossible && GetTickCount64() - batch.arrived < kExternalSubtitleWaitMs) break;
            if (!anyPaneLoaded()) {
                findExternalSubtitleVideo(requested, path);
                break;
            }
            openExternalSubtitle(path);
            ++batch.next;
            continue;
        }
        if (const int existing = externalLocalPane(path); existing >= 0) {
            focusExternalPane(static_cast<std::size_t>(existing));
            ++batch.next;
            continue;
        }
        std::vector<LocalEntry> queue;
        const bool queueCached = sameLocalDirectory(localDirectory(path), localDirectory_) &&
                                 localEntryIndex(localList_, path) >= 0;
        const auto queueSort = emby::sortKeyFromIndex(embyBrowser_.sort);
        const bool queueDescending = embyBrowser_.descending;
        if (queueCached) {
            queue = localList_;
        } else {
            queue.push_back(LocalEntry{path});
        }
        std::size_t pane = kMaxPanes;
        for (std::size_t candidate = 0; candidate < kMaxPanes; ++candidate) {
            if (!paneLogicallyLoaded(candidate) && paths_[candidate].empty() && !embyPanes_[candidate]) {
                pane = candidate;
                break;
            }
        }
        if (pane < kMaxPanes) {
            // The deck already shows something (an empty one was opened
            // above), so this is explicit Add: muted, other panes untouched.
            localPlayItem(path, queue, pane, true);
            if (!queueCached) loadExternalLocalQueue(pane, path, queueSort, queueDescending);
            ++batch.next;
            continue;
        }
        LocalPendingPlay pending;
        pending.path = std::move(path);
        pending.queue = std::move(queue);
        pending.external = true;
        pending.sheetOpen = settingsOpen_;
        pending.browserOpen = embyBrowserOpen_;
        pending.browserByEdge = embyBrowserByEdge_;
        pending.browserSource = browserSource_;
        pending.queueNeedsLoad = !queueCached;
        pending.queueSort = queueSort;
        pending.queueDescending = queueDescending;
        pending.scroll = settingsScroll_;
        pending.paths = paths_;
        pending.mediaSerials = subtitleSerial_;
        if (settingsOpen_) rememberSheetScroll();
        localPendingPlay_ = std::move(pending);
        settingsOpen_ = embyBrowserOpen_ = true;
        browserSource_ = BrowserSource::Local;
        embyBrowserByEdge_ = false;
        settingsScroll_ = 0.0F;
        panelPressArmed_ = false;
        pressedPanelKind_ = PanelHitKind::None;
        controlsLastInteraction_ = GetTickCount64();
        setControlsVisible(true);
        break;
    }
}

void App::localCancelReplacement() {
    if (!localPendingPlay_) return;
    const bool external = localPendingPlay_->external;
    if (external) {
        settingsOpen_ = localPendingPlay_->sheetOpen;
        embyBrowserOpen_ = localPendingPlay_->browserOpen;
        embyBrowserByEdge_ = localPendingPlay_->browserByEdge;
        browserSource_ = localPendingPlay_->browserSource;
    }
    settingsScroll_ = localPendingPlay_->scroll;
    localPendingPlay_.reset();
    panelPressArmed_ = false;
    pressedPanelKind_ = PanelHitKind::None;
    // Dropping only this batch leaves later independently accepted launches
    // intact. Their next chooser is created by a later frame, never here.
    if (external) completeExternalBatch();
}

void App::localConfirmReplacement(std::size_t pane) {
    if (!localPendingPlay_ || pane >= kMaxPanes) return;
    auto pending = std::move(*localPendingPlay_);
    // Restore the browser's position before discarding the moved snapshot.
    if (pending.external) {
        settingsOpen_ = pending.sheetOpen;
        embyBrowserOpen_ = pending.browserOpen;
        embyBrowserByEdge_ = pending.browserByEdge;
        browserSource_ = pending.browserSource;
    }
    settingsScroll_ = pending.scroll;
    localPendingPlay_.reset();
    panelPressArmed_ = false;
    pressedPanelKind_ = PanelHitKind::None;
    if (pending.external) {
        if (const int existing = externalLocalPane(pending.path); existing >= 0) {
            focusExternalPane(static_cast<std::size_t>(existing));
            if (!externalOpenBatches_.empty()) ++externalOpenBatches_.front().next;
            return;
        }
    }
    if (paths_[pane] != pending.paths[pane] || subtitleSerial_[pane] != pending.mediaSerials[pane]) {
        showNotice(pending.external ? L"That pane changed. Choose its replacement again."
                                    : L"That pane changed. Add again to choose its replacement.");
        return;
    }
    localPlayItem(pending.path, pending.queue, pane, true);
    if (pending.external) {
        if (pending.queueNeedsLoad)
            loadExternalLocalQueue(pane, pending.path, pending.queueSort, pending.queueDescending);
        if (!externalOpenBatches_.empty()) ++externalOpenBatches_.front().next;
    }
}

std::vector<PanelRow> App::buildLocalReplacementRows(float sheetWidth) const {
    std::vector<PanelRow> rows;
    if (!localPendingPlay_) return rows;
    PanelRow heading;
    heading.kind = PanelRowKind::Header;
    heading.label = L"All five panes are occupied";
    rows.push_back(std::move(heading));
    PanelRow note;
    note.kind = PanelRowKind::Note;
    note.wrapNote = true;
    note.label = L"Replace one pane with " + localFileName(localPendingPlay_->path) +
                 L" from the beginning (muted).";
    if (localPendingPlay_->external && !externalOpenBatches_.empty()) {
        const auto& batch = externalOpenBatches_.front();
        const auto remaining = batch.files.size() - std::min(batch.next, batch.files.size());
        note.label += L" " + std::to_wstring(remaining) +
                      (remaining == 1 ? L" file remains in this launch." : L" files remain in this launch.") +
                      L" Cancel skips the remaining files from this launch and keeps all panes. Later launches stay queued.";
    }
    rows.push_back(std::move(note));
    PanelRow cancel;
    cancel.kind = PanelRowKind::Buttons;
    cancel.id = SettingId::LocalReplaceCancel;
    cancel.options = {L"Cancel \x2014 keep all panes"};
    rows.push_back(std::move(cancel));
    const PanelMetrics metrics;
    const float innerWidth = std::max(0.0F, sheetWidth - 2.0F * metrics.pad * uiScale_);
    const int perLine = panelTilesPerLine(innerWidth, 200.0F, uiScale_, metrics);
    const auto order = panesByPosition();
    for (int start = 0; start < static_cast<int>(kMaxPanes); start += perLine) {
        PanelRow row;
        row.kind = PanelRowKind::Tiles;
        row.id = SettingId::LocalReplaceChoice;
        row.tileWidth = panelTileWidthFor(innerWidth, perLine, uiScale_, metrics);
        row.tileAspect = 16.0F / 9.0F;
        for (int position = start; position < std::min(start + perLine, static_cast<int>(kMaxPanes)); ++position) {
            const auto pane = static_cast<std::size_t>(order[static_cast<std::size_t>(position)]);
            const auto& slot = embyPanes_[pane];
            const auto title = slot ? slot->title : localFileName(paths_[pane]);
            row.options.push_back(L"V" + std::to_wstring(position + 1) + L" \x2014 " + title);
            row.tileKeys.push_back(slot ? slot->imageKey : localThumbnailKey(paths_[pane], 400));
            row.tileParams.push_back(static_cast<int>(pane));
            row.tileBadges.push_back(audioPaneEnabled(pane) ? L"Audio on" : L"Muted");
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<PanelRow> App::buildLocalRows(float sheetWidth) const {
    if (localPendingPlay_) return buildLocalReplacementRows(sheetWidth);
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
        row.wrapNote = true;
        row.label = std::move(text);
        rows.push_back(std::move(row));
    };
    const PanelMetrics metrics;
    const float innerWidth = std::max(0.0F, sheetWidth - 2.0F * metrics.pad * uiScale_);

    header(localDirectory_.empty() ? std::wstring(L"Folder") : localFileName(localDirectory_));
    {
        PanelRow row;
        row.kind = PanelRowKind::Buttons;
        row.id = SettingId::LocalNav;
        row.options = {L"\x2190 Settings", L"Emby\x2026", L"Refresh"};
        rows.push_back(std::move(row));
    }
    note(L"Play target: " + embyTargetLabel());
    note(L"+ Add: new pane, muted; starts from the beginning.");
    note(L"Click a video pane to change the play target.");
    {
        PanelRow row;
        row.kind = PanelRowKind::Choice;
        row.id = SettingId::EmbyView;
        row.label = L"View";
        row.options = {L"List", L"Posters", L"Thumbnails"};
        row.selected = embyBrowser_.view;
        row.segmentsPerLine = 3;
        rows.push_back(std::move(row));
    }
    {
        const auto key = emby::sortKeyFromIndex(embyBrowser_.sort);
        PanelRow row;
        row.kind = PanelRowKind::Choice;
        row.id = SettingId::LocalSort;
        row.label = std::wstring(L"Sort by \x2014 ") +
                    (key == emby::SortKey::Random ? L"in no particular order"
                     : embyBrowser_.descending ? L"descending" : L"ascending") +
                    L"   (Ctrl+4 name, 6 size, 7 date, 8 length, 9 random)";
        for (int choice = 0; choice < kLocalSortCount; ++choice) row.options.push_back(localSortName(choice));
        row.selected = localSortChoice(key);
        row.segmentsPerLine = panelSegmentsPerLine(innerWidth, uiScale_, kLocalSortCount, 64.0F, metrics);
        rows.push_back(std::move(row));
    }
    if (localListLoading_ && localList_.empty()) note(L"Reading the folder\x2026");
    else if (localList_.empty()) note(L"No videos in this folder.");
    else note(std::to_wstring(localList_.size()) + (localList_.size() == 1 ? L" video" : L" videos"));

    // Every local file being played is marked, including the same file
    // independently opened in more than one pane.
    PaneArray<int> playing;
    playing.fill(-1);
    for (std::size_t pane = 0; pane < paths_.size(); ++pane) {
        const auto& path = paths_[pane];
        if (!emby::isLocator(path) && sameLocalDirectory(localDirectory(path), localDirectory_)) {
            playing[pane] = localEntryIndex(localList_, path);
        }
    }
    const auto isPlaying = [&](std::size_t index) {
        return std::find(playing.begin(), playing.end(), static_cast<int>(index)) != playing.end();
    };
    const auto length = [](const LocalEntry& entry) {
        return entry.duration > 0.0 ? formatTime(entry.duration) : std::wstring();
    };
    // A video with subtitles to show is marked, so which ones have them can
    // be seen before choosing.
    const auto subtitleTag = [](const LocalEntry& entry) {
        return localHasSubtitles(entry) ? std::wstring(L"Sub") : std::wstring();
    };

    if (embyBrowser_.view == 0) {
        for (std::size_t i = 0; i < localList_.size(); ++i) {
            PanelRow row;
            row.kind = PanelRowKind::Item;
            row.id = SettingId::LocalItem;
            row.param = static_cast<int>(i);
            row.label = localFileName(localList_[i].path);
            // Length and size side by side; the size alone until the length is read.
            const std::wstring time = length(localList_[i]);
            row.value = time.empty() ? formatFileSize(localList_[i].size)
                                     : time + L"   " + formatFileSize(localList_[i].size);
            row.tag = subtitleTag(localList_[i]);
            row.current = isPlaying(i);
            row.addAvailable = true;
            rows.push_back(std::move(row));
        }
        return rows;
    }
    const bool posters = embyBrowser_.view == 1;
    const int perLine = panelTilesPerLine(innerWidth, posters ? kPosterTile : kThumbTile, uiScale_, metrics);
    const float tileWidth = panelTileWidthFor(innerWidth, perLine, uiScale_, metrics);
    const int imageWidth = std::max(200, static_cast<int>(std::ceil(tileWidth * uiScale_ * 2.0F / 100.0F)) * 100);
    for (std::size_t start = 0; start < localList_.size(); start += static_cast<std::size_t>(perLine)) {
        PanelRow row;
        row.kind = PanelRowKind::Tiles;
        row.id = SettingId::LocalItem;
        row.tileWidth = tileWidth;
        row.tileAspect = posters ? 2.0F / 3.0F : 16.0F / 9.0F;
        const std::size_t end = std::min(localList_.size(), start + static_cast<std::size_t>(perLine));
        for (std::size_t i = start; i < end; ++i) {
            row.options.push_back(localFileName(localList_[i].path));
            row.tileKeys.push_back(localThumbnailKey(localList_[i].path, imageWidth));
            // The length where the Emby tiles have theirs, the size at the left.
            const std::wstring time = length(localList_[i]);
            row.tileBadges.push_back(time.empty() ? formatFileSize(localList_[i].size) : time);
            row.tileMarks.push_back(time.empty() ? std::wstring() : formatFileSize(localList_[i].size));
            row.tileProgress.push_back(0.0F);
            row.tileTags.push_back(subtitleTag(localList_[i]));
            row.tileParams.push_back(static_cast<int>(i));
            row.tileAddAvailable.push_back(true);
            if (isPlaying(i)) row.selected = static_cast<int>(i - start);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

// ---------------------------------------------------------------------------
// Explorer's file types

void App::refreshFileTypesStatus() {
    fileTypes_ = fileAssociationStatus(currentExecutablePath());
}

void App::fileTypesAction(int part) {
    std::wstring error;
    if (part == 0) {
        if (registerFileAssociations(currentExecutablePath(), error)) {
            appendDiagnostic("File types registered for " + wideToUtf8Text(currentExecutablePath()));
            showNotice(L"Registered: choose QuadDeck in Default apps");
            // Windows keeps the choice of default to the user.
            openDefaultAppsSettings();
        } else {
            appendDiagnostic("File types not registered: " + wideToUtf8Text(error));
            showNotice(L"Could not register: " + error);
        }
    } else {
        if (unregisterFileAssociations(error)) {
            appendDiagnostic("File types removed");
            showNotice(L"QuadDeck removed from Explorer's file types");
        } else {
            appendDiagnostic("File types not fully removed: " + wideToUtf8Text(error));
            showNotice(L"Could not remove everything: " + error);
        }
    }
    refreshFileTypesStatus();
}

}  // namespace quaddeck
