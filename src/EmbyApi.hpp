#pragma once

// Emby's REST protocol, pure: request paths, the JSON shapes the server
// answers with, playback-report bodies, the emby:// locator that sessions
// store in place of a file path, and the signed-in state file. Nothing here
// touches the network, the window or the clock, so the Emby test binary
// exercises every line on replies a real Emby 4.11 server gave. EmbyClient
// does the HTTP; App decides what to ask for and when.

#include "TextEncoding.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <istream>
#include <limits>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace quaddeck::emby {

// Emby measures positions and durations in 100 ns ticks.
inline constexpr std::int64_t kTicksPerSecond = 10'000'000;
inline constexpr const char* kClientName = "QuadDeck";
inline constexpr const char* kClientVersion = "1.0.0";
inline constexpr const char* kDeviceName = "Windows";
// Extra DTO fields shared by browser, hierarchy and resume requests.
// ProductionYear and episode numbers are returned as part of BaseItemDto.
inline constexpr const char* kBrowserItemFields = "ParentId,DateCreated,PrimaryImageAspectRatio,Size,ChildCount";
inline constexpr std::size_t kMaximumItemReplyBytes = 8u * 1024u * 1024u;
inline constexpr std::size_t kMaximumOverviewBytes = 64u * 1024u;
inline constexpr std::size_t kMaximumMetadataEntries = 128;
inline constexpr std::size_t kMaximumBackdropImages = 32;

inline std::int64_t ticksFromSeconds(double seconds) {
    if (!(seconds > 0.0)) return 0;
    return static_cast<std::int64_t>(std::llround(seconds * static_cast<double>(kTicksPerSecond)));
}

inline double secondsFromTicks(std::int64_t ticks) {
    return ticks > 0 ? static_cast<double>(ticks) / static_cast<double>(kTicksPerSecond) : 0.0;
}

// Who we are to the server. Everything a request needs besides its path.
struct Session {
    std::string serverUrl;  // scheme://host[:port][/prefix], no trailing slash
    std::string serverId;
    std::string serverName;
    std::string userId;
    std::string userName;
    std::string token;
    std::string deviceId;
    bool signedIn() const { return !token.empty() && !userId.empty() && !serverUrl.empty(); }
};

struct ServerInfo {
    std::string name, version, id;
};

struct Authentication {
    std::string token, userId, userName, serverId;
};

struct Person {
    std::string id, name, type, role, primaryImageTag;
};

// One library entry: a view, folder, series, season, episode, movie, home
// video or photo. Emby sends the same BaseItemDto for all of them.
struct Item {
    std::string id, name, type, collectionType, container, mediaType;
    std::string seriesId, seriesName, seasonId, parentId;
    int indexNumber{-1};
    int parentIndexNumber{-1};
    int indexNumberEnd{-1};
    int productionYear{};
    bool isFolder{};
    std::int64_t runTimeTicks{};
    std::int64_t positionTicks{};
    bool played{};
    int unplayedCount{-1};
    int width{};
    int height{};
    std::string dateCreated, premiereDate, primaryImageTag;
    std::int64_t size{};  // bytes, with Fields=Size; 0 for a folder
    // An entry of a playlist: its place in it, which tells two entries of
    // the same video apart. Empty anywhere else.
    std::string playlistItemId;
    // How many items a playlist or folder holds, with Fields=ChildCount.
    int childCount{-1};
    // A folder without a picture of its own borrows a child's.
    std::string primaryImageItemId;
    double primaryImageAspect{};
    // Optional information for a detail page; list replies often omit it.
    std::string overview, officialRating, originalTitle, videoInfo, audioInfo;
    std::optional<double> communityRating, criticRating;
    std::vector<std::string> genres, taglines, studios;
    std::vector<Person> people;
    // Backdrop slots retain their server index, including an invalid/empty
    // slot. A parent supplies artwork only when there is no own artwork.
    std::vector<std::string> backdropImageTags, parentBackdropImageTags;
    std::string parentBackdropItemId, logoImageTag, parentLogoItemId, parentLogoImageTag;
};

// The orders the browser offers, as the server names them. Updated is
// what the server took in or changed last: a server told to keep a file's
// own date as its DateCreated -- this one is -- lists a video it has only
// just found among the files of that date, years back, so Added does not
// bring what is new to the front. New keys go at the end; the index is
// what settings.qconfig holds.
enum class SortKey { Name, DateAdded, Release, Runtime, Random, LastPlayed, Size, Updated };
inline constexpr int kSortKeyCount = 8;

inline SortKey sortKeyFromIndex(int index) {
    return static_cast<SortKey>(std::clamp(index, 0, kSortKeyCount - 1));
}

inline const wchar_t* sortKeyName(SortKey key) {
    switch (key) {
    case SortKey::DateAdded: return L"Added";
    case SortKey::Release: return L"Released";
    case SortKey::Runtime: return L"Length";
    case SortKey::Random: return L"Random";
    case SortKey::LastPlayed: return L"Played";
    case SortKey::Size: return L"Size";
    case SortKey::Updated: return L"Updated";
    default: return L"Name";
    }
}

// The server's names (an Emby 4.11 answered 200 to each; "FileSize" and
// "DateAdded" are a 500). DateLastSaved orders by when the server last
// wrote the item, alone and behind IsFolder; the date itself is not in
// its replies.
inline const char* sortByValue(SortKey key) {
    switch (key) {
    case SortKey::Updated: return "DateLastSaved";
    case SortKey::DateAdded: return "DateCreated";
    case SortKey::Release: return "PremiereDate";
    case SortKey::Runtime: return "Runtime";
    case SortKey::Random: return "Random";
    case SortKey::LastPlayed: return "DatePlayed";
    case SortKey::Size: return "Size";
    default: return "SortName";
    }
}

// Newest, longest, latest and largest first read naturally; names A to Z.
inline bool sortDescendsByDefault(SortKey key) {
    return key == SortKey::DateAdded || key == SortKey::Release || key == SortKey::Runtime ||
           key == SortKey::LastPlayed || key == SortKey::Size || key == SortKey::Updated;
}

// The number the server gave an item, -1 for an id that is not one. The
// server counts up, so among two items the higher was taken in later.
inline long long itemNumber(const std::string& id) {
    if (id.empty() || id.size() > 18) return -1;
    long long value = 0;
    for (const char c : id) {
        if (c < '0' || c > '9') return -1;
        value = value * 10 + (c - '0');
    }
    return value;
}

struct MediaStream {
    int index{-1};
    std::string type, codec, language, displayTitle;
    // The stream's own title; for a subtitle file beside the video, what
    // its name adds to the video's ("chs", "unibig5"), which is often all
    // that says its language.
    std::string title;
    bool isExternal{}, isDefault{}, isForced{}, isTextSubtitle{};
    int channels{};
};

struct MediaSource {
    std::string id, container, protocol, transcodingUrl;
    bool supportsDirectPlay{}, supportsDirectStream{};
    std::int64_t runTimeTicks{};
    int defaultAudioStreamIndex{-1};
    int defaultSubtitleStreamIndex{-1};
    std::vector<MediaStream> streams;
};

struct PlaybackInfo {
    std::string playSessionId, errorCode;
    std::vector<MediaSource> sources;
};

struct ItemList {
    std::vector<Item> items;
    int total{};
};

// --- classification --------------------------------------------------------

inline bool isPlayableVideo(const Item& item) {
    if (item.isFolder) return false;
    return item.type == "Movie" || item.type == "Episode" || item.type == "Video" ||
           item.type == "MusicVideo" || item.type == "Trailer" || item.mediaType == "Video";
}

