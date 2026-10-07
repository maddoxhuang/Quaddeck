// App: subtitles. What a pane offers -- the text streams an Emby server
// hands over, the files beside a local video or loaded by hand, the text
// streams inside the container -- which of them is shown when the viewer
// has not said, and the viewer's own choices: the menu, PotPlayer's keys,
// the timing, the size and the place.
//
// Nothing here reads a file on the window thread: a share that has gone away
// must not freeze the player. A scan or a read runs on a detached thread and
// comes back through the main-thread queue, where it finds its pane by the
// serial it left with and is dropped when the pane has moved on.

#include "App.hpp"
#include "AppInternal.hpp"
#include "Diagnostics.hpp"
#include "TextEncoding.hpp"

#include <commdlg.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>

namespace quaddeck {

using namespace app_internal;

namespace {

constexpr std::size_t kSubtitleFileLimit = 16u * 1024u * 1024u;
// As many entries as the menu has command numbers for.
constexpr std::size_t kSubtitleMenuLimit = CmdSubtitleLast - CmdSubtitleFirst + 1;
// A folder of near-identical files is not read from end to end to find
// one with lines in it.
constexpr std::size_t kSidecarReadAttempts = 4;
constexpr double kSubtitleDelayLimit = 600.0;
constexpr float kSubtitleDefaultPosition = SubtitleSettings{}.position;

std::wstring lowercase(std::wstring text) {
    for (auto& character : text) character = static_cast<wchar_t>(std::towlower(character));
    return text;
}

// The first `limit` bytes of a file, or nothing when it is larger. A
// subtitle never needs more than a few megabytes.
std::string readBoundedFile(const std::filesystem::path& path, std::size_t limit) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    std::string bytes(limit + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    const auto count = static_cast<std::size_t>(input.gcount());
    if (count > limit) return {};
    bytes.resize(count);
    return bytes;
}

// The code page a subtitle file that is not Unicode is most likely in: what
// the last part of its name says -- "movie.cht.ass", "movie.big5.srt" --
// before the system's own. A Traditional Chinese file on a system set to
// Simplified is otherwise read as the wrong characters, and the reverse.
UINT hintedCodePage(const std::filesystem::path& file) {
    const std::wstring stem = file.stem().wstring();
    const auto dot = stem.find_last_of(L'.');
    if (dot == std::wstring::npos) return CP_ACP;
    const std::string language = subtitle_detail::languageFromHint(stem.substr(dot + 1));
    if (language == "zh-Hant") return 950;
    if (language == "zh-Hans") return 936;
    if (language == "ja") return 932;
    if (language == "ko") return 949;
    return CP_ACP;
}

// Subtitle bytes as UTF-8: a UTF-16 byte-order mark is honoured, valid
// UTF-8 passes, anything else is taken as the code page named, if that
// reads it without an invalid character, and as the system's otherwise.
std::string subtitleBytesToUtf8(std::string bytes, UINT codePage) {
    bytes = decodeSubtitleBytes(std::move(bytes));
    if (bytes.empty() || wideToUtf8Text(utf8ToWideText(bytes)) == bytes) return bytes;
    const auto decode = [&](UINT page, DWORD flags) -> std::optional<std::string> {
        const int size = MultiByteToWideChar(page, flags, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (size <= 0) return std::nullopt;
        std::wstring converted(static_cast<std::size_t>(size), L'\0');
        MultiByteToWideChar(page, flags, bytes.data(), static_cast<int>(bytes.size()), converted.data(), size);
        return wideToUtf8Text(converted);
    };
    if (codePage != CP_ACP) {
        if (auto text = decode(codePage, MB_ERR_INVALID_CHARS)) return *text;
    }
    if (auto text = decode(CP_ACP, 0)) return *text;
    return bytes;
}

// A file's cues, or nothing when it cannot be read or holds no line.
std::shared_ptr<const SubtitleTrack> readSubtitleFile(const std::filesystem::path& file) {
    auto track = parseSubtitles(subtitleBytesToUtf8(readBoundedFile(file, kSubtitleFileLimit), hintedCodePage(file)));
    if (!track || track->cues.empty()) return nullptr;
    return std::make_shared<const SubtitleTrack>(std::move(*track));
}

// What a subtitle file's name adds to the video's: "movie.chs.ass" beside
// "movie.mkv" says "chs", which is where a language is usually named. A
// file named otherwise says its whole name.
std::wstring sidecarSuffix(const std::filesystem::path& video, const std::filesystem::path& file) {
    const std::wstring stem = lowercase(video.stem().wstring());
    const std::wstring name = file.stem().wstring();
    if (!stem.empty() && name.size() >= stem.size() && lowercase(name.substr(0, stem.size())) == stem) {
        std::wstring rest = name.substr(stem.size());
        while (!rest.empty() && (rest.front() == L'.' || rest.front() == L' ' || rest.front() == L'_' ||
                                 rest.front() == L'-')) {
            rest.erase(rest.begin());
        }
        return rest;
    }
    return name;
}

// What the Subtitles menu shows for a stream inside the container.
std::wstring subtitleTrackLabel(const SubtitleTrackInfo& track) {
    std::wstring label;
    if (!track.title.empty()) label = utf8ToWide(track.title);
    if (!track.language.empty()) {
        if (!label.empty()) label += L"  ";
        label += utf8ToWide(track.language);
    }
    if (label.empty()) label = L"Track " + std::to_wstring(track.streamIndex);
    if (!track.codec.empty()) label += L"  (" + utf8ToWide(track.codec) + L")";
    if (track.forced) label += L"  forced";
    return label;
}

// A menu takes `&` as the mark of a shortcut letter and a tab as the start
// of the key column; a file name means neither.
std::wstring menuText(std::wstring text) {
    std::wstring escaped;
    escaped.reserve(text.size());
    for (const wchar_t character : text) {
        if (character == L'\t') { escaped += L' '; continue; }
        escaped += character;
        if (character == L'&') escaped += L'&';
    }
    return escaped;
}

std::wstring signedSeconds(double value) {
    std::wostringstream text;
    text << std::showpos << std::fixed << std::setprecision(2) << value << L" s";
    return text.str();
}

std::wstring percentText(float value) {
    return std::to_wstring(static_cast<int>(std::lround(value * 100.0F))) + L"%";
}

}  // namespace

bool App::isSubtitleFile(const std::wstring& path) {
    return isSubtitleExtension(std::filesystem::path(path).extension().wstring());
}

// ---------------------------------------------------------------------------
// The languages wanted

std::vector<std::string> App::systemLanguages() const {
    if (systemLanguagesOverride_) return *systemLanguagesOverride_;
    if (!systemLanguagesRead_) {
        // The list Windows' language settings show -- the display language
        // and every other the viewer added -- which is more than the one
        // language the interface is in.
        std::vector<std::string> languages;
        DWORD size = 0;
        const wchar_t* key = L"Control Panel\\International\\User Profile";
        if (RegGetValueW(HKEY_CURRENT_USER, key, L"Languages", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &size) ==
                ERROR_SUCCESS && size > sizeof(wchar_t)) {
            std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
            if (RegGetValueW(HKEY_CURRENT_USER, key, L"Languages", RRF_RT_REG_MULTI_SZ, nullptr, buffer.data(),
                             &size) == ERROR_SUCCESS) {
                for (const wchar_t* entry = buffer.c_str(); *entry; entry += std::wcslen(entry) + 1) {
                    languages.push_back(wideToUtf8Text(entry));
                }
            }
        }
        if (languages.empty()) {
            wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
            if (GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH) > 0) languages.push_back(wideToUtf8Text(name));
        }
        systemLanguagesRead_ = std::move(languages);
    }
    return *systemLanguagesRead_;
}

std::vector<std::string> App::subtitleWanted() const {
    return subtitleLanguagePreference(clampSubtitleLanguage(subtitleSettings_.language), systemLanguages());
}

std::wstring App::subtitleLocale() const {
    const auto wanted = subtitleWanted();
    if (wanted.empty()) return {};
    const std::string& first = wanted.front();
    if (first == "zh-Hant") return L"zh-Hant";
    if (first == "zh-Hans" || first == "zh") return L"zh-Hans";
    if (first == "ja") return L"ja-JP";
    if (first == "ko") return L"ko-KR";
    return {};
}

SourceOptions App::localSourceOptions() const {
    SourceOptions options;
    options.readSubtitles = true;
    options.chooseSubtitle = [wanted = subtitleWanted()](const std::vector<SubtitleTrackInfo>& tracks) {
        std::vector<SubtitleCandidate> candidates;
        std::vector<int> streams;
        for (const auto& track : tracks) {
            if (!track.text) continue;
            candidates.push_back({track.language, utf8ToWideText(track.title), track.isDefault, track.forced, false});
            streams.push_back(track.streamIndex);
        }
        const int best = chooseSubtitle(candidates, wanted);
        return best >= 0 ? streams[static_cast<std::size_t>(best)] : -1;
    };
    return options;
}

// ---------------------------------------------------------------------------
// What a pane offers, and what it shows

std::vector<App::SubtitleOption> App::subtitleOptions(std::size_t pane) const {
    std::vector<SubtitleOption> options;
    if (pane >= sources_.size()) return options;
    const bool emby = embyPanes_[pane].has_value();
    if (emby) {
        for (const auto& stream : embyPanes_[pane]->streams) {
            if (stream.type != "Subtitle") continue;
            SubtitleOption option;
            // A stream inside the file is read from the file as it comes,
            // which the server sends unchanged: the server itself takes as
            // long to pull one out of a large file as to read the file.
            const bool inVideo = embyStreamReadWithVideo(pane, stream);
            option.what = {inVideo ? SubtitleKind::Embedded : SubtitleKind::EmbyStream, stream.index, {}};
            option.label = utf8ToWideText(emby::streamLabel(stream));
            const std::wstring title = utf8ToWideText(stream.title);
            if (!title.empty() && option.label.find(title) == std::wstring::npos) option.label += L"  \x00B7  " + title;
            option.usable = stream.isTextSubtitle;
            if (!option.usable) option.label += L"  (pictures, not shown)";
            // The title is the file name's own part for a file beside the
            // video; the display title spells the language out.
            option.candidate = {stream.language, title + L" " + utf8ToWideText(stream.displayTitle),
                                stream.isDefault, stream.isForced, stream.isExternal};
            options.push_back(std::move(option));
        }
    }
    const std::filesystem::path video = emby ? std::filesystem::path() : std::filesystem::path(paths_[pane]);
    for (const auto& file : subtitleFiles_[pane]) {
        SubtitleOption option;
        option.what = {SubtitleKind::File, -1, file};
        option.label = file.filename().wstring();
        option.candidate = {std::string(), sidecarSuffix(video, file), false, false, true};
        // A file already found to hold no line is not offered again: the
        // step key would stop at it each time round.
        const auto& empty = subtitleFilesWithoutLines_[pane];
        option.usable = std::find(empty.begin(), empty.end(), file) == empty.end();
        if (!option.usable) option.label += L"  (no lines)";
        options.push_back(std::move(option));
    }
    if (!emby && sources_[pane]) {
        for (const auto& track : sources_[pane]->subtitleTracks()) {
            SubtitleOption option;
            option.what = {SubtitleKind::Embedded, track.streamIndex, {}};
            option.label = subtitleTrackLabel(track);
            option.usable = track.text;
            if (!option.usable) option.label += L"  (pictures, not shown)";
            option.candidate = {track.language, utf8ToWideText(track.title), track.isDefault, track.forced, false};
            options.push_back(std::move(option));
        }
    }
    return options;
}

bool App::embyStreamReadWithVideo(std::size_t pane, const emby::MediaStream& stream) const {
    if (stream.isExternal || !stream.isTextSubtitle) return false;
    // The server numbers the streams inside a file as FFmpeg does.
    const auto& source = sources_[pane];
    if (!source || !source->ready()) return true;
    std::string header;
    bool plain = false;
    return source->subtitleStreamHeader(stream.index, header, plain);
}

std::wstring App::subtitleLabel(std::size_t pane) const {
    if (pane >= sources_.size() || subtitleSelection_[pane].kind == SubtitleKind::None) return L"Off";
    for (const auto& option : subtitleOptions(pane)) {
        if (option.what == subtitleSelection_[pane]) return option.label;
    }
    return subtitleSelection_[pane].kind == SubtitleKind::File
        ? subtitleSelection_[pane].file.filename().wstring() : L"Subtitles";
}

void App::selectSubtitle(std::size_t pane, const SubtitleSelection& selection, bool byViewer) {
    if (pane >= sources_.size()) return;
    if (byViewer) subtitleChosenByViewer_[pane] = true;
    subtitles_[pane].reset();
    subtitleSelection_[pane] = selection;
    subtitleCycleAnchor_[pane] = {};
    if (embyPanes_[pane]) embyPanes_[pane]->subtitleStream = -1;
    switch (selection.kind) {
    case SubtitleKind::EmbyStream:
        embyLoadSubtitle(pane, selection.stream);
        break;
    case SubtitleKind::File:
        loadSubtitleFile(pane, selection.file);
        break;
    case SubtitleKind::Embedded:
        // The source has been reading every text stream alongside the
        // video, so the lines of this one are there as far as it has read.
        if (sources_[pane]) sources_[pane]->setSubtitleTrack(selection.stream);
        if (embyPanes_[pane]) {
            appendDiagnostic("Emby: subtitle stream " + std::to_string(selection.stream) +
                             " is read from the video, pane=" + std::to_string(pane + 1));
        }
        break;
    default:
        break;
    }
}

void App::autoChooseSubtitle(std::size_t pane, int serverDefault) {
    if (pane >= sources_.size() || subtitleChosenByViewer_[pane]) return;
    const auto options = subtitleOptions(pane);
    const auto take = [&](const SubtitleOption& option) {
        if (!(subtitleSelection_[pane] == option.what)) selectSubtitle(pane, option.what, false);
    };
    // An Emby item's streams are one list, whether the server hands a
    // stream over or the video carries it.
    const bool emby = embyPanes_[pane].has_value();
    const auto group = [&](SubtitleKind kind) {
        return emby && kind == SubtitleKind::Embedded ? SubtitleKind::EmbyStream : kind;
    };
    // The account's own preference, where the server had one to apply.
    if (serverDefault >= 0) {
        for (const auto& option : options) {
            if (option.usable && group(option.what.kind) == SubtitleKind::EmbyStream &&
                option.what.stream == serverDefault) {
                take(option);
                return;
            }
        }
    }
    // Otherwise the language wanted; a file put beside the video before a
    // stream inside it.
    const auto wanted = subtitleWanted();
    for (const SubtitleKind kind : {SubtitleKind::EmbyStream, SubtitleKind::File, SubtitleKind::Embedded}) {
        std::vector<SubtitleCandidate> candidates;
        std::vector<std::size_t> indexes;
        for (std::size_t i = 0; i < options.size(); ++i) {
            if (!options[i].usable || group(options[i].what.kind) != kind) continue;
            candidates.push_back(options[i].candidate);
            indexes.push_back(i);
        }
        const int best = chooseSubtitle(candidates, wanted);
        if (best < 0) continue;
        take(options[indexes[static_cast<std::size_t>(best)]]);
        return;
    }
}

void App::adoptEmbeddedSubtitles() {
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (!subtitleEmbeddedPending_[pane]) continue;
        const auto& source = sources_[pane];
        if (!source || !source->error().empty()) {
            subtitleEmbeddedPending_[pane] = false;
            continue;
        }
        if (!source->ready()) continue;
        subtitleEmbeddedPending_[pane] = false;
        if (embyPanes_[pane]) {
            // The stream shown was taken to be inside the file before the
            // file was open; one the source does not read after all (more
            // text streams than it reads, a codec it cannot) is asked of the
            // server. The viewer's choice stays theirs: it is the same stream.
            const SubtitleSelection selection = subtitleSelection_[pane];
            if (selection.kind != SubtitleKind::Embedded) continue;
            std::string header;
            bool plain = false;
            if (source->subtitleStreamHeader(selection.stream, header, plain)) continue;
            appendDiagnostic("Emby: subtitle stream " + std::to_string(selection.stream) +
                             " is not read from the video; asking the server, pane=" + std::to_string(pane + 1));
            selectSubtitle(pane, {SubtitleKind::EmbyStream, selection.stream, {}}, false);
            continue;
        }
        if (subtitleChosenByViewer_[pane] || subtitleSelection_[pane].kind != SubtitleKind::None) continue;
        const int stream = source->subtitleTrack();
        if (stream < 0) continue;
        subtitleSelection_[pane] = {SubtitleKind::Embedded, stream, {}};
        appendDiagnostic("Subtitles: stream " + std::to_string(stream) + " inside the file is shown, pane=" +
                         std::to_string(pane + 1));
    }
}

