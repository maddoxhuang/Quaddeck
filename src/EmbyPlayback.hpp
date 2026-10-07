#pragma once

// Each pane owns its sequence. Browser replies may extend that sequence,
// while opening or sorting another page leaves it alone.

#include "EmbyApi.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace quaddeck::emby {

struct PlaybackQueue {
    std::vector<Item> items;
    int cursor{-1};
    std::string page;
};

inline PlaybackQueue takeQueue(const std::vector<Item>& list, int chosen, const std::string& page) {
    PlaybackQueue queue;
    queue.page = page;
    queue.items.reserve(list.size());
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (!isPlayableVideo(list[i]) && !isPhoto(list[i])) continue;
        if (chosen >= 0 && i == static_cast<std::size_t>(chosen)) queue.cursor = static_cast<int>(queue.items.size());
        queue.items.push_back(list[i]);
    }
    return queue;
}

inline int queueIndex(const PlaybackQueue& queue, const std::string& itemId) {
    if (itemId.empty()) return -1;
    if (queue.cursor >= 0 && static_cast<std::size_t>(queue.cursor) < queue.items.size() &&
        queue.items[queue.cursor].id == itemId) {
        return queue.cursor;
    }
    return entryIndex(queue.items, itemId);
}

namespace playback_detail {

inline bool sameEntry(const Item& a, const Item& b) {
    return a.id == b.id && a.playlistItemId == b.playlistItemId;
}

inline int occurrenceAt(const std::vector<Item>& items, int index) {
    int occurrence = 0;
    for (int i = 0; i < index; ++i) {
        if (sameEntry(items[i], items[index])) ++occurrence;
    }
    return occurrence;
}

inline int findEntry(const std::vector<Item>& items, const Item& entry, int occurrence) {
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (sameEntry(items[i], entry) && occurrence-- == 0) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace playback_detail

template<class Order>
inline void reorderQueue(PlaybackQueue& queue, Order&& order) {
    const bool playing = queue.cursor >= 0 && static_cast<std::size_t>(queue.cursor) < queue.items.size();
    const Item entry = playing ? queue.items[queue.cursor] : Item{};
    const int occurrence = playing ? playback_detail::occurrenceAt(queue.items, queue.cursor) : 0;
    std::forward<Order>(order)(queue.items);
    queue.cursor = playing ? playback_detail::findEntry(queue.items, entry, occurrence) : -1;
}

// A shorter or differently ordered reply must not change what Next means.
// Explicit sorting can opt into reordering, still on the same page and
// retaining every old entry (including repeated playlist entries).
inline bool adoptQueue(PlaybackQueue& queue, const std::vector<Item>& list, const std::string& itemId,
                       const std::string& page, bool allowReorder = false) {
    if (itemId.empty() || (!queue.items.empty() && queue.page != page)) return false;
    PlaybackQueue fresh = takeQueue(list, -1, page);
    if (fresh.items.size() < queue.items.size()) return false;

    const int playing = queueIndex(queue, itemId);
    if (playing >= 0) {
        fresh.cursor = playback_detail::findEntry(fresh.items, queue.items[playing],
                                                  playback_detail::occurrenceAt(queue.items, playing));
    } else {
        fresh.cursor = entryIndex(fresh.items, itemId);
    }
    if (fresh.cursor < 0) return false;

    if (allowReorder) {
        using Key = std::pair<std::string, std::string>;
        std::vector<Key> heldKeys, freshKeys;
        heldKeys.reserve(queue.items.size());
        freshKeys.reserve(fresh.items.size());
        for (const auto& item : queue.items) heldKeys.emplace_back(item.id, item.playlistItemId);
        for (const auto& item : fresh.items) freshKeys.emplace_back(item.id, item.playlistItemId);
        std::sort(heldKeys.begin(), heldKeys.end());
        std::sort(freshKeys.begin(), freshKeys.end());
        if (!std::includes(freshKeys.begin(), freshKeys.end(), heldKeys.begin(), heldKeys.end())) return false;
    } else {
        std::size_t kept = 0;
        for (const auto& item : fresh.items) {
            if (kept < queue.items.size() && playback_detail::sameEntry(queue.items[kept], item)) ++kept;
        }
        if (kept != queue.items.size()) return false;
    }
    queue = std::move(fresh);
    return true;
}

}  // namespace quaddeck::emby