inline bool isPhoto(const Item& item) {
    return !item.isFolder && (item.type == "Photo" || item.mediaType == "Photo");
}

inline bool isPlaylist(const Item& item) { return item.type == "Playlist"; }

inline bool isBrowsable(const Item& item) {
    if (item.isFolder) return true;
    return item.type == "CollectionFolder" || item.type == "UserView" || item.type == "Series" ||
           item.type == "Season" || item.type == "Folder" || item.type == "PhotoAlbum" ||
           item.type == "BoxSet" || item.type == "Playlist" || item.type == "Channel";
}

// The subtitle codecs the server can hand over as text, as it names them.
// Bitmap subtitles (PGS, VobSub) are not; they would need burning in or a
// bitmap renderer.
inline constexpr std::array<const char*, 12> kTextSubtitleCodecs{
    "subrip", "srt", "ass", "ssa", "webvtt", "vtt", "mov_text", "text", "tx3g", "ttml", "microdvd", "sami"};

inline bool isTextSubtitleCodec(std::string codec) {
    std::transform(codec.begin(), codec.end(), codec.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::any_of(kTextSubtitleCodecs.begin(), kTextSubtitleCodecs.end(),
                       [&](const char* text) { return codec == text; });
}

// A subtitle stream whose own format is ASS or its predecessor; the server
// hands such a stream over unconverted at Stream.ass (a 4.11 did for one
// beside the video and for one inside the container alike).
inline bool isAssSubtitleCodec(std::string codec) {
    std::transform(codec.begin(), codec.end(), codec.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return codec == "ass" || codec == "ssa";
}

// --- strings ---------------------------------------------------------------

inline std::string urlEncode(std::string_view value) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(value.size() * 3);
    for (const char c : value) {
        const auto byte = static_cast<unsigned char>(c);
        const bool unreserved = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                                (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                                byte == '.' || byte == '~';
        if (unreserved) {
            out.push_back(c);
        } else {
            out.push_back('%');
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 0x0F]);
        }
    }
    return out;
}

// "192.168.1.2:8096" -> "http://192.168.1.2:8096"; trailing slashes go.
inline std::string normalizeServerUrl(std::string url) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    url.erase(url.begin(), std::find_if(url.begin(), url.end(), notSpace));
    url.erase(std::find_if(url.rbegin(), url.rend(), notSpace).base(), url.end());
    if (url.empty()) return url;
    std::string lower = url;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower.rfind("http://", 0) != 0 && lower.rfind("https://", 0) != 0) url = "http://" + url;
    // Credentials pasted into the address would be logged and stored; the
    // sign-in dialog is where they go.
    const auto authority = url.find("://") + 3;
    const auto authorityEnd = url.find('/', authority);
    const auto at = url.find('@', authority);
    if (at != std::string::npos && (authorityEnd == std::string::npos || at < authorityEnd)) {
        url.erase(authority, at + 1 - authority);
    }
    while (url.size() > 8 && url.back() == '/') url.pop_back();
    return url;
}

// The X-Emby-Authorization value every request carries.
inline std::string authorizationHeader(const std::string& deviceId) {
    return std::string("MediaBrowser Client=\"") + kClientName + "\", Device=\"" + kDeviceName +
           "\", DeviceId=\"" + deviceId + "\", Version=\"" + kClientVersion + "\"";
}

// --- request paths (relative to the server URL) ----------------------------

inline std::string publicInfoPath() { return "/System/Info/Public"; }
inline std::string authenticatePath() { return "/Users/AuthenticateByName"; }
inline std::string logoutPath() { return "/Sessions/Logout"; }

inline std::string authenticateBody(const std::string& userName, const std::string& password) {
    nlohmann::json body;
    body["Username"] = userName;
    body["Pw"] = password;
    return body.dump();
}

inline std::string viewsPath(const std::string& userId) {
    return "/Users/" + urlEncode(userId) + "/Views";
}

struct ItemsQuery {
    std::string parentId;
    std::string includeTypes;  // "Series", "Movie,Video", ...
    std::string sortBy{"SortName"};
    std::string sortOrder{"Ascending"};
    std::string nameStartsWith;
    std::string searchTerm;
    std::string fields;
    std::string filters;       // "IsUnplayed"
    bool recursive{};
    int startIndex{};
    int limit{300};
};

// Folders first, then the chosen order; the server takes one order per
// sort field as a comma list. Random cannot be combined and never has
// folders first. An unknown SortBy is a 500 from the server, so only the
// names sortByValue knows are ever sent.
inline void applySort(ItemsQuery& query, SortKey key, bool descending, bool foldersFirst) {
    const char* order = descending ? "Descending" : "Ascending";
    if (key == SortKey::Random) {
        query.sortBy = "Random";
        query.sortOrder.clear();
        return;
    }
    if (foldersFirst) {
        query.sortBy = std::string("IsFolder,") + sortByValue(key);
        query.sortOrder = std::string("Ascending,") + order;
    } else {
        query.sortBy = sortByValue(key);
        query.sortOrder = order;
    }
}

inline std::string itemsPath(const std::string& userId, const ItemsQuery& query) {
    std::string path = "/Users/" + urlEncode(userId) + "/Items?";
    std::vector<std::string> params;
    if (!query.parentId.empty()) params.push_back("ParentId=" + urlEncode(query.parentId));
    if (!query.includeTypes.empty()) params.push_back("IncludeItemTypes=" + urlEncode(query.includeTypes));
    if (query.recursive) params.push_back("Recursive=true");
    if (!query.sortBy.empty()) params.push_back("SortBy=" + urlEncode(query.sortBy));
    if (!query.sortOrder.empty()) params.push_back("SortOrder=" + urlEncode(query.sortOrder));
    if (!query.nameStartsWith.empty()) params.push_back("NameStartsWith=" + urlEncode(query.nameStartsWith));
    if (!query.searchTerm.empty()) params.push_back("SearchTerm=" + urlEncode(query.searchTerm));
    if (!query.filters.empty()) params.push_back("Filters=" + urlEncode(query.filters));
    if (!query.fields.empty()) params.push_back("Fields=" + urlEncode(query.fields));
    params.push_back("StartIndex=" + std::to_string(std::max(0, query.startIndex)));
    params.push_back("Limit=" + std::to_string(std::max(1, query.limit)));
    for (std::size_t i = 0; i < params.size(); ++i) {
        if (i > 0) path += '&';
        path += params[i];
    }
    return path;
}

// --- what a browser page asks for --------------------------------------------

// The pages listed with /Users/{id}/Items: a library from the home page, a
// folder inside one, and a playlist. A 4.11 server lists a playlist's
// entries under ParentId like a folder's children, in the playlist's own
// order when no SortBy is sent, and honours SortBy and Filters there --
// /Playlists/{id}/Items gives the same order but ignores Filters.
enum class ListKind { Library, Folder, Playlist };

struct ListPage {
    ListKind kind{ListKind::Folder};
    std::string id;              // the ParentId
    std::string collectionType;  // a library's: tvshows, movies, homevideos, boxsets, playlists, ...
    std::string itemType;        // what was opened: Folder, BoxSet, PhotoAlbum, ...
    // A playlist in the order it was put together rather than the browser's.
    bool ownOrder{true};
};

struct ListArrangement {
    SortKey sort{SortKey::Name};
    bool descending{};
    bool unplayed{};
    bool flat{};  // every video under the page, not its folders
};