// ---------------------------------------------------------------------------
// Files

App::SidecarScan App::scanSidecars(const std::wstring& path, const std::vector<std::string>& wanted) {
    SidecarScan scan;
    std::error_code code;
    const std::filesystem::path video(path);
    const std::wstring stem = video.stem().wstring();
    if (stem.empty()) return scan;
    for (std::filesystem::directory_iterator entry(video.parent_path(), code), end;
         !code && entry != end; entry.increment(code)) {
        if (!entry->is_regular_file(code)) continue;
        const auto& candidate = entry->path();
        if (!isSubtitleExtension(candidate.extension().wstring())) continue;
        // "movie.srt" and "movie.en.srt" both belong to "movie.mkv".
        if (subtitleNameFitsVideo(candidate.stem().wstring(), stem)) scan.files.push_back(candidate);
    }
    std::sort(scan.files.begin(), scan.files.end());
    if (scan.files.empty()) return scan;
    // The file whose name says the wanted language; failing that the first,
    // as before. One that holds no line gives way to the next best.
    std::vector<SubtitleCandidate> candidates;
    for (const auto& file : scan.files) {
        candidates.push_back({std::string(), sidecarSuffix(video, file), false, false, true});
    }
    std::size_t attempts = 0;
    for (const std::size_t index : rankSubtitles(candidates, wanted)) {
        if (attempts++ >= kSidecarReadAttempts) break;
        if (auto track = readSubtitleFile(scan.files[index])) {
            scan.chosen = static_cast<int>(index);
            scan.track = std::move(track);
            break;
        }
    }
    return scan;
}

