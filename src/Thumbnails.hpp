#pragma once

// The encoded bytes of the pictures the Emby browser shows, keyed by the
// image key EmbyApi builds (item, tag, width). App fills it from HTTP
// replies on the window thread; the Overlay decodes what it draws into
// Direct2D bitmaps of its own, which a lost device throws away while these
// bytes stay. A failed fetch is kept as an empty entry so it is not asked
// for again every frame.

#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>

namespace quaddeck {

class ThumbnailCache {
public:
    static constexpr std::size_t kMaximumEntries = 600;
    static constexpr std::size_t kMaximumBytes = 32u * 1024u * 1024u;

    std::shared_ptr<const std::string> find(const std::string& key) const {
        const auto it = entries_.find(key);
        return it == entries_.end() ? nullptr : it->second;
    }
    bool contains(const std::string& key) const { return entries_.count(key) != 0; }
    std::size_t size() const { return entries_.size(); }
    std::size_t bytes() const { return bytes_; }

    void insert(const std::string& key, std::string data) {
        auto entry = std::make_shared<const std::string>(std::move(data));
        if (const auto existing = entries_.find(key); existing != entries_.end()) {
            bytes_ -= existing->second->size();
            existing->second = entry;
        } else {
            entries_.emplace(key, entry);
        }
        bytes_ += entry->size();
        order_.emplace_back(key, entry);
        while (!order_.empty() && (entries_.size() > kMaximumEntries || bytes_ > kMaximumBytes)) {
            const auto& [oldKey, oldEntry] = order_.front();
            const auto it = entries_.find(oldKey);
            // A key re-inserted since keeps its newer entry; only the one
            // this record made goes.
            if (it != entries_.end() && it->second == oldEntry) {
                bytes_ -= it->second->size();
                entries_.erase(it);
            }
            order_.pop_front();
        }
    }

    void clear() {
        entries_.clear();
        order_.clear();
        bytes_ = 0;
    }

    // Forgets the fetches that failed, so a picture the server could not
    // give a moment ago is asked for once more. Returns how many went.
    std::size_t forgetFailures() {
        std::size_t forgotten = 0;
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->second->empty()) {
                it = entries_.erase(it);
                ++forgotten;
            } else {
                ++it;
            }
        }
        return forgotten;
    }

private:
    std::unordered_map<std::string, std::shared_ptr<const std::string>> entries_;
    std::deque<std::pair<std::string, std::shared_ptr<const std::string>>> order_;
    std::size_t bytes_{};
};

}  // namespace quaddeck