// The one type a library lists recursively whatever its folders are, or
// nothing for a library that is walked.
inline const char* libraryItemType(const ListPage& page) {
    if (page.kind != ListKind::Library) return "";
    if (page.collectionType == "tvshows") return "Series";
    if (page.collectionType == "movies") return "Movie";
    return "";
}

// Whether the page can be shown without its folders. A library of series
// or movies already is; a library of box sets or of playlists lists those,
// and a box set or a playlist its own entries. Series and seasons keep
// their hierarchy instead of recursively flattening it.
inline bool listTakesFlat(const ListPage& page) {
    if (page.kind == ListKind::Playlist) return false;
    if (page.kind == ListKind::Library) {
        return std::string_view(libraryItemType(page)).empty() && page.collectionType != "boxsets" &&
               page.collectionType != "playlists";
    }
    return page.itemType != "BoxSet" && page.itemType != "Playlist" &&
           page.itemType != "Series" && page.itemType != "Season";
}

inline bool listInOwnOrder(const ListPage& page) {
    return page.kind == ListKind::Playlist && page.ownOrder;
}

inline ItemsQuery listQuery(const ListPage& page, const ListArrangement& arrangement, int startIndex,
                            int limit, const std::string& fields) {
    ItemsQuery query;
    query.parentId = page.id;
    query.startIndex = startIndex;
    query.limit = limit;
    query.fields = fields;
    if (arrangement.unplayed) query.filters = "IsUnplayed";
    const std::string type = libraryItemType(page);
    if (page.kind == ListKind::Playlist) {
        if (page.ownOrder) {
            // No SortBy at all: the server's answer is the playlist's order.
            query.sortBy.clear();
            query.sortOrder.clear();
        } else {
            applySort(query, arrangement.sort, arrangement.descending, false);
        }
    } else if (!type.empty()) {
        query.includeTypes = type;
        query.recursive = true;
        applySort(query, arrangement.sort, arrangement.descending, false);
    } else if (arrangement.flat && listTakesFlat(page)) {
        query.includeTypes = "Video,Photo,Movie,Episode";
        query.recursive = true;
        applySort(query, arrangement.sort, arrangement.descending, false);
    } else {
        // Folders first, then the chosen order, as the server's own client
        // shows a home-video or photo library.
        applySort(query, arrangement.sort, arrangement.descending, true);
    }
    return query;
}

// Both hierarchy endpoints support Fields and page bounds. Omit bounds by
// default so the neighbour walk can still ask for the whole series.
inline std::string seasonsPath(const std::string& seriesId, const std::string& userId,
                               const std::string& fields = kBrowserItemFields, int startIndex = 0,
                               int limit = 0) {
    std::string path = "/Shows/" + urlEncode(seriesId) + "/Seasons?UserId=" + urlEncode(userId);
    if (!fields.empty()) path += "&Fields=" + urlEncode(fields);
    if (limit > 0) {
        path += "&StartIndex=" + std::to_string(std::max(0, startIndex));
        path += "&Limit=" + std::to_string(limit);
    }
    return path;
}

inline std::string episodesPath(const std::string& seriesId, const std::string& userId,
                                const std::string& seasonId = {}, const std::string& fields = kBrowserItemFields,
                                int startIndex = 0, int limit = 0) {
    std::string path = "/Shows/" + urlEncode(seriesId) + "/Episodes?UserId=" + urlEncode(userId);
    if (!seasonId.empty()) path += "&SeasonId=" + urlEncode(seasonId);
    if (!fields.empty()) path += "&Fields=" + urlEncode(fields);
    if (limit > 0) {
        path += "&StartIndex=" + std::to_string(std::max(0, startIndex));
        path += "&Limit=" + std::to_string(limit);
    }
    return path;
}

inline std::string resumePath(const std::string& userId, int limit,
                              const std::string& fields = kBrowserItemFields) {
    std::string path = "/Users/" + urlEncode(userId) + "/Items/Resume?MediaTypes=Video&Limit=" +
                       std::to_string(std::max(1, limit));
    if (!fields.empty()) path += "&Fields=" + urlEncode(fields);
    return path;
}

// ParentId, which the neighbour walk needs, only comes when asked for.
inline std::string itemPath(const std::string& userId, const std::string& itemId) {
    return "/Users/" + urlEncode(userId) + "/Items/" + urlEncode(itemId) +
           "?Fields=" + urlEncode(kBrowserItemFields);
}

// --- which videos of a list have subtitles -------------------------------------

// A list's items do not say whether they have subtitles: a 4.11 server
// sends no HasSubtitles in them, and MediaStreams runs to megabytes for a
// video with a hundred subtitle files beside it. Asked for by id, the
// server answers with those that have a stream of a text codec
// (SubtitleCodecs filters by each stream's codec; HasSubtitles=true would
// count a video with only PGS, which the player cannot show).
inline std::string subtitledItemsPath(const std::string& userId, const std::vector<std::string>& ids) {
    std::string list;
    for (const auto& id : ids) list += (list.empty() ? "" : ",") + id;
    std::string codecs;
    for (const char* codec : kTextSubtitleCodecs) codecs += (codecs.empty() ? "" : ",") + std::string(codec);
    return "/Users/" + urlEncode(userId) + "/Items?Ids=" + urlEncode(list) + "&SubtitleCodecs=" +
           urlEncode(codecs) + "&EnableImages=false&EnableUserData=false&Limit=" +
           std::to_string(std::max<std::size_t>(1, ids.size()));
}

// The videos of a list whose subtitles are to be asked for: each playable
// video once, none whose question is still out, and none already answered
// unless the list is being looked at anew.
inline std::vector<std::string> subtitleQuestions(const std::vector<Item>& items,
                                                  const std::unordered_map<std::string, bool>& known,
                                                  const std::unordered_set<std::string>& asking, bool again) {
    std::vector<std::string> ids;
    std::unordered_set<std::string> listed;
    for (const auto& item : items) {
        if (!isPlayableVideo(item) || item.id.empty() || asking.contains(item.id)) continue;
        if (!again && known.contains(item.id)) continue;
        if (listed.insert(item.id).second) ids.push_back(item.id);
    }
    return ids;
}

// Ids in batches of at most `count`, and of at most `characters` once
// joined, so a request stays short whatever a server's ids look like.
inline std::vector<std::vector<std::string>> subtitleBatches(const std::vector<std::string>& ids,
                                                             std::size_t count = 100,
                                                             std::size_t characters = 1500) {
    std::vector<std::vector<std::string>> batches;
    std::size_t length = 0;
    for (const auto& id : ids) {
        if (batches.empty() || batches.back().size() >= count ||
            (!batches.back().empty() && length + 1 + id.size() > characters)) {
            batches.emplace_back();
            length = 0;
        }
        length += (batches.back().empty() ? 0 : 1) + id.size();
        batches.back().push_back(id);
    }
    return batches;
}

inline std::string itemDetailsPath(const std::string& userId, const std::string& itemId) {
    // The documented single-item endpoint returns BaseItemDto. Its contract
    // does not declare a Fields query; consume whichever details it returns.
    return "/Users/" + urlEncode(userId) + "/Items/" + urlEncode(itemId);
}

inline std::string ancestorsPath(const std::string& itemId, const std::string& userId) {
    return "/Items/" + urlEncode(itemId) + "/Ancestors?UserId=" + urlEncode(userId);
}

inline std::string playbackInfoPath(const std::string& itemId, const std::string& userId) {
    return "/Items/" + urlEncode(itemId) + "/PlaybackInfo?UserId=" + urlEncode(userId);
}