void App::loadSidecarSubtitles(std::size_t pane, const std::wstring& path) {
    if (pane >= sources_.size()) return;
    subtitleFiles_[pane].clear();
    subtitles_[pane].reset();
    subtitleSelection_[pane] = {};
    const std::uint64_t serial = subtitleSerial_[pane] = ++subtitleSerialCounter_;
    if (emby::isLocator(path) || path.find(L"://") != std::wstring::npos) return;
    // The listing and the read happen off the window thread: a share that
    // has gone away must not freeze the player. The result comes back
    // through the main-thread queue and is dropped if the pane moved on.
    std::thread([queue = mainQueue_, this, serial, path, wanted = subtitleWanted()] {
        SidecarScan scan = scanSidecars(path, wanted);
        queue->post([this, serial, scan = std::move(scan)]() mutable {
            if (const auto pane = subtitlePaneWithSerial(serial)) applySidecarScan(*pane, serial, std::move(scan));
        });
    }).detach();
}

void App::applySidecarScan(std::size_t pane, std::uint64_t serial, SidecarScan scan) {
    if (pane >= sources_.size() || subtitleSerial_[pane] != serial) return;
    // A file loaded by hand meanwhile stays, after the ones found.
    auto files = std::move(scan.files);
    for (const auto& held : subtitleFiles_[pane]) {
        if (std::find(files.begin(), files.end(), held) == files.end()) files.push_back(held);
    }
    subtitleFiles_[pane] = std::move(files);
    if (scan.chosen < 0 || !scan.track || subtitleChosenByViewer_[pane]) return;
    // A file beside the video is shown rather than a stream inside it, as
    // most players do; the menu turns it off.
    subtitleSelection_[pane] = {SubtitleKind::File, -1, subtitleFiles_[pane][static_cast<std::size_t>(scan.chosen)]};
    subtitles_[pane] = std::move(scan.track);
}

