// App: Emby. The sign-in and its file, progress reports back to the server,
// the per-frame Emby tick, the subtitle streams the server hands over (the
// rest of subtitles is AppSubtitles.cpp), and the audio-track choice that
// applies to any pane, Emby or local. The library browser is
// AppEmbyBrowser.cpp, playing an item into a pane AppEmbyPlayback.cpp and the
// Movie/Series pages AppEmbyDetails.cpp.
//
// Every reply from the server arrives through EmbyClient::pump() on the
// window thread, inside tick(). Handlers carry a serial and do nothing when
// the browser has moved on or the pane has been reused since they were
// queued; that is the whole concurrency story.

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

constexpr auto kEmbyProgressInterval = std::chrono::seconds(10);

std::wstring wide(const std::string& text) { return utf8ToWideText(text); }
std::string narrow(const std::wstring& text) { return wideToUtf8Text(text); }

struct SignInFields {
    std::wstring server, user, password;
};

INT_PTR CALLBACK signInProcedure(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_INITDIALOG: {
        SetWindowLongPtrW(dialog, GWLP_USERDATA, lParam);
        auto* fields = reinterpret_cast<SignInFields*>(lParam);
        SetDlgItemTextW(dialog, IDC_EMBY_SERVER, fields->server.c_str());
        SetDlgItemTextW(dialog, IDC_EMBY_USER, fields->user.c_str());
        const int focus = fields->server.empty() || fields->server == L"http://" ? IDC_EMBY_SERVER
                        : fields->user.empty() ? IDC_EMBY_USER : IDC_EMBY_PASSWORD;
        SetFocus(GetDlgItem(dialog, focus));
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            auto* fields = reinterpret_cast<SignInFields*>(GetWindowLongPtrW(dialog, GWLP_USERDATA));
            if (fields) {
                fields->server = dialogText(dialog, IDC_EMBY_SERVER);
                fields->user = dialogText(dialog, IDC_EMBY_USER);
                fields->password = dialogText(dialog, IDC_EMBY_PASSWORD);
            }
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

}  // namespace

// ---------------------------------------------------------------------------
// Sign-in and its file

std::filesystem::path App::embyAuthPath() const {
    if (!embyAuthPathOverride_.empty()) return embyAuthPathOverride_;
    const auto settings = settingsPath();
    if (settings.empty()) return {};
    return settings.parent_path() / L"emby.qauth";
}

void App::embyLoadAuth() {
    const auto path = embyAuthPath();
    std::wstring text;
    if (path.empty() || !readUtf8File(path, text)) return;
    std::wistringstream input(text);
    input.imbue(std::locale::classic());
    emby::StoredAuth stored;
    if (!emby::readStoredAuth(input, stored)) {
        appendDiagnostic("Emby: the sign-in file could not be read");
        return;
    }
    emby::Session session;
    session.serverUrl = stored.serverUrl;
    session.serverId = stored.serverId;
    session.serverName = stored.serverName;
    session.userId = stored.userId;
    session.userName = stored.userName;
    session.deviceId = stored.deviceId.empty() ? EmbyClient::newDeviceId() : stored.deviceId;
    session.token = EmbyClient::unprotectSecret(stored.protectedToken);
    if (!stored.protectedToken.empty() && session.token.empty()) {
        appendDiagnostic("Emby: the stored token could not be unprotected; sign in again");
    }
    embyConfigure(session);
    appendDiagnostic(session.signedIn()
        ? "Emby: signed in as " + session.userName + " on " + session.serverUrl
        : "Emby: server remembered (" + session.serverUrl + "), not signed in");
}

void App::embySaveAuth() const {
    const auto path = embyAuthPath();
    if (path.empty()) return;
    const auto& session = emby_.session();
    emby::StoredAuth stored;
    stored.serverUrl = session.serverUrl;
    stored.serverId = session.serverId;
    stored.serverName = session.serverName;
    stored.userId = session.userId;
    stored.userName = session.userName;
    stored.deviceId = session.deviceId;
    stored.protectedToken = EmbyClient::protectSecret(session.token);
    if (!session.token.empty() && stored.protectedToken.empty()) {
        appendDiagnostic("Emby: Windows could not protect the token; this sign-in will not be remembered");
        return;
    }
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    if (!writeUtf8FileAtomically(path, [&](std::wostream& output) {
            return emby::writeStoredAuth(output, stored);
        })) {
        appendDiagnostic("Emby: could not save the sign-in file");
    }
}

void App::embyConfigure(const emby::Session& session) {
    const bool changed = session.userId != emby_.session().userId ||
                         session.serverUrl != emby_.session().serverUrl ||
                         session.serverId != emby_.session().serverId ||
                         session.token != emby_.session().token;
    if (changed) {
        embyCancelReplacement();
        embyDetachPlayback();
        embyCloseDetails();
        ++embyAccountSerial_;
        ++embyRequestSerial_;
        embyItemLibraries_.clear();
        embyLibraryLookups_.clear();
        // Another account's answers name other videos.
        embySubtitled_.clear();
        embySubtitleAsking_.clear();
    }
    emby_.configure(session);
    embyImages_.setAccept("image/*");
    embyImages_.configure(session);
    embySubtitles_.configure(session);
    if (changed) {
        // Another account or server: its pictures are not this one's.
        embyImages_.clearPending();
        embyImagesInFlight_.clear();
        thumbnails_.clear();
    }
}

std::wstring App::embyStatusLine() const {
    const auto& session = emby_.session();
    if (session.signedIn()) {
        return L"Signed in as " + wide(session.userName) + L" on " +
               wide(session.serverName.empty() ? session.serverUrl : session.serverName) + L".";
    }
    if (!embyNote_.empty()) return embyNote_;
    if (!session.serverUrl.empty()) return L"Not signed in to " + wide(session.serverUrl) + L".";
    return L"Play Emby videos in a chosen pane, or add them to an empty pane.";
}

void App::embySignInDialog() {
    SignInFields fields;
    fields.server = wide(emby_.session().serverUrl);
    fields.user = wide(emby_.session().userName);
    if (fields.server.empty()) fields.server = L"http://";
    const INT_PTR result = DialogBoxParamW(instance_, MAKEINTRESOURCEW(IDD_EMBY_SIGNIN), window_,
                                           signInProcedure, reinterpret_cast<LPARAM>(&fields));
    if (result != IDOK) return;
    embySignIn(narrow(fields.server), narrow(fields.user), narrow(fields.password));
}

void App::embySignIn(const std::string& serverUrl, const std::string& userName, const std::string& password) {
    emby::Session session = emby_.session();
    session.serverUrl = emby::normalizeServerUrl(serverUrl);
    session.token.clear();
    session.userId.clear();
    session.userName = userName;
    if (session.deviceId.empty()) session.deviceId = EmbyClient::newDeviceId();
    if (session.serverUrl.empty()) {
        showNotice(L"Enter the Emby server address");
        return;
    }
    embyConfigure(session);
    embyPages_.clear();
    embyItems_.clear();
    embyResume_.clear();
    embyViews_.clear();
    embyNote_ = L"Connecting to " + wide(session.serverUrl) + L"\x2026";
    embyLoading_ = true;
    if (settingsOpen_) rememberSheetScroll();
    if (!settingsOpen_) toggleSettingsPanel();
    embyBrowserOpen_ = true;
    browserSource_ = BrowserSource::Emby;
    settingsScroll_ = 0.0F;
    const std::uint64_t serial = ++embyRequestSerial_;
    emby_.request("GET", emby::publicInfoPath(), "", [this, serial, userName, password](const EmbyClient::Response& response) {
        embyServerInfoArrived(serial, userName, password, response);
    });
}

void App::embyServerInfoArrived(std::uint64_t serial, const std::string& userName,
                              const std::string& password, const EmbyClient::Response& response) {
    if (serial != embyRequestSerial_) return;
    if (!response.ok) {
        embyLoading_ = false;
        embyBrowserOpen_ = false;
        embyNote_ = L"Cannot reach the server: " + wide(response.error);
        showNotice(L"Emby: cannot reach the server");
        return;
    }
    const auto info = emby::parseServerInfo(response.body);
    if (!info) {
        embyLoading_ = false;
        embyBrowserOpen_ = false;
        embyNote_ = L"That address does not answer like an Emby server.";
        showNotice(L"Emby: not an Emby server");
        return;
    }
    auto session = emby_.session();
    session.serverId = info->id;
    session.serverName = info->name;
    embyConfigure(session);
    const auto authenticationSerial = embyRequestSerial_;
    appendDiagnostic("Emby: server " + info->name + " version " + info->version);
    emby_.request("POST", emby::authenticatePath(), emby::authenticateBody(userName, password),
                  [this, authenticationSerial](const EmbyClient::Response& reply) {
        embyAuthenticationArrived(authenticationSerial, reply);
    });
}

void App::embyAuthenticationArrived(std::uint64_t serial, const EmbyClient::Response& response) {
    if (serial != embyRequestSerial_) return;
    embyLoading_ = false;
    if (!response.ok) {
        embyBrowserOpen_ = false;
        embyNote_ = response.status == 401 ? L"Wrong user name or password."
                                          : L"Sign-in failed: " + wide(response.error);
        showNotice(L"Emby: sign-in failed");
        return;
    }
    const auto auth = emby::parseAuthentication(response.body);
    if (!auth) {
        embyBrowserOpen_ = false;
        embyNote_ = L"The server's sign-in reply could not be read.";
        return;
    }
    auto session = emby_.session();
    session.token = auth->token;
    session.userId = auth->userId;
    if (!auth->userName.empty()) session.userName = auth->userName;
    if (!auth->serverId.empty()) session.serverId = auth->serverId;
    embyConfigure(session);
    embySaveAuth();
    embyNote_.clear();
    appendDiagnostic("Emby: signed in as " + session.userName + " on " + session.serverUrl);
    showNotice(L"Emby: signed in as " + wide(session.userName));
    embyGoHome();
}

void App::embySignOut() {
    // Whatever plays from this account stops under it; nothing queued for
    // it may run under the next one.
    embyDetachPlayback();
    auto session = emby_.session();
    if (session.signedIn()) emby_.request("POST", emby::logoutPath(), "", {});
    session.token.clear();
    session.userId.clear();
    embyConfigure(session);
    embySaveAuth();
    embyBrowserOpen_ = false;
    embyPages_.clear();
    embyItems_.clear();
    embyResume_.clear();
    embyViews_.clear();
    embyNote_.clear();
    embyLoading_ = false;
    embyLoadingMore_ = false;
    appendDiagnostic("Emby: signed out");
    showNotice(L"Emby: signed out");
}

void App::embySessionExpired() {
    embyDetachPlayback();
    auto session = emby_.session();
    session.token.clear();
    session.userId.clear();
    embyConfigure(session);
    embySaveAuth();
    embyBrowserOpen_ = false;
    embyPages_.clear();
    embyItems_.clear();
    embyResume_.clear();
    embyViews_.clear();
    embyLoading_ = false;
    embyLoadingMore_ = false;
    embyNote_ = L"The Emby sign-in has expired. Sign in again.";
    appendDiagnostic("Emby: the server refused the token; signed out");
    showNotice(L"Emby sign-in expired");
}

void App::embyDetachPlayback() {
    for (std::size_t pane = 0; pane < embyPanes_.size(); ++pane) {
        if (!embyPanes_[pane]) continue;
        // Enqueue Stopped with the old client's captured credentials before
        // configure changes accounts. Local panes keep playing untouched.
        closePane(pane);
    }
    ++embyRequestSerial_;
}

std::optional<std::size_t> App::embyPaneWithSerial(std::uint64_t serial) const {
    if (serial == 0) return std::nullopt;
    for (std::size_t pane = 0; pane < embyPanes_.size(); ++pane) {
        if (embyPanes_[pane] && embyPanes_[pane]->serial == serial && embyPaneAccountMatches(pane)) return pane;
    }
    return std::nullopt;
}

bool App::paneOpening(std::size_t pane) const {
    if (pane >= sources_.size()) return false;
    const auto& source = sources_[pane];
    if (source) return !source->ready() && source->error().empty();
    return embyPanes_[pane].has_value() && embyPanes_[pane]->resolving;
}

void App::resetPaneMediaState(std::size_t pane) {
    if (pane >= sources_.size()) return;
    embyClearPane(pane);
    localPanes_[pane].reset();
    subtitles_[pane].reset();
    subtitleSelection_[pane] = {};
    subtitleFiles_[pane].clear();
    subtitleFilesWithoutLines_[pane].clear();
    subtitleCycleAnchor_[pane] = {};
    subtitleChosenByViewer_[pane] = false;
    subtitleEmbeddedPending_[pane] = false;
    subtitleDelay_[pane] = 0.0;
    subtitleSerial_[pane] = ++subtitleSerialCounter_;
    assPanes_[pane] = {};
}

void App::embyFailure(const EmbyClient::Response& response) {
    if (response.status == 401) {
        embySessionExpired();
        return;
    }
    embyNote_ = L"Emby: " + wide(response.error);
    appendDiagnostic("Emby: request failed: " + response.error);
}

// ---------------------------------------------------------------------------
// Reports

std::optional<emby::Report> App::embyReportForPane(std::size_t pane) const {
    if (!embyPaneAccountMatches(pane)) return std::nullopt;
    const auto& state = *embyPanes_[pane];
    if (state.mediaSourceId.empty() || state.photo) return std::nullopt;
    // Defensive uniqueness for old session files / direct locator callers.
    if (embyPlayingPane(state.itemId) != static_cast<int>(pane)) return std::nullopt;
    emby::Report report;
    report.itemId = state.itemId;
    report.mediaSourceId = state.mediaSourceId;
    report.playSessionId = state.playSessionId;
    report.positionTicks = emby::ticksFromSeconds(currentSourceTime(pane));
    report.paused = !clock_.isPlaying() || sourcePaused_[pane];
    return report;
}

void App::embyReport(std::size_t pane, const char* kind, const std::string& eventName) {
    auto prepared = embyReportForPane(pane);
    if (!prepared) return;
    auto& state = *embyPanes_[pane];
    const std::string_view what(kind);
    const bool stopped = what == "stopped";
    if (stopped && !state.started) return;
    auto& report = *prepared;
    report.eventName = eventName;
    const std::string path = stopped ? emby::stoppedPath()
                           : what == "playing" ? emby::playingPath() : emby::progressPath();
    emby_.request("POST", path, emby::reportBody(report), {});
    state.lastReport = std::chrono::steady_clock::now();
    state.lastPaused = report.paused;
    if (what == "playing") state.started = true;
    if (stopped) state.started = false;
    // What the list says of this video -- how far it was watched, when it
    // was last played -- has just changed on the server. The request goes
    // out behind the report, on the same queue.
    if (stopped || what == "playing") embyWantRefresh(250);
}

void App::embyTick() {
    emby_.pump();
    // The pictures' replies too; without this pump no thumbnail ever lands.
    embyImages_.pump();
    embySubtitles_.pump();
    localThumbnails_.pump();
    embyRefreshWhenDue(GetTickCount64());
    const auto now = std::chrono::steady_clock::now();
    for (std::size_t pane = 0; pane < embyPanes_.size(); ++pane) {
        auto& slot = embyPanes_[pane];
        if (!slot || slot->endHandled || !embyPaneAccountMatches(pane) || slot->mediaSourceId.empty() ||
            slot->photo || !sources_[pane] || !sources_[pane]->ready()) {
            continue;
        }
        const bool paused = !clock_.isPlaying() || sourcePaused_[pane];
        if (!slot->started) {
            embyReport(pane, "playing");
            continue;
        }
        if (paused != slot->lastPaused) {
            embyReport(pane, "progress", paused ? "Pause" : "Unpause");
            continue;
        }
        if (now - slot->lastReport >= kEmbyProgressInterval) embyReport(pane, "progress", "TimeUpdate");
    }
}

void App::embyClearPane(std::size_t pane) {
    if (pane < embyPanes_.size()) embyPanes_[pane].reset();
}

// ---------------------------------------------------------------------------
// An Emby item's subtitle streams, and audio tracks

void App::embyLoadSubtitle(std::size_t pane, int streamIndex, bool converted) {
    if (pane >= embyPanes_.size() || !embyPanes_[pane]) return;
    auto& state = *embyPanes_[pane];
    state.subtitleStream = streamIndex;
    subtitles_[pane].reset();
    if (streamIndex < 0 || state.mediaSourceId.empty()) return;
    // An ASS stream is asked for as it is: the server's conversion to SRT
    // keeps a line's words but only some of where its script put it, and
    // leaves a shape's outline behind as if it were speech. Anything else,
    // and an ASS stream the server would not hand over so, comes as SRT.
    bool ass = false;
    for (const auto& stream : state.streams) {
        if (stream.index == streamIndex && !converted) ass = emby::isAssSubtitleCodec(stream.codec);
    }
    const std::uint64_t serial = state.serial;
    // On its own client: the server may take half a minute to pull a stream
    // out of a file the first time it is asked, and a click in the browser
    // or a progress report must not wait behind that.
    embySubtitles_.request("GET", emby::subtitlePath(state.itemId, state.mediaSourceId, streamIndex, ass ? "ass" : "srt"), "",
                  [this, serial, streamIndex, ass](const EmbyClient::Response& response) {
        const auto found = embyPaneWithSerial(serial);
        if (!found || embyPanes_[*found]->subtitleStream != streamIndex) return;
        const SubtitleSelection asked{SubtitleKind::EmbyStream, streamIndex, {}};
        if (!(subtitleSelection_[*found] == asked)) return;
        if (response.status == 401) {
            embySessionExpired();
            return;
        }
        // A file beside the video comes as it lies on the server's disk: in
        // UTF-16 it is converted here, and in a legacy code page it is left
        // to the server, whose conversion to SRT knows the file's encoding.
        const std::string body = response.ok ? decodeSubtitleBytes(response.body) : std::string();
        auto track = response.ok && (!ass || isUtf8Text(body)) ? parseSubtitles(body) : std::nullopt;
        if (ass && (!track || track->cues.empty())) {
            appendDiagnostic("Emby: subtitle stream " + std::to_string(streamIndex) +
                             " did not come as ASS; asking for SRT");
            embyLoadSubtitle(*found, streamIndex, true);
            return;
        }
        if (!response.ok) {
            showNotice(L"Subtitles unavailable: " + wide(response.error));
            return;
        }
        if (!track || track->cues.empty()) {
            showNotice(L"The subtitle stream has no lines");
            return;
        }
        appendDiagnostic("Emby: subtitle stream " + std::to_string(streamIndex) + " loaded as " +
                         (ass ? "ASS, " : "SRT, ") + std::to_string(track->cues.size()) + " cues");
        subtitles_[*found] = std::make_shared<const SubtitleTrack>(std::move(*track));
    });
}

void App::selectAudioTrack(std::size_t pane, int streamIndex) {
    if (pane >= sources_.size() || !sources_[pane]) return;
    sources_[pane]->setAudioTrack(streamIndex);
    std::wstring label = L"Audio track " + std::to_wstring(streamIndex);
    for (const auto& track : sources_[pane]->audioTracks()) {
        if (track.streamIndex == streamIndex) label = audioTrackLabel(track);
    }
    appendDiagnostic("Audio track requested: stream " + std::to_string(streamIndex) + " pane=" +
                     std::to_string(pane + 1));
    if (seekBarrier_.active()) {
        // The barrier owns every generation right now; rebuilding it at the
        // same target carries the new track through.
        seekAbsolute(clock_.position());
        showNotice(label);
        return;
    }
    // The worker swaps decoders on its next seek; give it one at the
    // current time, the way an audio-output change does.
    const double position = clock_.position();
    sources_[pane]->setAudioEnabled(false);
    const auto generation = sources_[pane]->requestAudioSeek(currentSourceTime(pane));
    audio_.flush(pane, generation);
    const bool useAudio = shouldOutputAudio(pane, position, clock_.isPlaying());
    sources_[pane]->setAudioEnabled(useAudio);
    audio_.setRate(pane, static_cast<float>(playbackRates_[pane]));
    if (useAudio) audio_.play(pane);
    showNotice(label);
}

}  // namespace quaddeck