inline std::string playbackInfoBody(const std::string& userId) {
    nlohmann::json body;
    body["UserId"] = userId;
    return body.dump();
}

// The file itself, byte for byte, which FFmpeg demuxes like a local file.
inline std::string streamPath(const std::string& itemId, const std::string& mediaSourceId,
                              const std::string& playSessionId) {
    std::string path = "/Videos/" + urlEncode(itemId) + "/stream?MediaSourceId=" +
                       urlEncode(mediaSourceId) + "&Static=true";
    if (!playSessionId.empty()) path += "&PlaySessionId=" + urlEncode(playSessionId);
    return path;
}

// A photo is served as its own primary image; maxWidth bounds the JPEG the
// server renders so a 50-megapixel original does not cross the network, and
// AutoOrient has the server apply the camera's orientation tag. Only ask
// for an item that has a Primary image: the server answers 500, not 404,
// for one that does not.
inline std::string imagePath(const std::string& itemId, int maxWidth, const std::string& tag = {}) {
    std::string path = "/Items/" + urlEncode(itemId) + "/Images/Primary?quality=90&AutoOrient=true";
    if (maxWidth > 0) path += "&maxWidth=" + std::to_string(maxWidth);
    if (!tag.empty()) path += "&tag=" + urlEncode(tag);
    return path;
}

enum class ImageType { Primary, Backdrop, Logo };

inline const char* imageTypeName(ImageType type) {
    switch (type) {
    case ImageType::Backdrop: return "Backdrop";
    case ImageType::Logo: return "Logo";
    default: return "Primary";
    }
}

inline std::string imagePath(const std::string& itemId, int maxWidth, const std::string& tag,
                             ImageType type, int index = 0) {
    if (type == ImageType::Primary) return imagePath(itemId, maxWidth, tag);
    std::string path = "/Items/" + urlEncode(itemId) + "/Images/" + imageTypeName(type) + "/" +
                       std::to_string(std::max(0, index)) + "?quality=90&AutoOrient=true";
    if (maxWidth > 0) path += "&maxWidth=" + std::to_string(maxWidth);
    if (!tag.empty()) path += "&tag=" + urlEncode(tag);
    return path;
}

// The original file, gated by the user's download permission.
inline std::string downloadPath(const std::string& itemId) {
    return "/Items/" + urlEncode(itemId) + "/Download";
}

// The picture a browser tile shows for an item, and the key it is cached
// under: item (or the child a folder borrows from), the tag that changes
// when the picture does, and the width asked for. Empty when the item has
// no picture -- the server answers 500, not 404, when asked anyway.
inline std::string imageKey(const Item& item, int width) {
    if (item.primaryImageTag.empty()) return {};
    const std::string& source = item.primaryImageItemId.empty() ? item.id : item.primaryImageItemId;
    return source + "|" + item.primaryImageTag + "|" + std::to_string(std::max(1, width));
}

struct ImageKey {
    std::string itemId, tag;
    int width{};
    ImageType type{ImageType::Primary};
    int index{};
};

inline std::string typedImageKey(const std::string& itemId, const std::string& tag, int width,
                                 ImageType type, int index = 0) {
    if (itemId.empty() || tag.empty() || itemId.find('|') != std::string::npos ||
        tag.find('|') != std::string::npos || itemId.size() > 1024 || tag.size() > 1024 || index < 0 ||
        (type != ImageType::Backdrop && index != 0) ||
        (type == ImageType::Backdrop && index >= static_cast<int>(kMaximumBackdropImages))) return {};
    return std::string("emby-image|") + imageTypeName(type) + "|" + std::to_string(index) + "|" +
           itemId + "|" + tag + "|" + std::to_string(std::max(1, width));
}

inline std::string backdropImageKey(const Item& item, int width, int index = 0) {
    if (index < 0 || index >= static_cast<int>(kMaximumBackdropImages)) return {};
    const auto hasOwn = std::any_of(item.backdropImageTags.begin(), item.backdropImageTags.end(),
                                    [](const std::string& tag) { return !tag.empty(); });
    const auto& tags = hasOwn ? item.backdropImageTags : item.parentBackdropImageTags;
    const auto& source = hasOwn ? item.id : item.parentBackdropItemId;
    if (static_cast<std::size_t>(index) >= tags.size()) return {};
    return typedImageKey(source, tags[static_cast<std::size_t>(index)], width, ImageType::Backdrop, index);
}

inline std::string logoImageKey(const Item& item, int width) {
    const bool own = !item.logoImageTag.empty();
    return typedImageKey(own ? item.id : item.parentLogoItemId,
                         own ? item.logoImageTag : item.parentLogoImageTag, width, ImageType::Logo);
}