std::optional<std::size_t> App::subtitlePaneWithSerial(std::uint64_t serial) const {
    if (serial == 0) return std::nullopt;
    for (std::size_t pane = 0; pane < subtitleSerial_.size(); ++pane) {
        if (subtitleSerial_[pane] == serial) return pane;
    }
    return std::nullopt;
}

void App::loadSubtitleFile(std::size_t pane, const std::filesystem::path& file) {
    if (pane >= sources_.size()) return;
    if (subtitleSerial_[pane] == 0) subtitleSerial_[pane] = ++subtitleSerialCounter_;
    const std::uint64_t serial = subtitleSerial_[pane];
    std::thread([queue = mainQueue_, this, serial, file] {
        auto track = readSubtitleFile(file);
        queue->post([this, serial, file, track = std::move(track)]() mutable {
            const auto found = subtitlePaneWithSerial(serial);
            if (!found) return;
            // Another choice made while the file was being read stands.
            const SubtitleSelection asked{SubtitleKind::File, -1, file};
            if (!(subtitleSelection_[*found] == asked)) return;
            if (!track) {
                auto& empty = subtitleFilesWithoutLines_[*found];
                if (std::find(empty.begin(), empty.end(), file) == empty.end()) empty.push_back(file);
                subtitleSelection_[*found] = {};
                showNotice(L"No subtitle lines in " + file.filename().wstring());
                // Chosen by the player, not the viewer: the next best takes
                // its place. The viewer's own choice is left at nothing.
                if (!subtitleChosenByViewer_[*found]) autoChooseSubtitle(*found);
                return;
            }
            appendDiagnostic("Subtitles: file loaded, " + std::to_string(track->cues.size()) + " cues, pane=" +
                             std::to_string(*found + 1));
            subtitles_[*found] = std::move(track);
        });
    }).detach();
}

