#pragma once

// Local preferences identify a library by the server's opaque identifiers,
// never by its name, address, or an item's type. CollectionType comes from
// the current server's library views and is deliberately not persisted.

#include "TextEncoding.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace quaddeck {

inline constexpr std::size_t kMaxEmbyLibraryPreferences = 256;
inline constexpr std::size_t kMaxEmbyLibraryIdBytes = 512;

struct EmbyLibraryPreference {
    std::string serverId, libraryId;
    bool detailsEnabled{};
    friend bool operator==(const EmbyLibraryPreference&, const EmbyLibraryPreference&) = default;
};

struct EmbyLibraryPrefs {
    std::vector<EmbyLibraryPreference> entries;
    friend bool operator==(const EmbyLibraryPrefs&, const EmbyLibraryPrefs&) = default;
};

inline bool validEmbyLibraryPreferenceId(std::string_view id) {
    if (id.empty() || id.size() > kMaxEmbyLibraryIdBytes) return false;
    const auto wide = utf8ToWideText(id);
    // The text helpers replace invalid sequences. An identity must survive
    // conversion exactly, rather than silently becoming a different key.
    if (wideToUtf8Text(wide) != id) return false;
    for (const wchar_t character : wide) {
        if (character <= 0x1f || (character >= 0x7f && character <= 0x9f) ||
            character == 0x2028 || character == 0x2029) return false;
    }
    return true;
}

inline bool validateEmbyLibraryPrefs(const EmbyLibraryPrefs& prefs) {
    if (prefs.entries.size() > kMaxEmbyLibraryPreferences) return false;
    for (std::size_t index = 0; index < prefs.entries.size(); ++index) {
        const auto& entry = prefs.entries[index];
        if (!validEmbyLibraryPreferenceId(entry.serverId) || !validEmbyLibraryPreferenceId(entry.libraryId)) {
            return false;
        }
        for (std::size_t before = 0; before < index; ++before) {
            if (prefs.entries[before].serverId == entry.serverId && prefs.entries[before].libraryId == entry.libraryId) {
                return false;
            }
        }
    }
    return true;
}

inline bool embyLibraryDetailsEnabled(const EmbyLibraryPrefs& prefs, std::string_view serverId,
                                      std::string_view libraryId, std::string_view collectionType) {
    if ((collectionType != "movies" && collectionType != "tvshows") ||
        !validEmbyLibraryPreferenceId(serverId) || !validEmbyLibraryPreferenceId(libraryId) ||
        prefs.entries.size() > kMaxEmbyLibraryPreferences) return false;
    const EmbyLibraryPreference* found = nullptr;
    for (const auto& entry : prefs.entries) {
        if (entry.serverId != serverId || entry.libraryId != libraryId) continue;
        if (found) return false;  // ambiguous in-memory data cannot enable a page
        found = &entry;
    }
    return found ? found->detailsEnabled : true;
}

inline bool setEmbyLibraryDetailsEnabled(EmbyLibraryPrefs& prefs, std::string_view serverId,
                                         std::string_view libraryId, bool enabled) {
    if (!validEmbyLibraryPreferenceId(serverId) || !validEmbyLibraryPreferenceId(libraryId) ||
        !validateEmbyLibraryPrefs(prefs)) return false;
    for (auto& entry : prefs.entries) {
        if (entry.serverId == serverId && entry.libraryId == libraryId) {
            entry.detailsEnabled = enabled;
            return true;
        }
    }
    if (prefs.entries.size() == kMaxEmbyLibraryPreferences) return false;
    prefs.entries.push_back({std::string(serverId), std::string(libraryId), enabled});
    return true;
}

}  // namespace quaddeck