inline std::optional<ImageKey> parseImageKey(const std::string& key) {
    if (key.size() > 4096) return std::nullopt;
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (;;) {
        const auto end = key.find('|', start);
        parts.emplace_back(key.data() + start, (end == std::string::npos ? key.size() : end) - start);
        if (parts.size() > 6) return std::nullopt;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    const auto number = [](std::string_view text, int& value) {
        if (text.empty()) return false;
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        return result.ec == std::errc{} && result.ptr == text.data() + text.size() && value >= 0;
    };
    ImageKey parsed;
    if (parts.size() == 3) {
        parsed.itemId = parts[0];
        parsed.tag = parts[1];
        if (!number(parts[2], parsed.width)) return std::nullopt;
    } else if (parts.size() == 6 && parts[0] == "emby-image") {
        if (parts[1] == "Backdrop") parsed.type = ImageType::Backdrop;
        else if (parts[1] == "Logo") parsed.type = ImageType::Logo;
        else if (parts[1] != "Primary") return std::nullopt;
        if (!number(parts[2], parsed.index) || !number(parts[5], parsed.width)) return std::nullopt;
        if ((parsed.type != ImageType::Backdrop && parsed.index != 0) ||
            (parsed.type == ImageType::Backdrop && parsed.index >= static_cast<int>(kMaximumBackdropImages))) {
            return std::nullopt;
        }
        parsed.itemId = parts[3];
        parsed.tag = parts[4];
    } else {
        return std::nullopt;
    }
    if (parsed.itemId.empty() || parsed.tag.empty() || parsed.width <= 0) return std::nullopt;
    return parsed;
}

inline std::string imagePath(const ImageKey& key) {
    return imagePath(key.itemId, key.width, key.tag, key.type, key.index);
}

inline std::string subtitlePath(const std::string& itemId, const std::string& mediaSourceId,
                                int streamIndex, const std::string& format = "srt") {
    return "/Videos/" + urlEncode(itemId) + "/" + urlEncode(mediaSourceId) + "/Subtitles/" +
           std::to_string(streamIndex) + "/Stream." + format;
}

inline std::string playingPath() { return "/Sessions/Playing"; }
inline std::string progressPath() { return "/Sessions/Playing/Progress"; }
inline std::string stoppedPath() { return "/Sessions/Playing/Stopped"; }

struct Report {
    std::string itemId, mediaSourceId, playSessionId, eventName;
    std::int64_t positionTicks{};
    bool paused{};
};

inline std::string reportBody(const Report& report) {
    nlohmann::json body;
    body["ItemId"] = report.itemId;
    body["MediaSourceId"] = report.mediaSourceId;
    if (!report.playSessionId.empty()) body["PlaySessionId"] = report.playSessionId;
    body["PositionTicks"] = report.positionTicks;
    body["IsPaused"] = report.paused;
    body["PlayMethod"] = "DirectPlay";
    body["CanSeek"] = true;
    if (!report.eventName.empty()) body["EventName"] = report.eventName;
    return body.dump();
}

// --- replies ---------------------------------------------------------------

namespace detail {

using json = nlohmann::json;

inline std::optional<json> parseJson(const std::string& text) {
    json value = json::parse(text, nullptr, false);
    if (value.is_discarded()) return std::nullopt;
    return value;
}

inline std::string str(const json& object, const char* key) {
    if (!object.is_object()) return {};
    const auto it = object.find(key);
    if (it == object.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<std::int64_t>());
    return {};
}

inline std::int64_t integer(const json& object, const char* key, std::int64_t fallback = 0) {
    if (!object.is_object()) return fallback;
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number()) return fallback;
    return it->is_number_float() ? static_cast<std::int64_t>(std::llround(it->get<double>()))
                                 : it->get<std::int64_t>();
}

inline bool flag(const json& object, const char* key) {
    if (!object.is_object()) return false;
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

// Optional numbering metadata has to be a nonnegative int, not a rounded
// float, a numeric string or an integer that wraps when narrowed to int.
inline int nonnegativeInt(const json& object, const char* key, int fallback = -1) {
    if (!object.is_object()) return fallback;
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_integer()) return fallback;
    if (it->is_number_unsigned()) {
        const auto value = it->get<std::uint64_t>();
        return value <= static_cast<std::uint64_t>(std::numeric_limits<int>::max())
                   ? static_cast<int>(value) : fallback;
    }
    const auto value = it->get<std::int64_t>();
    return value >= 0 && value <= std::numeric_limits<int>::max() ? static_cast<int>(value) : fallback;
}

inline std::string metadataText(const json& value, std::size_t maximum = 1024) {
    if (!value.is_string()) return {};
    const auto& text = value.get_ref<const std::string&>();
    return text.size() <= maximum ? text : std::string{};
}

inline std::string metadataString(const json& object, const char* key, std::size_t maximum = 1024) {
    if (!object.is_object()) return {};
    const auto value = object.find(key);
    return value == object.end() ? std::string{} : metadataText(*value, maximum);
}

inline std::string metadataId(const json& object, const char* key) {
    if (!object.is_object()) return {};
    const auto value = object.find(key);
    if (value == object.end()) return {};
    if (value->is_string()) return metadataText(*value);
    if (value->is_number_unsigned()) return std::to_string(value->get<std::uint64_t>());
    if (value->is_number_integer()) {
        const auto id = value->get<std::int64_t>();
        if (id >= 0) return std::to_string(id);
    }
    return {};
}

inline std::vector<std::string> metadataStrings(const json& object, const char* key,
                                               std::size_t maximum = kMaximumMetadataEntries,
                                               bool keepSlots = false) {
    std::vector<std::string> result;
    if (!object.is_object()) return result;
    const auto values = object.find(key);
    if (values == object.end() || !values->is_array()) return result;
    const auto count = std::min(maximum, values->size());
    result.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        auto text = metadataText((*values)[i]);
        if (keepSlots || !text.empty()) result.push_back(std::move(text));
    }
    return result;
}

// These are display validation ranges, not a claim that Played or other
// server state follows from a score. Null/invalid scores stay absent.
inline std::optional<double> metadataRating(const json& object, const char* key, double maximum) {
    const auto value = object.find(key);
    if (value == object.end() || !value->is_number()) return std::nullopt;
    const double rating = value->get<double>();
    return std::isfinite(rating) && rating >= 0.0 && rating <= maximum
               ? std::optional<double>{rating} : std::nullopt;
}