void App::addSubtitleFile(std::size_t pane, const std::filesystem::path& file) {
    if (pane >= sources_.size() || !paneLogicallyLoaded(pane)) return;
    auto& files = subtitleFiles_[pane];
    if (std::find(files.begin(), files.end(), file) == files.end()) files.push_back(file);
    selectSubtitle(pane, {SubtitleKind::File, -1, file}, true);
    showNotice(L"Subtitles: " + file.filename().wstring());
}

void App::loadSubtitleDialog(std::size_t pane) {
    if (pane >= sources_.size() || !paneLogicallyLoaded(pane)) return;
    wchar_t path[MAX_PATH]{};
    // Beside the video is where its subtitles usually are.
    std::wstring folder;
    if (!paths_[pane].empty() && !emby::isLocator(paths_[pane]) && paths_[pane].find(L"://") == std::wstring::npos) {
        folder = std::filesystem::path(paths_[pane]).parent_path().wstring();
    }
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = window_;
    dialog.lpstrFilter = L"Subtitles (*.srt;*.ass;*.ssa;*.vtt)\0*.srt;*.ass;*.ssa;*.vtt\0All files\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = MAX_PATH;
    dialog.lpstrInitialDir = folder.empty() ? nullptr : folder.c_str();
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    // The player goes on playing under the dialog and may have moved the
    // pane on to its next video, or swapped it, by the time a file is
    // picked: the file is for the video the dialog was opened on.
    const std::uint64_t serial = subtitleSerial_[pane];
    if (!GetOpenFileNameW(&dialog)) return;
    if (serial != 0) {
        const auto found = subtitlePaneWithSerial(serial);
        if (!found) {
            showNotice(L"That video is no longer playing; its subtitles were not loaded");
            return;
        }
        pane = *found;
    }
    addSubtitleFile(pane, path);
}

