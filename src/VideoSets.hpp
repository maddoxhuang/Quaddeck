#pragma once

// The video sets: QuadDeck's own list of decks to play whole. A set is the
// videos that played together, in their places on the screen, and their
// arrangement -- nothing of where they stood or how they were tuned. While
// one plays, Previous and Next step to the set before or after it, and the
// list's own order says what follows when it ends (AppVideoSets.cpp). The
// list is kept beside the settings and leaves the player only when it is
// exported to a .qdeck file. Pure: the QSETS format, the steps, the names
// and the merge.

#include "Core.hpp"

#include <algorithm>
#include <cstddef>
#include <iomanip>
#include <istream>
#include <ostream>
#include <string>
#include <vector>

namespace quaddeck {

// One video of a set: what its pane held -- a file's path, or an Emby
// item's locator, emby://<server>/<item> -- and, for an Emby item, what it
// was when the set was saved. That names it when it cannot be played, and
// finds it again when the server has given it another id (a rescan after
// the file was moved or renamed does).
struct VideoSetVideo {
    std::wstring path;
    std::wstring title;       // as its pane named it
    std::wstring serverName;  // the Emby server's name
    std::wstring serverPath;  // the file's path on the server
    std::wstring name;        // the item's own name
    std::wstring seriesName;
    std::wstring type;        // Movie, Episode, Video, ...
    int season{-1};
    int episode{-1};
    int year{};
    friend bool operator==(const VideoSetVideo&, const VideoSetVideo&) = default;
};

struct VideoSet {
    std::wstring name;
    LayoutMode layout{LayoutMode::Grid2x2};
    // The place on the screen shown larger, -1 for none.
    int expanded{-1};
    // By their places on the screen, V1 first; one to five.
    std::vector<VideoSetVideo> videos;
};

struct VideoSetList {
    std::vector<VideoSet> sets;
    // What follows a set's end, as the play order does for one video:
    // stop, the same set again, the next one (wrapping round or not), or
    // another at random.
    PlayOrder order{PlayOrder::InOrder};
};

inline constexpr int kVideoSetsVersion = 1;
inline constexpr std::size_t kVideoSetLimit = 10000;
inline constexpr std::size_t kVideoSetNameLimit = 120;
inline constexpr int kLayoutModeCount = 6;

inline bool sameVideos(const VideoSet& left, const VideoSet& right) {
    return left.videos.size() == right.videos.size() &&
           std::equal(left.videos.begin(), left.videos.end(), right.videos.begin(),
                      [](const VideoSetVideo& a, const VideoSetVideo& b) { return a.path == b.path; });
}

// The index of a set with the same videos in the same places, -1 for none.
inline int findSameVideos(const VideoSetList& list, const VideoSet& set) {
    for (std::size_t index = 0; index < list.sets.size(); ++index) {
        if (sameVideos(list.sets[index], set)) return static_cast<int>(index);
    }
    return -1;
}

// What a set is called until it is renamed: its first video's title, and
// how many more it has.
inline std::wstring videoSetDefaultName(const std::vector<std::wstring>& titles) {
    if (titles.empty()) return L"Set";
    std::wstring name = titles.front().empty() ? std::wstring(L"Video") : titles.front();
    if (name.size() > kVideoSetNameLimit) name = name.substr(0, kVideoSetNameLimit - 1) + L"\x2026";
    if (titles.size() > 1) name += L" + " + std::to_wstring(titles.size() - 1);
    return name;
}

// A typed name as it is kept: no line breaks or other control characters,
// no spaces at its ends, no longer than the limit; empty when nothing is left.
inline std::wstring cleanVideoSetName(std::wstring name) {
    for (auto& character : name) {
        if (character < 0x20 || character == 0x7F) character = L' ';
    }
    const auto first = name.find_first_not_of(L' ');
    if (first == std::wstring::npos) return {};
    name = name.substr(first, name.find_last_not_of(L' ') - first + 1);
    if (name.size() > kVideoSetNameLimit) name.resize(kVideoSetNameLimit);
    return name;
}

// The set Previous (-1) or Next (+1) leads to from `current` of `count`:
// the one before or after, round the ends only under Repeat the list;
// under Shuffle any other at random (`random` decides which). -1 for none.
inline int videoSetStep(PlayOrder order, int current, int count, int step, unsigned random) {
    if (count <= 0 || step == 0) return -1;
    if (current < 0 || current >= count) return step > 0 ? 0 : count - 1;
    if (order == PlayOrder::Shuffle) {
        if (count == 1) return -1;
        const int pick = static_cast<int>(random % static_cast<unsigned>(count - 1));
        return pick >= current ? pick + 1 : pick;
    }
    const int next = current + (step > 0 ? 1 : -1);
    if (next >= 0 && next < count) return next;
    if (order != PlayOrder::RepeatList) return -1;
    return (next + count) % count;
}

// The set the list goes on to when `current` cannot play and is skipped
// whole: the next `step`'s way as videoSetStep goes, but none already
// tried (`tried`, by index), and under Shuffle any not tried. -1 for none.
inline int videoSetSkipTo(PlayOrder order, int current, int count, int step, const std::vector<bool>& tried,
                          unsigned random) {
    if (count <= 0 || step == 0) return -1;
    const auto untried = [&](int index) {
        return static_cast<std::size_t>(index) >= tried.size() || !tried[static_cast<std::size_t>(index)];
    };
    if (order == PlayOrder::Shuffle) {
        std::vector<int> left;
        for (int index = 0; index < count; ++index) {
            if (untried(index)) left.push_back(index);
        }
        return left.empty() ? -1 : left[random % left.size()];
    }
    int at = current;
    for (int tries = 0; tries < count; ++tries) {
        at = videoSetStep(order, at, count, step, random);
        if (at < 0) return -1;
        if (untried(at)) return at;
    }
    return -1;
}

// The sets of `imported` put after those of `list`, but none whose videos a
// set of the list already has in the same places, and none past the limit.
// Returns how many were added.
inline std::size_t appendVideoSets(VideoSetList& list, const VideoSetList& imported) {
    std::size_t added = 0;
    for (const auto& set : imported.sets) {
        if (list.sets.size() >= kVideoSetLimit) break;
        if (findSameVideos(list, set) >= 0) continue;
        list.sets.push_back(set);
        ++added;
    }
    return added;
}

// QSETS 1: the order, then each set -- its name, arrangement, the place
// shown larger and how many videos -- followed by a line per video. Text
// is quoted, so a name or a path may hold anything but a line break.
inline bool writeVideoSets(std::wostream& output, const VideoSetList& list) {
    output << L"QSETS " << kVideoSetsVersion << L'\n';
    output << L"order " << static_cast<int>(list.order) << L'\n';
    for (const auto& set : list.sets) {
        output << L"set " << std::quoted(set.name) << L' ' << static_cast<int>(set.layout) << L' '
               << set.expanded << L' ' << set.videos.size() << L'\n';
        for (const auto& video : set.videos) {
            output << L"video " << std::quoted(video.path) << L' ' << std::quoted(video.title) << L' '
                   << std::quoted(video.serverName) << L' ' << std::quoted(video.serverPath) << L' '
                   << std::quoted(video.name) << L' ' << std::quoted(video.seriesName) << L' '
                   << std::quoted(video.type) << L' ' << video.season << L' ' << video.episode << L' '
                   << video.year << L'\n';
        }
    }
    return output.good();
}

// Refuses anything but a whole QSETS 1 document: another format (an old
// QDECK session among them), a newer version, a set without videos or with
// more than five, a video without a path.
inline bool readVideoSets(std::wistream& input, VideoSetList& list) {
    std::wstring key;
    int version{};
    if (!(input >> key >> version) || key != L"QSETS" || version < 1 || version > kVideoSetsVersion) {
        return false;
    }
    int order{};
    if (!(input >> key) || key != L"order" || !(input >> order)) return false;
    VideoSetList read;
    read.order = clampPlayOrder(order);
    while (input >> key) {
        if (key != L"set" || read.sets.size() >= kVideoSetLimit) return false;
        VideoSet set;
        int layout{};
        std::size_t count{};
        if (!(input >> std::quoted(set.name) >> layout >> set.expanded >> count)) return false;
        if (count == 0 || count > kMaxPanes) return false;
        set.name = cleanVideoSetName(std::move(set.name));
        set.layout = static_cast<LayoutMode>(std::clamp(layout, 0, kLayoutModeCount - 1));
        if (set.expanded < -1 || set.expanded >= static_cast<int>(count)) set.expanded = -1;
        for (std::size_t index = 0; index < count; ++index) {
            VideoSetVideo video;
            if (!(input >> key) || key != L"video" ||
                !(input >> std::quoted(video.path) >> std::quoted(video.title) >> std::quoted(video.serverName) >>
                  std::quoted(video.serverPath) >> std::quoted(video.name) >> std::quoted(video.seriesName) >>
                  std::quoted(video.type) >> video.season >> video.episode >> video.year)) {
                return false;
            }
            if (video.path.empty()) return false;
            video.season = std::max(-1, video.season);
            video.episode = std::max(-1, video.episode);
            video.year = std::clamp(video.year, 0, 9999);
            set.videos.push_back(std::move(video));
        }
        if (set.name.empty()) {
            std::vector<std::wstring> titles;
            for (const auto& video : set.videos) titles.push_back(video.title);
            set.name = videoSetDefaultName(titles);
        }
        read.sets.push_back(std::move(set));
    }
    if (!input.eof()) return false;
    list = std::move(read);
    return true;
}

}  // namespace quaddeck