inline void metadataTechnicalInfo(Item& item, const json& streams) {
    if (!streams.is_array()) return;
    const auto append = [](std::string& text, const std::string& part) {
        if (part.empty()) return;
        if (!text.empty()) text += " \xC2\xB7 ";
        text += part;
    };
    bool audioDefault = false;
    const auto count = std::min(kMaximumMetadataEntries, streams.size());
    for (std::size_t i = 0; i < count; ++i) {
        const auto& stream = streams[i];
        if (!stream.is_object()) continue;
        const auto type = metadataString(stream, "Type", 32);
        if (type != "Video" && type != "Audio") continue;
        std::string codec = metadataString(stream, "Codec", 64);
        std::transform(codec.begin(), codec.end(), codec.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        std::string info;
        append(info, codec);
        if (type == "Video") {
            if (!item.videoInfo.empty()) continue;
            const int width = nonnegativeInt(stream, "Width", 0);
            const int height = nonnegativeInt(stream, "Height", 0);
            if (width > 0 && width <= 32768 && height > 0 && height <= 32768) {
                append(info, std::to_string(width) + "\xC3\x97" + std::to_string(height));
            }
            const auto range = metadataString(stream, "VideoRange", 64);
            if (range != "SDR") append(info, range);
            if (info.empty()) info = metadataString(stream, "DisplayTitle");
            item.videoInfo = std::move(info);
        } else {
            const bool isDefault = flag(stream, "IsDefault");
            if (!item.audioInfo.empty() && (audioDefault || !isDefault)) continue;
            const int channels = nonnegativeInt(stream, "Channels", 0);
            if (channels > 0 && channels <= 64) append(info, std::to_string(channels) + "ch");
            const int rate = nonnegativeInt(stream, "SampleRate", 0);
            if (rate > 0 && rate <= 768000) append(info, std::to_string(rate) + " Hz");
            append(info, metadataString(stream, "Language", 64));
            if (info.empty()) info = metadataString(stream, "DisplayTitle");
            if (!info.empty()) {
                item.audioInfo = std::move(info);
                audioDefault = isDefault;
            }
        }
    }
}

inline void itemDetailsFromJson(Item& item, const json& j) {
    item.overview = metadataString(j, "Overview", kMaximumOverviewBytes);
    item.officialRating = metadataString(j, "OfficialRating");
    item.originalTitle = metadataString(j, "OriginalTitle");
    item.communityRating = metadataRating(j, "CommunityRating", 10.0);
    item.criticRating = metadataRating(j, "CriticRating", 100.0);
    item.genres = metadataStrings(j, "Genres");
    item.taglines = metadataStrings(j, "Taglines");
    item.backdropImageTags = metadataStrings(j, "BackdropImageTags", kMaximumBackdropImages, true);
    item.parentBackdropImageTags = metadataStrings(j, "ParentBackdropImageTags", kMaximumBackdropImages, true);
    item.parentBackdropItemId = metadataId(j, "ParentBackdropItemId");
    item.parentLogoItemId = metadataId(j, "ParentLogoItemId");
    item.parentLogoImageTag = metadataString(j, "ParentLogoImageTag");
    if (const auto images = j.find("ImageTags"); images != j.end() && images->is_object()) {
        item.logoImageTag = metadataString(*images, "Logo");
    }
    if (const auto studios = j.find("Studios"); studios != j.end() && studios->is_array()) {
        const auto count = std::min(kMaximumMetadataEntries, studios->size());
        for (std::size_t i = 0; i < count; ++i) {
            auto name = metadataString((*studios)[i], "Name");
            if (!name.empty()) item.studios.push_back(std::move(name));
        }
    }
    if (const auto people = j.find("People"); people != j.end() && people->is_array()) {
        const auto count = std::min(kMaximumMetadataEntries, people->size());
        for (std::size_t i = 0; i < count; ++i) {
            const auto& value = (*people)[i];
            Person person;
            person.name = metadataString(value, "Name");
            if (person.name.empty()) continue;
            person.id = metadataId(value, "Id");
            person.type = metadataString(value, "Type", 64);
            person.role = metadataString(value, "Role");
            person.primaryImageTag = metadataString(value, "PrimaryImageTag");
            item.people.push_back(std::move(person));
        }
    }
    if (const auto streams = j.find("MediaStreams"); streams != j.end()) metadataTechnicalInfo(item, *streams);
    if (item.videoInfo.empty() || item.audioInfo.empty()) {
        if (const auto sources = j.find("MediaSources"); sources != j.end() && sources->is_array()) {
            const auto count = std::min(kMaximumMetadataEntries, sources->size());
            for (std::size_t i = 0; i < count; ++i) {
                const auto& source = (*sources)[i];
                if (!source.is_object()) continue;
                const auto streams = source.find("MediaStreams");
                Item technical;
                if (streams != source.end()) metadataTechnicalInfo(technical, *streams);
                if (technical.videoInfo.empty() && technical.audioInfo.empty()) continue;
                if (item.videoInfo.empty()) item.videoInfo = std::move(technical.videoInfo);
                if (item.audioInfo.empty()) item.audioInfo = std::move(technical.audioInfo);
                break;  // Never combine technical data from different versions.
            }
        }
    }
}

inline Item itemFromJson(const json& j) {
    Item item;
    item.id = str(j, "Id");
    item.name = str(j, "Name");
    item.type = str(j, "Type");
    item.collectionType = str(j, "CollectionType");
    item.container = str(j, "Container");
    item.mediaType = str(j, "MediaType");
    item.seriesId = str(j, "SeriesId");
    item.seriesName = str(j, "SeriesName");
    item.seasonId = str(j, "SeasonId");
    item.parentId = str(j, "ParentId");
    item.indexNumber = nonnegativeInt(j, "IndexNumber");
    item.parentIndexNumber = nonnegativeInt(j, "ParentIndexNumber");
    item.indexNumberEnd = nonnegativeInt(j, "IndexNumberEnd");
    const int year = nonnegativeInt(j, "ProductionYear", 0);
    item.productionYear = year > 0 && year <= 9999 ? year : 0;
    item.isFolder = flag(j, "IsFolder");
    item.runTimeTicks = integer(j, "RunTimeTicks");
    item.width = static_cast<int>(integer(j, "Width"));
    item.height = static_cast<int>(integer(j, "Height"));
    item.dateCreated = str(j, "DateCreated");
    item.premiereDate = str(j, "PremiereDate");
    if (const auto size = j.find("Size"); size != j.end() && size->is_number()) {
        item.size = size->get<std::int64_t>();
    }
    item.playlistItemId = str(j, "PlaylistItemId");
    item.childCount = static_cast<int>(integer(j, "ChildCount", -1));
    if (const auto tags = j.find("ImageTags"); tags != j.end() && tags->is_object()) {
        item.primaryImageTag = str(*tags, "Primary");
    }
    if (item.primaryImageTag.empty()) {
        // A folder in a home-video library carries the picture of one of
        // its files instead of one of its own.
        item.primaryImageItemId = str(j, "PrimaryImageItemId");
        if (!item.primaryImageItemId.empty()) item.primaryImageTag = str(j, "PrimaryImageTag");
    }
    if (const auto aspect = j.find("PrimaryImageAspectRatio"); aspect != j.end() && aspect->is_number()) {
        item.primaryImageAspect = aspect->get<double>();
    }
    if (const auto user = j.find("UserData"); user != j.end() && user->is_object()) {
        item.positionTicks = integer(*user, "PlaybackPositionTicks");
        item.played = flag(*user, "Played");
        item.unplayedCount = static_cast<int>(integer(*user, "UnplayedItemCount", -1));
    }
    itemDetailsFromJson(item, j);
    return item;
}

inline MediaStream streamFromJson(const json& j) {
    MediaStream stream;
    stream.index = static_cast<int>(integer(j, "Index", -1));
    stream.type = str(j, "Type");
    stream.codec = str(j, "Codec");
    stream.language = str(j, "Language");
    stream.displayTitle = str(j, "DisplayTitle");
    stream.title = str(j, "Title");
    stream.isExternal = flag(j, "IsExternal");
    stream.isDefault = flag(j, "IsDefault");
    stream.isForced = flag(j, "IsForced");
    stream.channels = static_cast<int>(integer(j, "Channels"));
    stream.isTextSubtitle = stream.type == "Subtitle" &&
                            (flag(j, "IsTextSubtitleStream") || isTextSubtitleCodec(stream.codec));
    return stream;
}

}  // namespace detail

inline std::optional<ServerInfo> parseServerInfo(const std::string& text) {
    const auto j = detail::parseJson(text);
    if (!j || !j->is_object()) return std::nullopt;
    ServerInfo info;
    info.name = detail::str(*j, "ServerName");
    info.version = detail::str(*j, "Version");
    info.id = detail::str(*j, "Id");
    if (info.id.empty()) return std::nullopt;
    return info;
}

inline std::optional<Authentication> parseAuthentication(const std::string& text) {
    const auto j = detail::parseJson(text);
    if (!j || !j->is_object()) return std::nullopt;
    Authentication auth;
    auth.token = detail::str(*j, "AccessToken");
    auth.serverId = detail::str(*j, "ServerId");
    if (const auto user = j->find("User"); user != j->end() && user->is_object()) {
        auth.userId = detail::str(*user, "Id");
        auth.userName = detail::str(*user, "Name");
        if (auth.serverId.empty()) auth.serverId = detail::str(*user, "ServerId");
    }
    if (auth.token.empty() || auth.userId.empty()) return std::nullopt;
    return auth;
}

// {"Items":[...],"TotalRecordCount":n} or, for /Items/Latest, a bare array.
inline std::optional<ItemList> parseItems(const std::string& text) {
    const auto j = detail::parseJson(text);
    if (!j) return std::nullopt;
    ItemList list;
    list.total = -1;
    const nlohmann::json* array = nullptr;
    if (j->is_array()) {
        array = &*j;
    } else if (j->is_object()) {
        const auto items = j->find("Items");
        if (items == j->end() || !items->is_array()) return std::nullopt;
        array = &*items;
        list.total = static_cast<int>(detail::integer(*j, "TotalRecordCount", -1));
    } else {
        return std::nullopt;
    }
    for (const auto& entry : *array) {
        if (entry.is_object()) list.items.push_back(detail::itemFromJson(entry));
    }
    if (list.total < 0) list.total = static_cast<int>(list.items.size());
    return list;
}

inline std::optional<Item> parseItem(const std::string& text) {
    if (text.size() > kMaximumItemReplyBytes) return std::nullopt;
    const auto j = detail::parseJson(text);
    if (!j || !j->is_object()) return std::nullopt;
    Item item = detail::itemFromJson(*j);
    if (item.id.empty()) return std::nullopt;
    return item;
}

inline std::optional<std::vector<Item>> parseAncestorsArray(const std::string& text) {
    if (text.size() > kMaximumItemReplyBytes) return std::nullopt;
    const auto j = detail::parseJson(text);
    if (!j || !j->is_array() || j->size() > kMaximumMetadataEntries) return std::nullopt;
    std::vector<Item> ancestors;
    for (const auto& value : *j) {
        if (!value.is_object()) continue;
        auto item = detail::itemFromJson(value);
        if (!item.id.empty()) ancestors.push_back(std::move(item));
    }
    return ancestors;
}