// ---------------------------------------------------------------------------
// The viewer's choices

int App::subtitlePane() const {
    if (const int pane = commandPane(); pane >= 0) return pane;
    if (const int single = singleLoadedPane(); single >= 0) return single;
    int first = -1;
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (!paneLogicallyLoaded(pane)) continue;
        if (subtitleSelection_[pane].kind != SubtitleKind::None) return static_cast<int>(pane);
        if (first < 0) first = static_cast<int>(pane);
    }
    return first;
}

void App::toggleSubtitlesShown() {
    subtitleSettings_.show = !subtitleSettings_.show;
    showNotice(subtitleSettings_.show ? L"Subtitles shown" : L"Subtitles hidden");
    scheduleAppSettingsSave();
}

void App::cycleSubtitle(std::size_t pane) {
    if (pane >= sources_.size()) return;
    const auto options = subtitleOptions(pane);
    const auto usable = static_cast<std::size_t>(std::count_if(
        options.begin(), options.end(), [](const SubtitleOption& option) { return option.usable; }));
    if (usable == 0) {
        showNotice(L"This video has no subtitles  (Alt+O loads a file)");
        return;
    }
    // Off, then each in turn, then off again. The step goes on from what is
    // shown -- or from what the last step chose, when that was a file that
    // turned out to hold no line and nothing is shown for it.
    const SubtitleSelection from = subtitleSelection_[pane].kind != SubtitleKind::None
        ? subtitleSelection_[pane] : subtitleCycleAnchor_[pane];
    std::size_t start = 0;
    if (from.kind != SubtitleKind::None) {
        for (std::size_t i = 0; i < options.size(); ++i) {
            if (options[i].what == from) start = i + 1;
        }
    }
    std::size_t next = start;
    while (next < options.size() && !options[next].usable) ++next;
    if (next >= options.size()) {
        selectSubtitle(pane, {}, true);
        showNotice(L"Subtitles off");
        return;
    }
    const SubtitleOption& option = options[next];
    selectSubtitle(pane, option.what, true);
    subtitleCycleAnchor_[pane] = option.what;
    const auto number = std::count_if(options.begin(), options.begin() + static_cast<std::ptrdiff_t>(next) + 1,
                                      [](const SubtitleOption& counted) { return counted.usable; });
    showNotice(L"Subtitles " + std::to_wstring(number) + L"/" + std::to_wstring(usable) + L":  " + option.label);
}

void App::nudgeSubtitleDelay(std::size_t pane, double seconds) {
    if (pane >= sources_.size()) return;
    subtitleDelay_[pane] = std::clamp(subtitleDelay_[pane] + seconds, -kSubtitleDelayLimit, kSubtitleDelayLimit);
    if (std::abs(subtitleDelay_[pane]) < 0.0005) subtitleDelay_[pane] = 0.0;
    showNotice(std::wstring(L"Subtitles ") + (seconds < 0.0 ? L"earlier" : L"later") + L"  (" +
               signedSeconds(subtitleDelay_[pane]) + L")");
}

void App::resetSubtitleDelay(std::size_t pane) {
    if (pane >= sources_.size()) return;
    subtitleDelay_[pane] = 0.0;
    showNotice(L"Subtitles timed as their file says");
}

void App::setSubtitleSize(float size) {
    subtitleSettings_.size = size;
    subtitleSettings_ = clampSubtitleSettings(subtitleSettings_);
    showNotice(L"Subtitle size " + percentText(subtitleSettings_.size));
    scheduleAppSettingsSave();
}

void App::setSubtitlePosition(float position) {
    subtitleSettings_.position = position;
    subtitleSettings_ = clampSubtitleSettings(subtitleSettings_);
    showNotice(subtitleSettings_.position > 0.0F
                   ? L"Subtitles raised " + percentText(subtitleSettings_.position)
                   : std::wstring(L"Subtitles where their style puts them"));
    scheduleAppSettingsSave();
}

void App::setSubtitleLanguage(int language) {
    subtitleSettings_.language = language;
    subtitleSettings_ = clampSubtitleSettings(subtitleSettings_);
    showNotice(std::wstring(L"Subtitle language: ") +
               subtitleLanguageName(clampSubtitleLanguage(subtitleSettings_.language)));
    // What is playing follows, where the viewer has not chosen for it.
    for (std::size_t pane = 0; pane < sources_.size(); ++pane) {
        if (paneLogicallyLoaded(pane)) autoChooseSubtitle(pane);
    }
    scheduleAppSettingsSave();
}

