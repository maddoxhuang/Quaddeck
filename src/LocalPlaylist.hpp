#pragma once

// The folder a local video plays from, as a list: what the F6 sheet shows
// beside the video and what Page Up/Down, the bar's step buttons and the
// playback order walk. Pure: listing the folder and comparing names the
// way Explorer does are the caller's, so this can be tested without a disk.

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
};

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
// reached the player was spelled.
inline int localEntryIndex(const std::vector<LocalEntry>& entries, const std::wstring& path) {
    const std::wstring name = localFileName(path);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (_wcsicmp(localFileName(entries[i].path).c_str(), name.c_str()) == 0) return static_cast<int>(i);
    }
    return -1;
}

inline bool sameLocalDirectory(const std::wstring& left, const std::wstring& right) {
    return !left.empty() && _wcsicmp(left.c_str(), right.c_str()) == 0;
}

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
    std::wostringstream out;
    const double value = static_cast<double>(bytes);
    if (value >= kilo * kilo * kilo) {
        out << std::fixed << std::setprecision(value >= 10.0 * kilo * kilo * kilo ? 0 : 1)
            << value / (kilo * kilo * kilo) << L" GB";
    } else if (value >= kilo * kilo) {
        out << std::fixed << std::setprecision(0) << value / (kilo * kilo) << L" MB";
    } else if (value >= kilo) {
        out << std::fixed << std::setprecision(0) << value / kilo << L" KB";
    } else {
        out << bytes << L" B";
    }
    return out.str();
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