// Virtual user views need not appear among physical ancestors. Only a
// unique exact id match to a known library establishes the source library.
inline std::string uniqueAncestorLibraryId(const std::vector<Item>& ancestors,
                                           const std::vector<Item>& knownLibraries) {
    std::string match;
    for (const auto& ancestor : ancestors) {
        if (ancestor.id.empty()) continue;
        const bool known = std::any_of(knownLibraries.begin(), knownLibraries.end(), [&](const Item& library) {
            return library.id == ancestor.id && (library.type == "CollectionFolder" || library.type == "UserView");
        });
        if (!known) continue;
        if (!match.empty() && match != ancestor.id) return {};
        match = ancestor.id;
    }
    return match;
}

inline std::optional<PlaybackInfo> parsePlaybackInfo(const std::string& text) {
    const auto j = detail::parseJson(text);
    if (!j || !j->is_object()) return std::nullopt;
    PlaybackInfo info;
    info.playSessionId = detail::str(*j, "PlaySessionId");
    info.errorCode = detail::str(*j, "ErrorCode");
    if (const auto sources = j->find("MediaSources"); sources != j->end() && sources->is_array()) {
        for (const auto& entry : *sources) {
            if (!entry.is_object()) continue;
            MediaSource source;
            source.id = detail::str(entry, "Id");
            source.container = detail::str(entry, "Container");
            source.protocol = detail::str(entry, "Protocol");
            source.transcodingUrl = detail::str(entry, "TranscodingUrl");
            source.supportsDirectPlay = detail::flag(entry, "SupportsDirectPlay");
            source.supportsDirectStream = detail::flag(entry, "SupportsDirectStream");
            source.runTimeTicks = detail::integer(entry, "RunTimeTicks");
            source.defaultAudioStreamIndex =
                static_cast<int>(detail::integer(entry, "DefaultAudioStreamIndex", -1));
            source.defaultSubtitleStreamIndex =
                static_cast<int>(detail::integer(entry, "DefaultSubtitleStreamIndex", -1));
            if (const auto streams = entry.find("MediaStreams"); streams != entry.end() && streams->is_array()) {
                for (const auto& stream : *streams) {
                    if (stream.is_object()) source.streams.push_back(detail::streamFromJson(stream));
                }
            }
            info.sources.push_back(std::move(source));
        }
    }
    return info;
}

// The source to play: the first one the server will hand over as a file.
inline const MediaSource* directPlaySource(const PlaybackInfo& info) {
    for (const auto& source : info.sources) {
        if (source.supportsDirectPlay || source.supportsDirectStream) return &source;
    }
    return info.sources.empty() ? nullptr : &info.sources.front();
}

// --- locators --------------------------------------------------------------

// What a pane holds instead of a file path: emby://<serverId>/<itemId>. The
// stream URL is resolved when the pane opens, so a session file never carries
// a token or a server address that may change.
struct Locator {
    std::string serverId, itemId;
};

inline constexpr std::wstring_view kLocatorPrefix = L"emby://";

inline bool isLocator(std::wstring_view path) {
    return path.size() > kLocatorPrefix.size() && path.substr(0, kLocatorPrefix.size()) == kLocatorPrefix;
}

inline std::wstring formatLocator(const std::string& serverId, const std::string& itemId) {
    return std::wstring(kLocatorPrefix) + utf8ToWideText(serverId) + L"/" + utf8ToWideText(itemId);
}

inline std::optional<Locator> parseLocator(std::wstring_view path) {
    if (!isLocator(path)) return std::nullopt;
    path.remove_prefix(kLocatorPrefix.size());
    const auto slash = path.find(L'/');
    if (slash == std::wstring_view::npos || slash == 0 || slash + 1 >= path.size()) return std::nullopt;
    Locator locator;
    locator.serverId = wideToUtf8Text(path.substr(0, slash));
    locator.itemId = wideToUtf8Text(path.substr(slash + 1));
    if (locator.itemId.find(L'/') != std::string::npos) return std::nullopt;
    return locator;
}

// --- display ---------------------------------------------------------------

inline std::string episodeCode(const Item& item) {
    if (item.indexNumber < 0) return {};
    std::string code;
    if (item.parentIndexNumber >= 0) code += "S" + std::to_string(item.parentIndexNumber);
    code += "E" + std::to_string(item.indexNumber);
    if (item.indexNumberEnd > item.indexNumber) code += "\xE2\x80\x93" "E" + std::to_string(item.indexNumberEnd);
    return code;
}

// Film/series years distinguish remakes; seasons and episodes keep readable
// titles when optional names or numbering metadata are missing.
inline std::string displayTitle(const Item& item) {
    if (item.type == "Movie" || item.type == "Series") {
        std::string title = item.name.empty() ? item.type : item.name;
        if (item.productionYear > 0 && item.productionYear <= 9999) {
            title += " (" + std::to_string(item.productionYear) + ")";
        }
        return title;
    }
    if (item.type == "Season" && item.name.empty()) {
        if (item.indexNumber == 0) return "Specials";
        return item.indexNumber > 0 ? "Season " + std::to_string(item.indexNumber) : "Season";
    }
    if (item.type != "Episode") return item.name;
    std::string title;
    const auto add = [&](const std::string& part) {
        if (part.empty()) return;
        if (!title.empty()) title += " \xC2\xB7 ";
        title += part;
    };
    add(item.seriesName);
    add(episodeCode(item));
    add(item.name);
    return title.empty() ? "Episode" : title;
}

// "✓" when played, "▶ 40%" when partly watched, otherwise empty.
inline std::string progressLabel(const Item& item) {
    if (item.played) return "\xE2\x9C\x93";
    if (item.positionTicks > 0 && item.runTimeTicks > 0) {
        const int percent = static_cast<int>(std::clamp(
            std::llround(100.0 * static_cast<double>(item.positionTicks) /
                         static_cast<double>(item.runTimeTicks)), 0LL, 100LL));
        return "\xE2\x96\xB6 " + std::to_string(percent) + "%";
    }
    return {};
}

// A label for an audio or subtitle stream in a menu.
inline std::string streamLabel(const MediaStream& stream) {
    if (!stream.displayTitle.empty()) return stream.displayTitle;
    std::string label = stream.language.empty() ? "Track" : stream.language;
    if (!stream.codec.empty()) label += " " + stream.codec;
    if (stream.channels > 0) label += " " + std::to_string(stream.channels) + "ch";
    return label;
}

// Orders a list the browser merged itself (search results) the way the
// server would have: folders first, then the key. LastPlayed has no field
// in a list reply and Random keeps the order it came in. Updated has no
// field either and goes by the item's number, which says what the server
// took in later, though not what it changed since.
inline void sortItems(std::vector<Item>& items, SortKey key, bool descending) {
    if (key == SortKey::Random) return;
    const auto lower = [](const std::string& text) {
        std::wstring wide = utf8ToWideText(text);
        for (auto& c : wide) c = static_cast<wchar_t>(std::towlower(c));
        return wide;
    };
    std::stable_sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
        if (a.isFolder != b.isFolder) return a.isFolder;
        int order = 0;
        switch (key) {
        case SortKey::DateAdded: order = a.dateCreated.compare(b.dateCreated); break;
        case SortKey::Release: order = a.premiereDate.compare(b.premiereDate); break;
        case SortKey::Runtime: order = a.runTimeTicks < b.runTimeTicks ? -1 : a.runTimeTicks > b.runTimeTicks ? 1 : 0; break;
        case SortKey::Size: order = a.size < b.size ? -1 : a.size > b.size ? 1 : 0; break;
        case SortKey::Updated: {
            const long long left = itemNumber(a.id), right = itemNumber(b.id);
            order = left < right ? -1 : left > right ? 1 : 0;
            break;
        }
        default: order = lower(a.name).compare(lower(b.name)); break;
        }
        if (order == 0) order = lower(a.name).compare(lower(b.name));
        return descending ? order > 0 : order < 0;
    });
}