bool App::handleAltKey(WPARAM key) {
    const int pane = subtitlePane();
    switch (key) {
    case 'H': toggleSubtitlesShown(); return true;
    case 'L': if (pane >= 0) cycleSubtitle(static_cast<std::size_t>(pane)); return true;
    case 'O': if (pane >= 0) loadSubtitleDialog(static_cast<std::size_t>(pane)); return true;
    case VK_PRIOR: setSubtitleSize(subtitleSettings_.size + 0.1F); return true;
    case VK_NEXT: setSubtitleSize(subtitleSettings_.size - 0.1F); return true;
    case VK_UP: setSubtitlePosition(subtitleSettings_.position + 0.02F); return true;
    case VK_DOWN: setSubtitlePosition(subtitleSettings_.position - 0.02F); return true;
    case VK_HOME: setSubtitlePosition(kSubtitleDefaultPosition); return true;
    default: return false;
    }
}

// ---------------------------------------------------------------------------
// libass

PlainSubtitleStyle App::plainSubtitleStyle() const {
    return {plainSubtitleFont(subtitleWanted()), subtitleSettings_.background};
}

bool App::syncAssSubtitles(std::size_t pane) {
    if (assDisabled_ || pane >= sources_.size() || !sources_[pane]) return false;
    const SubtitleSelection& selection = subtitleSelection_[pane];
    if (selection.kind == SubtitleKind::None) return false;
    AssPane& state = assPanes_[pane];
    // One libass per video: the fonts a video brought are not another's.
    // One that could not be set up is not tried again for the same video.
    if (state.serial != subtitleSerial_[pane] || (!state.subtitles && !state.failed)) {
        state = AssPane{};
        state.serial = subtitleSerial_[pane];
        state.subtitles = AssSubtitles::create();
        if (!state.subtitles) {
            state.failed = true;
            appendDiagnostic("Subtitles: libass could not be set up; plain lines are drawn, pane=" +
                             std::to_string(pane + 1));
        }
    }
    if (!state.subtitles) return false;
    AssSubtitles& ass = *state.subtitles;
    const PlainSubtitleStyle plain = plainSubtitleStyle();
    ass.setFallbackFont(plain.font);
    if (!state.fontsTaken && sources_[pane]->ready()) {
        state.fontsTaken = true;
        if (const auto fonts = sources_[pane]->fonts(); fonts && !fonts->empty()) ass.addFonts(*fonts);
    }

    if (selection.kind == SubtitleKind::Embedded) {
        // A stream inside the file: its header once, then its events as the
        // video worker reads them, each handed over once.
        const bool current = !state.track && state.stream == selection.stream &&
                             (!state.plain || state.plainStyle == plain);
        if (!current) {
            std::string header;
            bool streamPlain = false;
            // Not open yet, or not one that is read: the plain lines, if any.
            if (!sources_[pane]->subtitleStreamHeader(selection.stream, header, streamPlain)) return false;
            // An ASS stream without its header (a Matroska track missing its
            // CodecPrivate) names no format for its events, and libass would
            // take none of them: it is drawn under the plain header instead.
            if (header.empty()) streamPlain = true;
            state.track.reset();
            state.stream = selection.stream;
            state.events = 0;
            state.plain = streamPlain;
            state.plainStyle = plain;
            // FFmpeg gives a plain stream a stand-in header of its own; it is
            // drawn like a plain file instead.
            state.failed = !ass.startStream(streamPlain ? plainSubtitleHeader(plain) : header, streamPlain);
        }
        if (state.failed) return false;
        std::vector<SubtitleEvent> events;
        state.events = sources_[pane]->subtitleEventsSince(selection.stream, state.events, events);
        for (const auto& event : events) ass.addEvent(event);
        return true;
    }

    // A file, or a server's stream: read and parsed whole off this thread.
    const auto& track = subtitles_[pane];
    if (!track || track->text.empty()) return false;
    const bool current = state.track == track && (!state.plain || state.plainStyle == plain);
    if (!current) {
        state.track = track;
        state.stream = -1;
        state.events = 0;
        state.plain = !track->ass;
        state.plainStyle = plain;
        std::string script = track->ass ? track->text : plainSubtitleScript(track->text, plain);
        state.failed = !ass.loadScript(std::move(script), state.plain);
        if (state.failed) {
            appendDiagnostic("Subtitles: libass read no event from the script; plain lines are drawn, pane=" +
                             std::to_string(pane + 1));
        }
    }
    return !state.failed;
}

