#pragma once

// The folder a local video plays from, as a list: what the F6 sheet shows
// beside the video and what Page Up/Down, the bar's step buttons and the
// playback order walk. Pure: listing the folder and comparing names the
// way Explorer does are the caller's; this model performs no disk access.

#include "EmbyApi.hpp"

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace quaddeck {

struct LocalEntry {
    std::wstring path;
    std::uintmax_t size{};
    // Last write time in the file clock's ticks; 0 when it could not be read.
    std::int64_t written{};
    // Length in seconds; 0 until it has been read, or when there is none.
    double duration{};
    // A subtitle file beside it that playing it would load, as the folder
    // was read.
    bool subtitleFile{};
    // A text subtitle stream inside it, once the file has been read.
    bool subtitleStream{};
};

// What reading a file says of it: its length, and whether a subtitle
// stream the player shows is inside.
struct LocalProbe {
    double duration{};
    bool subtitleStream{};
    // The file as it was listed when read: one listed since at another size
    // or date is read again.
    std::uintmax_t size{};
    std::int64_t written{};
    // Whether it opened at all. A forced Refresh tries again a file that did
    // not, such as one read while a share hiccuped; one that opened and has
    // not changed is not read again.
    bool opened{};
};

// Whether what was read of a file still describes it as listed now.
inline bool localProbeCurrent(const LocalProbe& probe, const LocalEntry& entry) {
    return probe.size == entry.size && probe.written == entry.written;
}

// Whether the list marks a file as one with subtitles: a file of its name
// beside it or a text stream inside, either of which playing it shows.
inline bool localHasSubtitles(const LocalEntry& entry) {
    return entry.subtitleFile || entry.subtitleStream;
}

// Orders entries already sorted by name (Explorer's order): by size, date
// or length on top of that, reversed on request, or shuffled -- one shuffle
// per seed, so the order holds while a folder is being walked. Release and
// last played mean nothing to a folder and stay by name.
inline void orderLocalEntries(std::vector<LocalEntry>& entries, emby::SortKey key, bool descending,
                              std::uint64_t seed) {
    if (key == emby::SortKey::Random) {
        std::mt19937_64 order(seed);
        std::shuffle(entries.begin(), entries.end(), order);
        return;
    }
    if (key == emby::SortKey::Runtime) {
        // A length not read yet comes after every one that has been,
        // whichever way the rest run, and takes its place when it arrives.
        std::stable_sort(entries.begin(), entries.end(), [&](const LocalEntry& left, const LocalEntry& right) {
            const bool leftKnown = left.duration > 0.0;
            const bool rightKnown = right.duration > 0.0;
            if (leftKnown != rightKnown) return leftKnown;
            if (!leftKnown) return false;
            return descending ? left.duration > right.duration : left.duration < right.duration;
        });
        return;
    }
    if (key == emby::SortKey::Size || key == emby::SortKey::DateAdded) {
        std::stable_sort(entries.begin(), entries.end(), [&](const LocalEntry& left, const LocalEntry& right) {
            return key == emby::SortKey::Size ? left.size < right.size : left.written < right.written;
        });
    }
    if (descending) std::reverse(entries.begin(), entries.end());
}

inline std::wstring localFileName(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

inline std::wstring localDirectory(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// Within one folder the name identifies the file, however the path that
// reached the player was spelled. A list of every video under a folder can
// hold one name twice, in two of its folders: the file of the path's own
// folder is that one, and the first of its name only when none is.
inline int localEntryIndex(const std::vector<LocalEntry>& entries, const std::wstring& path) {
    const std::wstring name = localFileName(path);
    const std::size_t folderLength = path.size() - name.size();
    int byName = -1;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        // The name in place: the sheet asks this of every pane each frame.
        const auto& candidate = entries[i].path;
        const auto slash = candidate.find_last_of(L"\\/");
        const std::size_t nameAt = slash == std::wstring::npos ? 0 : slash + 1;
        if (_wcsicmp(candidate.c_str() + nameAt, name.c_str()) != 0) continue;
        if (nameAt == folderLength && _wcsnicmp(candidate.c_str(), path.c_str(), folderLength) == 0) {
            return static_cast<int>(i);
        }
        if (byName < 0) byName = static_cast<int>(i);
    }
    return byName;
}

inline bool sameLocalDirectory(const std::wstring& left, const std::wstring& right) {
    return !left.empty() && _wcsicmp(left.c_str(), right.c_str()) == 0;
}

inline bool isLocalSeparator(wchar_t character) { return character == L'\\' || character == L'/'; }

// The path a folder is read by. A drive's top folder is "D:" as a file's
// folder ("D:\clip.mp4"), and "D:" alone would be that drive's current folder.
inline std::wstring localListingPath(const std::wstring& directory) {
    if (directory.size() == 2 && directory[1] == L':') return directory + L"\\";
    return directory;
}

// The folder above one the list shows, or nothing at the top of a drive or
// a share: "D:" above "D:\Videos", "\\nas\share" above "\\nas\share\Videos",
// and nothing above either of those.
inline std::wstring localParentFolder(const std::wstring& directory) {
    std::wstring folder = directory;
    while (folder.size() > 1 && isLocalSeparator(folder.back())) folder.pop_back();
    if (folder.size() > 2 && isLocalSeparator(folder[0]) && isLocalSeparator(folder[1])) {
        // \\server\share is a share's top; a server alone cannot be listed.
        const auto separators = std::count_if(folder.begin() + 2, folder.end(), isLocalSeparator);
        if (separators < 2) return {};
    }
    const auto slash = folder.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0) return {};
    return folder.substr(0, slash);
}

// Whether a path is in a folder or any folder under it.
inline bool localPathUnder(const std::wstring& path, const std::wstring& directory) {
    if (directory.empty() || path.size() <= directory.size()) return false;
    if (_wcsnicmp(path.c_str(), directory.c_str(), directory.size()) != 0) return false;
    return isLocalSeparator(directory.back()) || isLocalSeparator(path[directory.size()]);
}

// The folders between a folder and a file under it: "Season 1" for
// "D:\Show\Season 1\E01.mkv" under "D:\Show", nothing for a file in it.
inline std::wstring localRelativeFolder(const std::wstring& path, const std::wstring& directory) {
    if (!localPathUnder(path, directory)) return {};
    const std::wstring folder = localDirectory(path);
    std::size_t start = directory.size();
    while (start < folder.size() && isLocalSeparator(folder[start])) ++start;
    return start < folder.size() ? folder.substr(start) : std::wstring();
}

// Where the list is, as the browser's header says it: the folder's last two
// names under an ellipsis -- "… › Show › Season 1" -- or all of them when
// there are no more than three: "D: › Show".
inline std::wstring localFolderCrumb(const std::wstring& directory) {
    std::vector<std::wstring> names;
    std::wstring name;
    for (const wchar_t character : directory) {
        if (isLocalSeparator(character)) {
            if (!name.empty()) names.push_back(std::move(name));
            name.clear();
        } else {
            name += character;
        }
    }
    if (!name.empty()) names.push_back(std::move(name));
    if (names.empty()) return L"Folder";
    std::size_t first = 0;
    std::wstring crumb;
    if (names.size() > 3) {
        first = names.size() - 2;
        crumb = L"\x2026";
    }
    for (std::size_t i = first; i < names.size(); ++i) {
        if (!crumb.empty()) crumb += L" \x203A ";
        crumb += names[i];
    }
    return crumb;
}

// Every video under a folder (All videos) is read only so far: a share's
// whole tree for one list has to end somewhere, and so does a link that
// leads back above itself.
inline constexpr std::size_t kLocalFlatLimit = 5000;
inline constexpr int kLocalFlatDepth = 16;

// A folder's row in the list is named by a param of its own, counting down
// from this for the folders in order; -1 names no row, and a video's param
// is its place in the list.
inline constexpr int kLocalFolderParam = -2;

// What reading a folder for the list gives: its videos, and the folders in
// it -- or, for All videos, every video under it, each folder's own before
// those of the folders in it, and no folders.
struct LocalListing {
    std::vector<LocalEntry> entries;
    std::vector<std::wstring> folders;
    std::wstring error;
    // All videos stopped at kLocalFlatLimit.
    bool capped{};
};

// A file's name without its last extension: "movie.chs" of "movie.chs.ass".
inline std::wstring localStem(const std::wstring& path) {
    const std::wstring name = localFileName(path);
    const auto dot = name.find_last_of(L'.');
    return dot == std::wstring::npos || dot == 0 ? name : name.substr(0, dot);
}

// Whether a subtitle file is one of a video's, by their names without
// extensions: the same name ("movie.srt" for "movie.mkv"), or the name and
// a dot ("movie.chs.ass"), in any case.
inline bool subtitleNameFitsVideo(const std::wstring& subtitleStem, const std::wstring& videoStem) {
    if (videoStem.empty() || subtitleStem.size() < videoStem.size()) return false;
    for (std::size_t i = 0; i < videoStem.size(); ++i) {
        if (std::towlower(subtitleStem[i]) != std::towlower(videoStem[i])) return false;
    }
    return subtitleStem.size() == videoStem.size() || subtitleStem[videoStem.size()] == L'.';
}

// The video a subtitle file belongs to among the videos of its folder, or
// -1: the longest name it fits, since "Show.S01E01.chs.ass" is
// "Show.S01E01.mkv"'s and not "Show.mkv"'s; of two videos with one name,
// the first in the folder's order.
inline int videoForSubtitle(const std::wstring& subtitle, const std::vector<LocalEntry>& videos) {
    const std::wstring stem = localStem(subtitle);
    int best = -1;
    std::size_t bestLength = 0;
    for (std::size_t i = 0; i < videos.size(); ++i) {
        const std::wstring video = localStem(videos[i].path);
        if (video.size() > bestLength && subtitleNameFitsVideo(stem, video)) {
            best = static_cast<int>(i);
            bestLength = video.size();
        }
    }
    return best;
}

// Marks the videos of a folder that one of its subtitle files belongs to,
// by the rule playing a video loads them by: every video whose name the
// file's fits, not only the longest, since playing "Show.mkv" loads
// "Show.S01E01.chs.ass" as well.
inline void markSubtitleFiles(std::vector<LocalEntry>& videos, const std::vector<std::wstring>& subtitles) {
    std::vector<std::wstring> stems;
    stems.reserve(subtitles.size());
    for (const auto& subtitle : subtitles) stems.push_back(localStem(subtitle));
    for (auto& video : videos) {
        const std::wstring stem = localStem(video.path);
        video.subtitleFile = std::any_of(stems.begin(), stems.end(), [&](const std::wstring& subtitle) {
            return subtitleNameFitsVideo(subtitle, stem);
        });
    }
}

// "594 MB", "1.4 GB", "12 KB": what the list says beside a file.
inline std::wstring formatFileSize(std::uintmax_t bytes) {
    constexpr double kilo = 1024.0;
    const double value = static_cast<double>(bytes);
    // Into a buffer on the stack rather than a stream: a folder's list asks
    // this of every row it fills.
    wchar_t text[64];
    int length = 0;
    if (value >= kilo * kilo * kilo) {
        length = std::swprintf(text, std::size(text), value >= 10.0 * kilo * kilo * kilo ? L"%.0f GB" : L"%.1f GB",
                               value / (kilo * kilo * kilo));
    } else if (value >= kilo * kilo) {
        length = std::swprintf(text, std::size(text), L"%.0f MB", value / (kilo * kilo));
    } else if (value >= kilo) {
        length = std::swprintf(text, std::size(text), L"%.0f KB", value / kilo);
    } else {
        length = std::swprintf(text, std::size(text), L"%llu B", static_cast<unsigned long long>(bytes));
    }
    return length > 0 ? std::wstring(text, static_cast<std::size_t>(length)) : std::wstring();
}

// The sorts a folder can answer, in the order the sheet offers them, and
// where each sits among the browser's sort keys.
inline constexpr int kLocalSortCount = 5;

inline emby::SortKey localSortKey(int choice) {
    switch (choice) {
    case 1: return emby::SortKey::Size;
    case 2: return emby::SortKey::DateAdded;
    case 3: return emby::SortKey::Runtime;
    case 4: return emby::SortKey::Random;
    default: return emby::SortKey::Name;
    }
}

inline int localSortChoice(emby::SortKey key) {
    switch (key) {
    case emby::SortKey::Name: return 0;
    case emby::SortKey::Size: return 1;
    case emby::SortKey::DateAdded: return 2;
    case emby::SortKey::Runtime: return 3;
    case emby::SortKey::Random: return 4;
    default: return -1;   // an order a folder does not have: shown by name
    }
}

inline const wchar_t* localSortName(int choice) {
    switch (choice) {
    case 1: return L"Size";
    case 2: return L"Date";
    case 3: return L"Length";
    case 4: return L"Random";
    default: return L"Name";
    }
}

// The cache key of a local file's picture: "file|<width>|<utf-8 path>". A
// path cannot hold the bar, so the first two are the only separators.
inline std::string localThumbnailKey(const std::wstring& path, int width) {
    if (path.empty()) return {};
    return "file|" + std::to_string(std::max(1, width)) + "|" + wideToUtf8Text(path);
}

struct LocalThumbnailKey {
    std::wstring path;
    int width{};
};

inline std::optional<LocalThumbnailKey> parseLocalThumbnailKey(const std::string& key) {
    if (key.rfind("file|", 0) != 0) return std::nullopt;
    const auto second = key.find('|', 5);
    if (second == std::string::npos) return std::nullopt;
    LocalThumbnailKey parsed;
    try {
        parsed.width = std::stoi(key.substr(5, second - 5));
    } catch (...) {
        return std::nullopt;
    }
    parsed.path = utf8ToWideText(key.substr(second + 1));
    if (parsed.width <= 0 || parsed.path.empty()) return std::nullopt;
    return parsed;
}

}  // namespace quaddeck