// Where the entry being played is in a list: the one with its place in the
// playlist when that is known -- a playlist may hold a video twice -- and
// the first with its id otherwise. -1 when the list does not hold it.
inline int entryIndex(const std::vector<Item>& items, const std::string& itemId,
                      const std::string& playlistItemId = {}) {
    int first = -1;
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].id != itemId) continue;
        if (!playlistItemId.empty() && items[i].playlistItemId == playlistItemId) return static_cast<int>(i);
        if (first < 0) first = static_cast<int>(i);
    }
    return first;
}

// Whether the entries two lists share come in the same order in both. A
// page asked for again may bring entries the list being played does not
// have, and those are welcome; it must not re-order the list under the
// viewer -- sorted by last played, the video just started would jump to
// the top and the next step would go back to the one before it.
inline bool sameRelativeOrder(const std::vector<Item>& held, const std::vector<Item>& fresh) {
    const auto key = [](const Item& item) { return item.id + "|" + item.playlistItemId; };
    std::vector<std::string> heldKeys, freshKeys;
    heldKeys.reserve(held.size());
    freshKeys.reserve(fresh.size());
    for (const auto& item : held) heldKeys.push_back(key(item));
    for (const auto& item : fresh) freshKeys.push_back(key(item));
    std::vector<std::string> heldSorted = heldKeys, freshSorted = freshKeys;
    std::sort(heldSorted.begin(), heldSorted.end());
    std::sort(freshSorted.begin(), freshSorted.end());
    const auto shared = [](const std::vector<std::string>& keys, const std::vector<std::string>& other) {
        std::vector<std::string> kept;
        for (const auto& value : keys) {
            if (std::binary_search(other.begin(), other.end(), value)) kept.push_back(value);
        }
        return kept;
    };
    return shared(heldKeys, freshSorted) == shared(freshKeys, heldSorted);
}

// Whether a page asked for again says anything the list shown does not:
// another item, another order, a title, a picture, how far something was
// watched.
inline bool sameListing(const std::vector<Item>& shown, const std::vector<Item>& fresh) {
    if (shown.size() != fresh.size()) return false;
    for (std::size_t i = 0; i < shown.size(); ++i) {
        const Item& a = shown[i];
        const Item& b = fresh[i];
        if (a.id != b.id || a.playlistItemId != b.playlistItemId || a.name != b.name ||
            a.seriesName != b.seriesName || a.indexNumber != b.indexNumber ||
            a.parentIndexNumber != b.parentIndexNumber || a.indexNumberEnd != b.indexNumberEnd ||
            a.productionYear != b.productionYear ||
            a.positionTicks != b.positionTicks || a.played != b.played ||
            a.unplayedCount != b.unplayedCount || a.childCount != b.childCount ||
            a.runTimeTicks != b.runTimeTicks || a.primaryImageTag != b.primaryImageTag ||
            a.primaryImageItemId != b.primaryImageItemId) {
            return false;
        }
    }
    return true;
}

// --- keeping a list fresh ------------------------------------------------------

// What was watched, added or renamed on the server since a page was asked
// for shows up only when the page is asked for again. The browser asks
// again, in place: when it is opened; when something says the server has
// news (`wanted`: the window came back to the front, a video the player
// reported has started or stopped); and every half minute it stays up.
// Never while a press or a drag is under way or a request for the page is
// out, not more often than every two seconds, and only where asking again
// gives the same order -- a random order would shuffle the list being
// looked at, and a search is several requests merged.
inline constexpr unsigned long long kListRefreshIntervalMs = 30'000;
inline constexpr unsigned long long kListRefreshSpacingMs = 2'000;
// A list grown beyond this by scrolling is asked for again when there is
// news, not every half minute.
inline constexpr int kListRefreshLargest = 2000;

struct ListRefreshState {
    bool shown{};       // the browser is up and signed in, on a page the server was asked for
    bool busy{};        // a press or a drag is under way, or a request for the page is out
    bool keepsOrder{};  // asking again gives the same order
    int held{};         // items of the list shown
    // Ticks in milliseconds: now, when the page was last asked for (0 for
    // never), and from when on a refresh is wanted (0 for none).
    unsigned long long now{}, asked{}, wanted{};
};

inline bool listRefreshDue(const ListRefreshState& state) {
    if (!state.shown || state.busy || !state.keepsOrder || state.asked == 0) return false;
    if (state.now < state.asked + kListRefreshSpacingMs) return false;
    if (state.wanted != 0 && state.now >= state.wanted) return true;
    return state.held <= kListRefreshLargest && state.now >= state.asked + kListRefreshIntervalMs;
}

// The neighbour of `currentId` in a list, without wrapping: a season has a
// first and a last episode.
inline int adjacentIndex(const std::vector<Item>& items, const std::string& currentId, int step) {
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].id != currentId) continue;
        const long long next = static_cast<long long>(i) + step;
        if (next < 0 || next >= static_cast<long long>(items.size())) return -1;
        return static_cast<int>(next);
    }
    return -1;
}

// --- the signed-in state file ---------------------------------------------

// %LOCALAPPDATA%\QuadDeck\emby.qauth. The token is stored as App protected
// it (DPAPI, base64); this layer only carries the string.
struct StoredAuth {
    std::string serverUrl, serverId, serverName, userId, userName, deviceId, protectedToken;
};

inline bool writeStoredAuth(std::wostream& output, const StoredAuth& auth) {
    const auto w = [](const std::string& value) { return utf8ToWideText(value); };
    output << L"QEMBY 1\n";
    output << L"server " << std::quoted(w(auth.serverUrl)) << L' ' << std::quoted(w(auth.serverId))
           << L' ' << std::quoted(w(auth.serverName)) << L'\n';
    output << L"user " << std::quoted(w(auth.userId)) << L' ' << std::quoted(w(auth.userName)) << L'\n';
    output << L"device " << std::quoted(w(auth.deviceId)) << L'\n';
    output << L"token " << std::quoted(w(auth.protectedToken)) << L'\n';
    return output.good();
}

inline bool readStoredAuth(std::wistream& input, StoredAuth& auth) {
    std::wstring key;
    int version{};
    if (!(input >> key >> version) || key != L"QEMBY" || version != 1) return false;
    std::wstring url, serverId, serverName, userId, userName, deviceId, token;
    if (!(input >> key) || key != L"server" ||
        !(input >> std::quoted(url) >> std::quoted(serverId) >> std::quoted(serverName))) return false;
    if (!(input >> key) || key != L"user" ||
        !(input >> std::quoted(userId) >> std::quoted(userName))) return false;
    if (!(input >> key) || key != L"device" || !(input >> std::quoted(deviceId))) return false;
    if (!(input >> key) || key != L"token" || !(input >> std::quoted(token))) return false;
    auth.serverUrl = wideToUtf8Text(url);
    auth.serverId = wideToUtf8Text(serverId);
    auth.serverName = wideToUtf8Text(serverName);
    auth.userId = wideToUtf8Text(userId);
    auth.userName = wideToUtf8Text(userName);
    auth.deviceId = wideToUtf8Text(deviceId);
    auth.protectedToken = wideToUtf8Text(token);
    return !auth.serverUrl.empty();
}

}  // namespace quaddeck::emby
