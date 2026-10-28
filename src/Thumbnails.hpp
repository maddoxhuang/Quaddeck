#pragma once

// The encoded bytes of the pictures the Emby browser shows, keyed by the
// image key EmbyApi builds (item, tag, width). App fills it from HTTP
// replies on the window thread; the Overlay decodes what it draws into
// Direct2D bitmaps of its own, which a lost device throws away while these
// bytes stay. A failed fetch is kept as an empty entry so it is not asked
// for again every frame. Beside the pictures it keeps the shape of the
// videos they were taken from, which the Overlay draws them in.

#include <cmath>
#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace quaddeck {

// The shape of the video behind a picture, each as width over height: its
// frame as stored, and as it is meant to be shown (0 where unknown). They
// differ for a video whose pixels are not square: a 9:16 phone clip stored
// as 1280x720 with a pixel aspect of 81:256.
struct VideoShape {
    float stored{};
    float display{};
};

// Two shapes within this of each other are taken as one: Emby rounds a
// display aspect within 3 % of 16:9 to "16:9".
inline constexpr float kVideoShapeTolerance = 0.03F;

// The width over height a picture of `width` x `height` is drawn at. A
// server that takes a video's picture from its frame as stored -- Emby does,
// whatever the video's pixel aspect -- gives a non-square-pixel video a
// picture of the stored shape, which drawn as it is comes out stretched
// (that phone clip filling a 16:9 tile). Such a picture is drawn in the
// video's display shape; any other, a poster or a picture already
// corrected, in its own.
inline float pictureDrawAspect(float width, float height, const VideoShape& shape) {
    if (!(width > 0.0F) || !(height > 0.0F)) return 1.0F;
    const float own = width / height;
    // Not `near`: windef.h makes that word nothing.
    const auto alike = [](float a, float b) { return std::abs(a / b - 1.0F) <= kVideoShapeTolerance; };
    if (!(shape.stored > 0.0F) || !(shape.display > 0.0F) || !std::isfinite(shape.stored) ||
        !std::isfinite(shape.display)) {
        return own;
    }
    return alike(own, shape.stored) && !alike(shape.display, shape.stored) ? shape.display : own;
}

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
        shapes_.clear();
    }

    // What an item's video looks like, as the server answered: kept apart
    // from the pictures, for every key naming the item whatever its width.
    // An answer with no video in it is kept too, so it is asked once.
    void setVideoShape(const std::string& itemId, VideoShape shape) {
        if (itemId.empty()) return;
        if (shapes_.size() >= kMaximumShapes && !shapes_.contains(itemId)) shapes_.clear();
        shapes_[itemId] = shape;
    }
    bool knowsVideoShape(const std::string& itemId) const { return shapes_.contains(itemId); }
    std::size_t videoShapes() const { return shapes_.size(); }

    // The width over height the picture under `key` is drawn at, given its
    // own size: pictureDrawAspect with the shape of the item the key names.
    float drawAspect(const std::string& key, float width, float height) const {
        const auto shape = shapes_.find(std::string(keyItem(key)));
        return pictureDrawAspect(width, height, shape == shapes_.end() ? VideoShape{} : shape->second);
    }

    // The item an Emby item picture's key names (`item|tag|width`, as
    // emby::imageKey builds it); empty for a local file's (`file|...`), a
    // typed picture's (`emby-image|...`) or anything else.
    static std::string_view keyItem(std::string_view key) {
        const auto first = key.find('|');
        if (first == std::string_view::npos || first == 0) return {};
        const auto second = key.find('|', first + 1);
        if (second == std::string_view::npos || key.find('|', second + 1) != std::string_view::npos) return {};
        const auto item = key.substr(0, first);
        return item == "file" || item == "emby-image" ? std::string_view() : item;
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
    // Far more than a session's browsing asks about; past it they are all
    // forgotten and asked for again as they show.
    static constexpr std::size_t kMaximumShapes = 20000;

    std::unordered_map<std::string, std::shared_ptr<const std::string>> entries_;
    std::unordered_map<std::string, VideoShape> shapes_;
    std::deque<std::pair<std::string, std::shared_ptr<const std::string>>> order_;
    std::size_t bytes_{};
};

}  // namespace quaddeck