std::shared_ptr<const SubtitleBitmap> App::renderAssSubtitles(std::size_t pane, double seconds, bool& drawn,
                                                              POINT& origin) {
    drawn = false;
    if (!syncAssSubtitles(pane)) return nullptr;
    drawn = true;
    const RectF& cell = chromeCells_[pane];
    const int videoWidth = sources_[pane]->videoWidth();
    const int videoHeight = sources_[pane]->videoHeight();
    const ViewMode mode = paneViews_[pane].mode;
    const AssPane& state = assPanes_[pane];
    const SubtitlePlacement placement = subtitlePlacement(
        cell, visiblePictureRect(cell, videoWidth, videoHeight, mode),
        pictureRect(cell, videoWidth, videoHeight, mode, paneViews_[pane].zoom), videoWidth, videoHeight,
        state.plain);
    const SubtitleFrame& frame = placement.frame;
    if (frame.width <= 0 || frame.height <= 0) return nullptr;
    origin = {placement.originX, placement.originY};

    SubtitleLook look;
    look.fontScale = subtitleSettings_.size;
    look.videoMatrix = sources_[pane]->colourMatrix();
    // libass raises the bottom lines by a share of the height between their
    // place and the top of the frame.
    double raise = static_cast<double>(subtitleSettings_.position) * 100.0;
    // Clear of the transport bar while it is up, as far as it reaches into
    // the frame -- at once when it is half in, not with every frame of its
    // slide, for each change of the line position empties libass's caches.
    if (dockProgress_ > 0.5F) {
        const double cover = paneCoveredBottom(cell, true) -
                             (static_cast<double>(cell.y + cell.height) - (placement.originY + frame.height));
        if (cover > 0.0) raise += 100.0 * cover / frame.height;
    }
    look.raise = std::min(std::round(raise * 10.0) / 10.0, 100.0);
    return state.subtitles->render(frame, look, std::llround(seconds * 1000.0));
}

// ---------------------------------------------------------------------------
// The menu

void App::appendSubtitleMenu(HMENU menu, std::size_t pane) const {
    const bool loaded = pane < sources_.size() && paneLogicallyLoaded(pane);
    AppendMenuW(menu, MF_STRING | (subtitleSettings_.show ? MF_CHECKED : 0), CmdSubtitleShow,
                L"Show subtitles\tAlt+H");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const auto options = loaded ? subtitleOptions(pane) : std::vector<SubtitleOption>{};
    const SubtitleSelection selection = loaded ? subtitleSelection_[pane] : SubtitleSelection{};
    // Kept for the command the menu returns: the player goes on running
    // under an open menu, and a file found meanwhile would shift the places.
    subtitleMenuOptions_ = options;
    subtitleMenuPane_ = loaded ? static_cast<int>(pane) : -1;
    AppendMenuW(menu, MF_STRING | (selection.kind == SubtitleKind::None ? MF_CHECKED : 0) |
                (loaded ? 0 : MF_GRAYED), CmdSubtitleOff, L"Off");
    for (std::size_t choice = 0; choice < options.size() && choice < kSubtitleMenuLimit; ++choice) {
        AppendMenuW(menu, MF_STRING | (options[choice].what == selection ? MF_CHECKED : 0) |
                    (options[choice].usable ? 0 : MF_GRAYED),
                    CmdSubtitleFirst + static_cast<unsigned>(choice), menuText(options[choice].label).c_str());
    }
    if (options.size() > kSubtitleMenuLimit) {
        const std::wstring more = std::to_wstring(options.size() - kSubtitleMenuLimit) +
                                  L" more  (Alt+L steps through them all)";
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, more.c_str());
    }
    AppendMenuW(menu, MF_STRING | (options.empty() ? MF_GRAYED : 0), CmdSubtitleCycle, L"Next subtitle\tAlt+L");
    AppendMenuW(menu, MF_STRING | (loaded ? 0 : MF_GRAYED), CmdSubtitleLoad, L"Load subtitle file...\tAlt+O");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const bool timed = loaded && selection.kind != SubtitleKind::None;
    AppendMenuW(menu, MF_STRING | (timed ? 0 : MF_GRAYED), CmdSubtitleEarlier, L"Earlier by 0.5 s\t.");
    AppendMenuW(menu, MF_STRING | (timed ? 0 : MF_GRAYED), CmdSubtitleLater, L"Later by 0.5 s\t,");
    const std::wstring reset = L"Timing as in the file  (now " +
                               signedSeconds(loaded ? subtitleDelay_[pane] : 0.0) + L")\t/";
    AppendMenuW(menu, MF_STRING | (timed && subtitleDelay_[pane] != 0.0 ? 0 : MF_GRAYED), CmdSubtitleSyncReset,
                reset.c_str());
}

void App::showSubtitleMenu(int pane, POINT screenPoint) {
    contextPane_ = pane;
    contextMenuOpen_ = true;
    HMENU menu = CreatePopupMenu();
    if (!menu) { contextMenuOpen_ = false; contextPane_ = -1; return; }
    appendSubtitleMenu(menu, pane >= 0 ? static_cast<std::size_t>(pane) : kMaxPanes);
    const unsigned command = TrackPopupMenuEx(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, window_, nullptr);
    if (command) handleContextCommand(command, pane);
    DestroyMenu(menu);
    subtitleMenuOptions_.clear();
    subtitleMenuPane_ = -1;
    contextMenuOpen_ = false;
    contextPane_ = -1;
}

}  // namespace quaddeck
