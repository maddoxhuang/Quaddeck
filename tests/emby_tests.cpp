// Pure Emby protocol, locator, auth-file, subtitle and text-encoding checks.
// The JSON here is what an Emby 4.11.0.3 server answered on 2026-09-26,
// trimmed to the keys the parsers read.

#include "EmbyApi.hpp"
#include "EmbyPlayback.hpp"
#include "LocalPlaylist.hpp"
#include "Subtitles.hpp"
#include "TextEncoding.hpp"
#include "Thumbnails.hpp"

#include <cassert>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>

using namespace quaddeck;

namespace {

bool near(double a, double b, double tolerance = 0.0005) { return std::abs(a - b) < tolerance; }

void protocolTests() {
    // --- ticks --------------------------------------------------------------
    assert(emby::ticksFromSeconds(30.0) == 300000000);
    assert(emby::ticksFromSeconds(-1.0) == 0);
    assert(emby::ticksFromSeconds(0.0) == 0);
    assert(near(emby::secondsFromTicks(14200110000), 1420.011));
    assert(emby::secondsFromTicks(-5) == 0.0);

    // --- strings ------------------------------------------------------------
    assert(emby::urlEncode("a b/c,d~x-y_z.9") == "a%20b%2Fc%2Cd~x-y_z.9");
    assert(emby::urlEncode("\xE3\x82\x88") == "%E3%82%88");
    assert(emby::normalizeServerUrl("  192.168.1.2:8096/ ") == "http://192.168.1.2:8096");
    assert(emby::normalizeServerUrl("HTTPS://emby.example.com/emby//") == "HTTPS://emby.example.com/emby");
    assert(emby::normalizeServerUrl("   ") == "");
    // Credentials in the address are dropped: the dialog is where they go.
    assert(emby::normalizeServerUrl("http://user:secret@host:8096/") == "http://host:8096");
    assert(emby::normalizeServerUrl("host:8096/emby") == "http://host:8096/emby");
    const std::string header = emby::authorizationHeader("dev42");
    assert(header.find("Client=\"QuadDeck\"") != std::string::npos);
    assert(header.find("DeviceId=\"dev42\"") != std::string::npos);
    assert(header.find("Token") == std::string::npos);

    // --- paths --------------------------------------------------------------
    emby::ItemsQuery query;
    query.parentId = "47096";
    query.includeTypes = "Series,Movie";
    query.recursive = true;
    query.startIndex = 300;
    query.limit = 300;
    assert(emby::itemsPath("u1", query) ==
           "/Users/u1/Items?ParentId=47096&IncludeItemTypes=Series%2CMovie&Recursive=true"
           "&SortBy=SortName&SortOrder=Ascending&StartIndex=300&Limit=300");
    emby::ItemsQuery folder;
    folder.parentId = "93465";
    folder.sortBy = "IsFolder,SortName";
    folder.fields = "DateCreated";
    assert(emby::itemsPath("u1", folder) ==
           "/Users/u1/Items?ParentId=93465&SortBy=IsFolder%2CSortName&SortOrder=Ascending"
           "&Fields=DateCreated&StartIndex=0&Limit=300");
    assert(emby::viewsPath("u1") == "/Users/u1/Views");
    const std::string browserFields = "&Fields=ParentId%2CDateCreated%2CPrimaryImageAspectRatio%2CSize%2CChildCount";
    assert(emby::seasonsPath("109364", "u1") == "/Shows/109364/Seasons?UserId=u1" + browserFields);
    assert(emby::episodesPath("109364", "u1", "109665") ==
           "/Shows/109364/Episodes?UserId=u1&SeasonId=109665" + browserFields);
    assert(emby::episodesPath("109364", "u1") == "/Shows/109364/Episodes?UserId=u1" + browserFields);
    assert(emby::resumePath("u1", 10) == "/Users/u1/Items/Resume?MediaTypes=Video&Limit=10" + browserFields);
    assert(emby::itemPath("u1", "109672") ==
           "/Users/u1/Items/109672?Fields=ParentId%2CDateCreated%2CPrimaryImageAspectRatio%2CSize%2CChildCount");
    assert(emby::playbackInfoPath("109672", "u1") == "/Items/109672/PlaybackInfo?UserId=u1");
    assert(emby::streamPath("109672", "mediasource_109672", "psid") ==
           "/Videos/109672/stream?MediaSourceId=mediasource_109672&Static=true&PlaySessionId=psid");
    assert(emby::streamPath("1", "ms", "") == "/Videos/1/stream?MediaSourceId=ms&Static=true");
    assert(emby::subtitlePath("110010", "ms", 4) == "/Videos/110010/ms/Subtitles/4/Stream.srt");
    assert(emby::imagePath("119982", 1920, "abc") ==
           "/Items/119982/Images/Primary?quality=90&AutoOrient=true&maxWidth=1920&tag=abc");
    assert(emby::imagePath("119982", 0) == "/Items/119982/Images/Primary?quality=90&AutoOrient=true");
    assert(emby::downloadPath("5") == "/Items/5/Download");
    emby::ItemsQuery search;
    search.parentId = "79571";
    search.recursive = true;
    search.includeTypes = "Series,Movie,Video";
    search.searchTerm = "86 eighty";
    search.limit = 200;
    assert(emby::itemsPath("u1", search) ==
           "/Users/u1/Items?ParentId=79571&IncludeItemTypes=Series%2CMovie%2CVideo&Recursive=true"
           "&SortBy=SortName&SortOrder=Ascending&SearchTerm=86%20eighty&StartIndex=0&Limit=200");

    // --- the browser's orders and filters --------------------------------
    assert(std::string(emby::sortByValue(emby::SortKey::Name)) == "SortName");
    assert(std::string(emby::sortByValue(emby::SortKey::DateAdded)) == "DateCreated");
    assert(std::string(emby::sortByValue(emby::SortKey::Release)) == "PremiereDate");
    assert(std::string(emby::sortByValue(emby::SortKey::Runtime)) == "Runtime");
    assert(std::string(emby::sortByValue(emby::SortKey::Random)) == "Random");
    assert(std::string(emby::sortByValue(emby::SortKey::LastPlayed)) == "DatePlayed");
    assert(std::string(emby::sortByValue(emby::SortKey::Size)) == "Size");   // "FileSize" is a 500
    assert(emby::sortKeyFromIndex(-3) == emby::SortKey::Name && emby::sortKeyFromIndex(99) == emby::SortKey::Updated);
    assert(emby::sortKeyFromIndex(6) == emby::SortKey::Size && emby::sortKeyFromIndex(7) == emby::SortKey::Updated &&
           emby::kSortKeyCount == 8);
    // What the server took in or changed last: "DateLastSaved", newest
    // first ("DateAdded" is a 500, and DateCreated here is the file's date).
    assert(std::string(emby::sortByValue(emby::SortKey::Updated)) == "DateLastSaved");
    assert(std::wstring(emby::sortKeyName(emby::SortKey::Updated)) == L"Updated");
    assert(emby::sortDescendsByDefault(emby::SortKey::Updated));
    {
        emby::ListPage library;
        library.kind = emby::ListKind::Library;
        library.id = "47096";
        library.collectionType = "homevideos";
        emby::ListArrangement newest;
        newest.sort = emby::SortKey::Updated;
        newest.descending = true;
        assert(emby::itemsPath("u1", emby::listQuery(library, newest, 0, 300, "ParentId")) ==
               "/Users/u1/Items?ParentId=47096&SortBy=IsFolder%2CDateLastSaved&SortOrder=Ascending%2CDescending"
               "&Fields=ParentId&StartIndex=0&Limit=300");
        newest.flat = true;
        assert(emby::itemsPath("u1", emby::listQuery(library, newest, 0, 300, "ParentId")) ==
               "/Users/u1/Items?ParentId=47096&IncludeItemTypes=Video%2CPhoto%2CMovie%2CEpisode&Recursive=true"
               "&SortBy=DateLastSaved&SortOrder=Descending&Fields=ParentId&StartIndex=0&Limit=300");
        // A list merged here has no such date and goes by the number the
        // server gave each item, which counts up.
        assert(emby::itemNumber("123200") == 123200 && emby::itemNumber("0") == 0);
        assert(emby::itemNumber("") == -1 && emby::itemNumber("12a") == -1 &&
               emby::itemNumber("ad1da731c16b46e588aca50ea1dd80d7") == -1 &&
               emby::itemNumber("1234567890123456789") == -1);
        std::vector<emby::Item> found(4);
        found[0].id = "119854"; found[0].name = "b";
        found[1].id = "123200"; found[1].name = "d";
        found[2].id = "9731";   found[2].name = "a";
        found[3].id = "123199"; found[3].name = "c";
        emby::sortItems(found, emby::SortKey::Updated, true);
        assert(found[0].id == "123200" && found[1].id == "123199" && found[2].id == "119854" && found[3].id == "9731");
        emby::sortItems(found, emby::SortKey::Updated, false);
        assert(found[0].id == "9731" && found[3].id == "123200");
    }
    assert(std::wstring(emby::sortKeyName(emby::SortKey::Size)) == L"Size");
    assert(!emby::sortDescendsByDefault(emby::SortKey::Name) && emby::sortDescendsByDefault(emby::SortKey::DateAdded) &&
           !emby::sortDescendsByDefault(emby::SortKey::Random) && emby::sortDescendsByDefault(emby::SortKey::Runtime) &&
           emby::sortDescendsByDefault(emby::SortKey::Size));
    {
        emby::ItemsQuery bySize;
        bySize.parentId = "47096";
        emby::applySort(bySize, emby::SortKey::Size, true, true);
        assert(bySize.sortBy == "IsFolder,Size" && bySize.sortOrder == "Ascending,Descending");
    }
    emby::ItemsQuery ordered;
    ordered.parentId = "47096";
    ordered.filters = "IsUnplayed";
    emby::applySort(ordered, emby::SortKey::DateAdded, true, true);
    assert(emby::itemsPath("u1", ordered) ==
           "/Users/u1/Items?ParentId=47096&SortBy=IsFolder%2CDateCreated&SortOrder=Ascending%2CDescending"
           "&Filters=IsUnplayed&StartIndex=0&Limit=300");
    emby::applySort(ordered, emby::SortKey::Runtime, false, false);
    assert(ordered.sortBy == "Runtime" && ordered.sortOrder == "Ascending");
    // Random carries no order and never puts folders first (the server
    // answers 500 to an order it does not know).
    emby::applySort(ordered, emby::SortKey::Random, true, true);
    assert(ordered.sortBy == "Random" && ordered.sortOrder.empty());
    assert(emby::itemsPath("u1", ordered).find("SortOrder") == std::string::npos);
    assert(emby::itemsPath("u1", ordered).find("SortBy=Random") != std::string::npos);

    // --- pictures and their keys ----------------------------------------
    {
        emby::Item clip;
        clip.id = "109672";
        clip.primaryImageTag = "abc";
        assert(emby::imageKey(clip, 440) == "109672|abc|440");
        emby::Item borrowed = clip;
        borrowed.primaryImageItemId = "555";
        assert(emby::imageKey(borrowed, 300) == "555|abc|300");
        emby::Item bare;
        bare.id = "1";
        assert(emby::imageKey(bare, 440).empty());   // no picture: never asked for
        const auto parsed = emby::parseImageKey("555|abc|300");
        assert(parsed && parsed->itemId == "555" && parsed->tag == "abc" && parsed->width == 300);
        assert(!emby::parseImageKey("") && !emby::parseImageKey("a|b") && !emby::parseImageKey("|b|3") &&
               !emby::parseImageKey("a|b|x") && !emby::parseImageKey("a|b|0"));
        assert(emby::imagePath(parsed->itemId, parsed->width, parsed->tag) ==
               "/Items/555/Images/Primary?quality=90&AutoOrient=true&maxWidth=300&tag=abc");

        ThumbnailCache cache;
        assert(!cache.contains("k") && !cache.find("k"));
        cache.insert("k", "bytes");
        assert(cache.contains("k") && cache.find("k") && *cache.find("k") == "bytes" && cache.bytes() == 5);
        cache.insert("k", "more bytes");
        assert(cache.size() == 1 && cache.bytes() == 10 && *cache.find("k") == "more bytes");
        cache.insert("failed", "");
        assert(cache.contains("failed") && cache.find("failed")->empty());
        for (int i = 0; i < static_cast<int>(ThumbnailCache::kMaximumEntries); ++i) {
            cache.insert("n" + std::to_string(i), "x");
        }
        assert(cache.size() == ThumbnailCache::kMaximumEntries);
        assert(!cache.contains("k") && !cache.contains("failed"));  // the oldest went first
        assert(cache.contains("n" + std::to_string(ThumbnailCache::kMaximumEntries - 1)));
        cache.clear();
        assert(cache.size() == 0 && cache.bytes() == 0);
    }

    // --- ordering a merged list -----------------------------------------
    {
        std::vector<emby::Item> mixed(5);
        mixed[0].id = "v1"; mixed[0].name = "beta"; mixed[0].dateCreated = "2026-02-01T00:00:00Z"; mixed[0].runTimeTicks = 30;
        mixed[1].id = "f1"; mixed[1].name = "Zed folder"; mixed[1].isFolder = true;
        mixed[2].id = "v2"; mixed[2].name = "Alpha"; mixed[2].dateCreated = "2026-03-01T00:00:00Z"; mixed[2].runTimeTicks = 10;
        mixed[3].id = "f2"; mixed[3].name = "apple folder"; mixed[3].isFolder = true;
        mixed[4].id = "v3"; mixed[4].name = "gamma"; mixed[4].dateCreated = "2026-01-01T00:00:00Z"; mixed[4].runTimeTicks = 20;
        mixed[0].size = 300; mixed[2].size = 100; mixed[4].size = 200;
        const auto order = [&] {
            std::string ids;
            for (const auto& entry : mixed) ids += entry.id + " ";
            return ids;
        };
        emby::sortItems(mixed, emby::SortKey::Name, false);
        assert(order() == "f2 f1 v2 v1 v3 ");          // folders first, names without case
        emby::sortItems(mixed, emby::SortKey::Name, true);
        assert(order() == "f1 f2 v3 v1 v2 ");          // reversed within each group
        emby::sortItems(mixed, emby::SortKey::DateAdded, true);
        assert(order() == "f1 f2 v2 v1 v3 ");          // newest first; folders by name
        emby::sortItems(mixed, emby::SortKey::Runtime, false);
        assert(order() == "f2 f1 v2 v3 v1 ");
        emby::sortItems(mixed, emby::SortKey::Size, true);
        assert(order() == "f1 f2 v1 v3 v2 ");          // largest first; folders (no size) by name
        emby::sortItems(mixed, emby::SortKey::Size, false);
        assert(order() == "f2 f1 v2 v3 v1 ");
        const auto before = order();
        emby::sortItems(mixed, emby::SortKey::Random, true);
        assert(order() == before);                      // random keeps the order it came in
    }

    // --- bodies -------------------------------------------------------------
    {
        const auto body = nlohmann::json::parse(emby::authenticateBody("guest", ""));
        assert(body["Username"] == "guest" && body["Pw"] == "");
        const auto info = nlohmann::json::parse(emby::playbackInfoBody("u1"));
        assert(info["UserId"] == "u1");
        emby::Report report;
        report.itemId = "109672";
        report.mediaSourceId = "ms";
        report.playSessionId = "ps";
        report.positionTicks = 300000000;
        report.paused = true;
        report.eventName = "Pause";
        const auto progress = nlohmann::json::parse(emby::reportBody(report));
        assert(progress["ItemId"] == "109672" && progress["MediaSourceId"] == "ms" &&
               progress["PlaySessionId"] == "ps" && progress["PositionTicks"] == 300000000 &&
               progress["IsPaused"] == true && progress["PlayMethod"] == "DirectPlay" &&
               progress["CanSeek"] == true && progress["EventName"] == "Pause");
        report.eventName.clear();
        report.playSessionId.clear();
        const auto stopped = nlohmann::json::parse(emby::reportBody(report));
        assert(!stopped.contains("EventName") && !stopped.contains("PlaySessionId"));
    }

    // --- replies ------------------------------------------------------------
    const auto info = emby::parseServerInfo(
        R"json({"LocalAddresses":[],"RemoteAddresses":[],"ServerName":"SyntheticServer","Version":"4.11.0.3","Id":"0123456789abcdef0123456789abcdef"})json");
    assert(info && info->name == "SyntheticServer" && info->version == "4.11.0.3" &&
           info->id == "0123456789abcdef0123456789abcdef");
    assert(!emby::parseServerInfo("<html>not json</html>"));
    assert(!emby::parseServerInfo(R"json({"ServerName":"x"})json"));

    const auto auth = emby::parseAuthentication(
        R"json({"User":{"Name":"guest","ServerId":"0123","Id":"fedcba9876543210fedcba9876543210","HasPassword":false},"SessionInfo":{},"AccessToken":"tok123","ServerId":"0123"})json");
    assert(auth && auth->token == "tok123" && auth->userId == "fedcba9876543210fedcba9876543210" &&
           auth->userName == "guest" && auth->serverId == "0123");
    assert(!emby::parseAuthentication(R"json({"User":{"Name":"guest"}})json"));
    assert(!emby::parseAuthentication("Invalid user or password"));

    const auto list = emby::parseItems(
        R"json({"Items":[
          {"Name":"anime","Id":"79571","IsFolder":true,"Type":"CollectionFolder","CollectionType":"tvshows","UserData":{"PlaybackPositionTicks":0,"Played":false}},
          {"Name":"ようこそ","Id":"109672","SeriesName":"86 Eighty-Six","SeriesId":"109364","SeasonId":"109665","IndexNumber":12,"ParentIndexNumber":1,"RunTimeTicks":14200110000,"Type":"Episode","Container":"mp4","MediaType":"Video","UserData":{"PlaybackPositionTicks":300000000,"PlayCount":0,"Played":false}},
          {"Name":"86 Eighty-Six","Id":"109364","IsFolder":true,"Type":"Series","UserData":{"UnplayedItemCount":12,"Played":false}},
          {"Name":"Camera Roll","Id":"93465","IsFolder":true,"Type":"Folder"},
          {"Name":"holiday.jpg","Id":"555","Type":"Photo","MediaType":"Photo","Width":4000,"Height":3000,"ImageTags":{"Primary":"tag1"},"PrimaryImageAspectRatio":1.3333333,"DateCreated":"2026-09-26T03:21:58.3879358Z","Size":594140373},
          {"Name":"2024 trip","Id":"777","IsFolder":true,"Type":"Folder","PrimaryImageItemId":"778","PrimaryImageTag":"borrowed","PrimaryImageAspectRatio":1.7777}
        ],"TotalRecordCount":2447})json");
    assert(list && list->total == 2447 && list->items.size() == 6);
    const auto& view = list->items[0];
    assert(view.isFolder && view.type == "CollectionFolder" && view.collectionType == "tvshows");
    assert(emby::isBrowsable(view) && !emby::isPlayableVideo(view) && !emby::isPhoto(view));
    const auto& episode = list->items[1];
    assert(episode.name == "ようこそ" && episode.seriesName == "86 Eighty-Six" && episode.indexNumber == 12 &&
           episode.parentIndexNumber == 1 && episode.runTimeTicks == 14200110000 &&
           episode.positionTicks == 300000000 && !episode.played && episode.seasonId == "109665");
    assert(emby::isPlayableVideo(episode) && !emby::isBrowsable(episode));
    assert(emby::episodeCode(episode) == "S1E12");
    assert(emby::displayTitle(episode) == "86 Eighty-Six \xC2\xB7 S1E12 \xC2\xB7 ようこそ");
    assert(emby::progressLabel(episode) == "\xE2\x96\xB6 2%");
    const auto& series = list->items[2];
    assert(series.unplayedCount == 12 && emby::isBrowsable(series) && emby::displayTitle(series) == "86 Eighty-Six");
    assert(emby::progressLabel(series).empty());
    assert(emby::isBrowsable(list->items[3]) && !emby::isPlayableVideo(list->items[3]));
    const auto& photo = list->items[4];
    assert(emby::isPhoto(photo) && !emby::isPlayableVideo(photo) && !emby::isBrowsable(photo) &&
           photo.width == 4000 && photo.primaryImageTag == "tag1" && photo.dateCreated.rfind("2026-09-26", 0) == 0);
    assert(photo.primaryImageItemId.empty() && near(photo.primaryImageAspect, 1.3333333));
    assert(photo.size == 594140373 && list->items[1].size == 0);
    // A folder without a picture of its own borrows a child's: the key
    // names the child, so the server is asked for a picture it has.
    assert(list->items[5].isFolder && list->items[5].primaryImageTag == "borrowed" &&
           list->items[5].primaryImageItemId == "778" && near(list->items[5].primaryImageAspect, 1.7777));
    assert(emby::imageKey(list->items[5], 440) == "778|borrowed|440");
    emby::Item played;
    played.played = true;
    assert(emby::progressLabel(played) == "\xE2\x9C\x93");
    // /Items/Latest answers with a bare array.
    const auto latest = emby::parseItems(R"json([{"Id":"1","Name":"a","Type":"Movie"},{"Id":"2","Name":"b","Type":"Series","IsFolder":true}])json");
    assert(latest && latest->total == 2 && latest->items[1].isFolder);
    assert(!emby::parseItems(R"json({"NoItems":[]})json"));
    assert(!emby::parseItems("[1,2"));
    const auto single = emby::parseItem(R"json({"Name":"2k (109)","Id":"119982","Type":"Video","MediaType":"Video","RunTimeTicks":2625383330,"UserData":{"PlaybackPositionTicks":0,"Played":false}})json");
    assert(single && single->id == "119982" && emby::isPlayableVideo(*single));
    assert(!emby::parseItem(R"json({"Name":"no id"})json"));

    const auto playback = emby::parsePlaybackInfo(
        R"json({"MediaSources":[{"Protocol":"File","Id":"mediasource_110010","Container":"mkv","SupportsDirectPlay":true,"SupportsDirectStream":true,"RunTimeTicks":66000000000,"DefaultAudioStreamIndex":1,"MediaStreams":[
            {"Codec":"hevc","Type":"Video","Index":0,"IsDefault":true},
            {"Codec":"flac","Language":"jpn","DisplayTitle":"Japanese FLAC stereo","Type":"Audio","Index":1,"IsDefault":true,"Channels":2},
            {"Codec":"dts","Language":"jpn","DisplayTitle":"Japanese DTS-HD MA 5.1","Type":"Audio","Index":2,"Channels":6},
            {"Codec":"ass","DisplayTitle":"(ASS)","Title":"unibig5","Type":"Subtitle","Index":4,"IsExternal":true,"IsTextSubtitleStream":true,"SupportsExternalStream":true},
            {"Codec":"pgssub","Type":"Subtitle","Index":5}
        ]}],"PlaySessionId":"8b04d938baab4f07ad4347123c345b41"})json");
    assert(playback && playback->playSessionId == "8b04d938baab4f07ad4347123c345b41" && playback->sources.size() == 1);
    const auto* source = emby::directPlaySource(*playback);
    assert(source && source->id == "mediasource_110010" && source->container == "mkv" &&
           source->supportsDirectPlay && source->defaultAudioStreamIndex == 1 &&
           source->defaultSubtitleStreamIndex == -1 && source->streams.size() == 5);
    assert(source->streams[1].type == "Audio" && source->streams[1].channels == 2 &&
           emby::streamLabel(source->streams[1]) == "Japanese FLAC stereo");
    assert(source->streams[3].isTextSubtitle && source->streams[3].isExternal);
    assert(!source->streams[4].isTextSubtitle);
    // A file beside the video says its language, if at all, in its title:
    // what its name adds to the video's.
    assert(source->streams[3].title == "unibig5" && source->streams[1].title.empty());
    // An ASS stream is asked for as ASS, anything else as SRT.
    assert(emby::isAssSubtitleCodec("ASS") && emby::isAssSubtitleCodec("ssa") && !emby::isAssSubtitleCodec("subrip") &&
           !emby::isAssSubtitleCodec("pgssub"));
    assert(emby::subtitlePath("110010", "ms", 4, "ass") == "/Videos/110010/ms/Subtitles/4/Stream.ass");
    emby::MediaStream bare;
    bare.language = "eng";
    bare.codec = "aac";
    bare.channels = 6;
    assert(emby::streamLabel(bare) == "eng aac 6ch");
    assert(emby::isTextSubtitleCodec("SubRip") && emby::isTextSubtitleCodec("webvtt") && !emby::isTextSubtitleCodec("dvdsub"));
    assert(!emby::isTextSubtitleCodec("PGSSUB") && !emby::isTextSubtitleCodec("sup"));

    // Which videos of a list have subtitles: asked by id, for text codecs
    // only, each playable video once.
    assert(emby::subtitledItemsPath("u 1", {"100543", "109320"}) ==
           "/Users/u%201/Items?Ids=100543%2C109320&SubtitleCodecs=subrip%2Csrt%2Cass%2Cssa%2Cwebvtt%2Cvtt%2C"
           "mov_text%2Ctext%2Ctx3g%2Cttml%2Cmicrodvd%2Csami&EnableImages=false&EnableUserData=false&Limit=2");
    {
        std::vector<emby::Item> listed(5);
        listed[0].id = "a";
        listed[0].type = "Movie";
        listed[1].id = "folder";
        listed[1].type = "Folder";
        listed[1].isFolder = true;
        listed[2].id = "b";
        listed[2].type = "Episode";
        listed[3].id = "a";   // the same video twice, as a playlist may hold it
        listed[3].type = "Movie";
        listed[4].id = "c";
        listed[4].type = "Video";
        std::unordered_map<std::string, bool> known{{"b", false}};
        std::unordered_set<std::string> asking{"c"};
        assert((emby::subtitleQuestions(listed, known, asking, false) == std::vector<std::string>{"a"}));
        assert((emby::subtitleQuestions(listed, known, asking, true) == std::vector<std::string>{"a", "b"}));
        assert((emby::subtitleQuestions(listed, {}, {}, false) == std::vector<std::string>{"a", "b", "c"}));
    }
    {
        std::vector<std::string> ids;
        for (int i = 0; i < 250; ++i) ids.push_back(std::to_string(100000 + i));
        const auto batches = emby::subtitleBatches(ids);
        assert(batches.size() == 3 && batches[0].size() == 100 && batches[2].size() == 50 && batches[2].back() == "100249");
        // Long ids: a batch ends before its list passes the length.
        const auto guids = emby::subtitleBatches(std::vector<std::string>(100, std::string(32, 'f')));
        assert(guids.size() == 3 && guids[0].size() == 45 && guids[2].size() == 10);
        assert(emby::subtitleBatches({std::string(2000, 'x'), "1"}).size() == 2);   // one too long goes alone
        assert(emby::subtitleBatches({}).empty());
    }
    const auto noSource = emby::parsePlaybackInfo(R"json({"MediaSources":[],"ErrorCode":"NoCompatibleStream"})json");
    assert(noSource && noSource->errorCode == "NoCompatibleStream" && emby::directPlaySource(*noSource) == nullptr);
    // A transcode-only source is still returned so the caller can say why.
    const auto transcodeOnly = emby::parsePlaybackInfo(R"json({"MediaSources":[{"Id":"a","SupportsDirectPlay":false,"SupportsDirectStream":false,"TranscodingUrl":"/x.m3u8"}]})json");
    assert(transcodeOnly && emby::directPlaySource(*transcodeOnly)->transcodingUrl == "/x.m3u8");

    // --- locators -----------------------------------------------------------
    const std::wstring locator = emby::formatLocator("0123", "109672");
    assert(locator == L"emby://0123/109672" && emby::isLocator(locator));
    const auto parsed = emby::parseLocator(locator);
    assert(parsed && parsed->serverId == "0123" && parsed->itemId == "109672");
    assert(!emby::parseLocator(L"emby://0123") && !emby::parseLocator(L"emby:///1") &&
           !emby::parseLocator(L"emby://a/") && !emby::parseLocator(L"C:\\video.mp4") &&
           !emby::isLocator(L"emby://") && !emby::isLocator(L"C:\\emby\\a.mp4"));

    // --- neighbours ---------------------------------------------------------
    std::vector<emby::Item> episodes(3);
    episodes[0].id = "a"; episodes[1].id = "b"; episodes[2].id = "c";
    assert(emby::adjacentIndex(episodes, "b", 1) == 2 && emby::adjacentIndex(episodes, "b", -1) == 0);
    assert(emby::adjacentIndex(episodes, "c", 1) == -1 && emby::adjacentIndex(episodes, "a", -1) == -1);
    assert(emby::adjacentIndex(episodes, "zz", 1) == -1);

    // --- auth file ----------------------------------------------------------
    emby::StoredAuth stored;
    stored.serverUrl = "http://192.168.1.2:8096";
    stored.serverId = "0123";
    stored.serverName = "My \"Test\" Server";
    stored.userId = "fedc";
    stored.userName = "访客";
    stored.deviceId = "dev42";
    stored.protectedToken = "AQAAANCMnd8BFdERjHoAwE/Cl+sBAAAA==";
    std::wstringstream file;
    assert(emby::writeStoredAuth(file, stored));
    emby::StoredAuth loaded;
    assert(emby::readStoredAuth(file, loaded));
    assert(loaded.serverUrl == stored.serverUrl && loaded.serverId == stored.serverId &&
           loaded.serverName == stored.serverName && loaded.userId == stored.userId &&
           loaded.userName == stored.userName && loaded.deviceId == stored.deviceId &&
           loaded.protectedToken == stored.protectedToken);
    std::wstringstream wrongVersion(L"QEMBY 2\nserver \"a\" \"b\" \"c\"\n");
    assert(!emby::readStoredAuth(wrongVersion, loaded));
    std::wstringstream truncated(L"QEMBY 1\nserver \"http://x\" \"b\" \"c\"\nuser \"u\" \"n\"\n");
    assert(!emby::readStoredAuth(truncated, loaded));
    std::wstringstream noServer(L"QEMBY 1\nserver \"\" \"b\" \"c\"\nuser \"u\" \"n\"\ndevice \"d\"\ntoken \"t\"\n");
    assert(!emby::readStoredAuth(noServer, loaded));
}

// Synthetic edge cases for the Movie/Series/Season/Episode BaseItemDto
// metadata documented by Emby. These fixtures never contact a server.
void movieTvMetadataTests() {
    const auto titles = emby::parseItems(R"json({"Items":[
        {"Id":"movie","Name":"Dune","Type":"Movie","ProductionYear":2021,"ImageTags":{"Primary":"poster"}},
        {"Id":"series","Name":"Example show","Type":"Series","ProductionYear":2018,"IsFolder":true},
        {"Id":"specials","Type":"Season","IndexNumber":0,"SeriesId":"series"},
        {"Id":"season3","Type":"Season","IndexNumber":3,"SeriesId":"series"},
        {"Id":"named-season","Name":"Book One","Type":"Season","IndexNumber":1},
        {"Id":"unknown-season","Type":"Season"},
        {"Id":"episode","Name":"The double","Type":"Episode","SeriesId":"series","SeriesName":"Example show",
         "SeasonId":"season3","ParentId":"season3","ParentIndexNumber":3,"IndexNumber":8,"IndexNumberEnd":9}
    ],"TotalRecordCount":7})json");
    assert(titles && titles->items.size() == 7 && titles->total == 7);
    assert(titles->items[0].productionYear == 2021 && emby::displayTitle(titles->items[0]) == "Dune (2021)");
    assert(emby::isPlayableVideo(titles->items[0]) && !emby::isBrowsable(titles->items[0]));
    assert(emby::imageKey(titles->items[0], 300) == "movie|poster|300");
    assert(emby::displayTitle(titles->items[1]) == "Example show (2018)");
    assert(emby::isBrowsable(titles->items[1]) && !emby::isPlayableVideo(titles->items[1]));
    assert(emby::displayTitle(titles->items[2]) == "Specials" && emby::isBrowsable(titles->items[2]));
    assert(emby::displayTitle(titles->items[3]) == "Season 3" && emby::isBrowsable(titles->items[3]));
    assert(emby::displayTitle(titles->items[4]) == "Book One");
    assert(emby::displayTitle(titles->items[5]) == "Season");
    const auto& episode = titles->items[6];
    assert(episode.seriesId == "series" && episode.seasonId == "season3" && episode.parentId == "season3");
    assert(emby::episodeCode(episode) == "S3E8\xE2\x80\x93" "E9");
    assert(emby::displayTitle(episode) == "Example show \xC2\xB7 S3E8\xE2\x80\x93" "E9 \xC2\xB7 The double");

    auto incomplete = episode;
    incomplete.name.clear();
    assert(emby::displayTitle(incomplete) == "Example show \xC2\xB7 S3E8\xE2\x80\x93" "E9");
    incomplete.seriesName.clear();
    assert(emby::displayTitle(incomplete) == "S3E8\xE2\x80\x93" "E9");
    incomplete.parentIndexNumber = -1;
    assert(emby::episodeCode(incomplete) == "E8\xE2\x80\x93" "E9");
    incomplete.indexNumberEnd = incomplete.indexNumber;
    assert(emby::episodeCode(incomplete) == "E8");
    incomplete.indexNumberEnd = 4;
    assert(emby::episodeCode(incomplete) == "E8");
    incomplete.indexNumber = -1;
    assert(emby::episodeCode(incomplete).empty() && emby::displayTitle(incomplete) == "Episode");
    incomplete.name = "Named episode";
    assert(emby::displayTitle(incomplete) == "Named episode");
    incomplete.name.clear();
    incomplete.parentIndexNumber = 0;
    incomplete.indexNumber = 0;
    incomplete.indexNumberEnd = -1;
    assert(emby::displayTitle(incomplete) == "S0E0");

    const auto missing = emby::parseItem(R"json({"Id":"missing","Type":"Movie"})json");
    assert(missing && missing->productionYear == 0 && missing->indexNumberEnd == -1);
    assert(emby::displayTitle(*missing) == "Movie");
    auto unnamedSeries = *missing;
    unnamedSeries.type = "Series";
    unnamedSeries.productionYear = 2020;
    assert(emby::displayTitle(unnamedSeries) == "Series (2020)");
    unnamedSeries.productionYear = 10000;
    assert(emby::displayTitle(unnamedSeries) == "Series");

    // Bad optional metadata cannot wrap, round or turn into a made-up year
    // or episode number. Null and wrong types are treated as absent.
    const auto invalid = nlohmann::json::parse(
        R"json([null,true,-1,1.5,"2021",[],{},2147483648,18446744073709551615,1e100])json");
    for (const auto& value : invalid) {
        nlohmann::json dto{{"Id", "bad"}, {"Type", "Episode"}, {"ProductionYear", value},
                           {"IndexNumber", value}, {"ParentIndexNumber", value}, {"IndexNumberEnd", value}};
        const auto parsed = emby::parseItem(dto.dump());
        assert(parsed && parsed->productionYear == 0 && parsed->indexNumber == -1 &&
               parsed->parentIndexNumber == -1 && parsed->indexNumberEnd == -1);
        assert(emby::displayTitle(*parsed) == "Episode");
    }
    for (const int year : {0, 10000}) {
        const auto parsed = emby::parseItem(nlohmann::json{{"Id", "year"}, {"ProductionYear", year}}.dump());
        assert(parsed && parsed->productionYear == 0);
    }
    for (const int year : {1, 9999}) {
        const auto parsed = emby::parseItem(nlohmann::json{{"Id", "year"}, {"ProductionYear", year}}.dump());
        assert(parsed && parsed->productionYear == year);
    }

    // Household video, photo and playlist names retain their existing form.
    for (const char* type : {"Video", "Photo", "Playlist"}) {
        emby::Item unchanged;
        unchanged.name = "2024 trip";
        unchanged.type = type;
        unchanged.productionYear = 2024;
        assert(emby::displayTitle(unchanged) == "2024 trip");
    }

    // A hierarchy page keeps the server's sequence: no SortBy is added.
    assert(emby::seasonsPath("show /", "user /", "ParentId,ChildCount", 300, 300) ==
           "/Shows/show%20%2F/Seasons?UserId=user%20%2F&Fields=ParentId%2CChildCount&StartIndex=300&Limit=300");
    assert(emby::episodesPath("show /", "user /", "season /", "ParentId,Size", 600, 300) ==
           "/Shows/show%20%2F/Episodes?UserId=user%20%2F&SeasonId=season%20%2F"
           "&Fields=ParentId%2CSize&StartIndex=600&Limit=300");
    assert(emby::episodesPath("show", "user", "", "", -20, 1) ==
           "/Shows/show/Episodes?UserId=user&StartIndex=0&Limit=1");
    assert(emby::seasonsPath("show", "user", "", -20, 1) ==
           "/Shows/show/Seasons?UserId=user&StartIndex=0&Limit=1");
    assert(emby::episodesPath("show", "user", "", "", 300, 0) == "/Shows/show/Episodes?UserId=user");
    assert(emby::seasonsPath("show", "user", "", 300, 0) == "/Shows/show/Seasons?UserId=user");
    assert(emby::resumePath("user /", 0, "ParentId") ==
           "/Users/user%20%2F/Items/Resume?MediaTypes=Video&Limit=1&Fields=ParentId");
    assert(emby::resumePath("user", 10, "") == "/Users/user/Items/Resume?MediaTypes=Video&Limit=10");

    emby::ListPage hierarchy;
    hierarchy.itemType = "Series";
    assert(!emby::listTakesFlat(hierarchy));
    hierarchy.itemType = "Season";
    assert(!emby::listTakesFlat(hierarchy));

    // sameListing reports changes to the title metadata in a refreshed page.
    const std::vector<emby::Item> shown{episode};
    auto fresh = shown;
    fresh[0].seriesName = "Renamed show";
    assert(!emby::sameListing(shown, fresh));
    fresh = shown; fresh[0].indexNumber = 10;
    assert(!emby::sameListing(shown, fresh));
    fresh = shown; fresh[0].parentIndexNumber = 4;
    assert(!emby::sameListing(shown, fresh));
    fresh = shown; fresh[0].indexNumberEnd = 10;
    assert(!emby::sameListing(shown, fresh));
    fresh = shown; fresh[0].productionYear = 2020;
    assert(!emby::sameListing(shown, fresh));
}

void detailMetadataTests() {
    const auto item = emby::parseItem(R"json({
        "Id":"film","Name":"Example film","Type":"Movie","OriginalTitle":"Original name",
        "Overview":"A complete synopsis.\nSecond paragraph.","OfficialRating":"PG-13",
        "CommunityRating":8.25,"CriticRating":92,
        "Genres":["Drama",null,7,"Science Fiction",""],"Taglines":["A tagline."],
        "Studios":[{"Id":12,"Name":"Example Studio"},null,{"Name":false}],
        "People":[{"Id":"director","Name":"Example Director","Type":"Director"},
                  {"Id":42,"Name":"Example Actor","Type":"Actor","Role":"A character","PrimaryImageTag":"face"},
                  {"Name":9,"Type":"Actor"},null],
        "ImageTags":{"Primary":"poster","Logo":"own-logo"},
        "BackdropImageTags":[null,"own-wide"],"ParentBackdropItemId":"series",
        "ParentBackdropImageTags":["parent-wide"],"ParentLogoItemId":"series","ParentLogoImageTag":"parent-logo",
        "MediaStreams":[{"Type":"Video","Codec":"hevc","Width":3840,"Height":2160,"VideoRange":"HDR10"},
                        {"Type":"Audio","Codec":"aac","Channels":2,"Language":"eng"},
                        {"Type":"Audio","Codec":"flac","Channels":6,"SampleRate":48000,"Language":"jpn","IsDefault":true},
                        {"Type":"Subtitle","Codec":"ass","DisplayTitle":"Subtitle"}]
    })json");
    assert(item && item->overview == "A complete synopsis.\nSecond paragraph." && item->officialRating == "PG-13");
    assert(item->originalTitle == "Original name" && item->communityRating && near(*item->communityRating, 8.25));
    assert(item->criticRating && near(*item->criticRating, 92.0));
    assert(item->genres == (std::vector<std::string>{"Drama", "Science Fiction"}));
    assert(item->taglines == (std::vector<std::string>{"A tagline."}));
    assert(item->studios == (std::vector<std::string>{"Example Studio"}));
    assert(item->people.size() == 2 && item->people[0].type == "Director" && item->people[0].id == "director");
    assert(item->people[1].name == "Example Actor" && item->people[1].type == "Actor" &&
           item->people[1].id == "42" && item->people[1].role == "A character" && item->people[1].primaryImageTag == "face");
    assert(item->videoInfo == "HEVC \xC2\xB7 3840\xC3\x97" "2160 \xC2\xB7 HDR10");
    assert(item->audioInfo == "FLAC \xC2\xB7 6ch \xC2\xB7 48000 Hz \xC2\xB7 jpn");
    assert(item->backdropImageTags.size() == 2 && item->backdropImageTags[0].empty() && item->backdropImageTags[1] == "own-wide");

    const auto malformed = emby::parseItem(R"json({"Id":"bad","Overview":123,"OfficialRating":false,
        "OriginalTitle":[],"CommunityRating":"8","CriticRating":-1,"Genres":"Drama","Taglines":null,
        "Studios":["Not a studio DTO",{}],"People":[{},false],"BackdropImageTags":[false,123],
        "MediaStreams":[{"Type":"Video","Width":1e100,"Height":2160},
                        {"Type":"Audio","Channels":2147483648,"SampleRate":-1}]})json");
    assert(malformed && malformed->overview.empty() && malformed->officialRating.empty() && malformed->originalTitle.empty());
    assert(!malformed->communityRating && !malformed->criticRating && malformed->genres.empty() && malformed->taglines.empty());
    assert(malformed->studios.empty() && malformed->people.empty() && malformed->videoInfo.empty() && malformed->audioInfo.empty());
    assert(malformed->backdropImageTags.size() == 2 && malformed->backdropImageTags[0].empty() && malformed->backdropImageTags[1].empty());
    for (const auto& value : nlohmann::json::parse(R"json([null,true,"9",-1,11,1e100])json")) {
        const auto parsed = emby::parseItem(nlohmann::json{{"Id", "score"}, {"CommunityRating", value}}.dump());
        assert(parsed && !parsed->communityRating);
    }
    for (const double score : {0.0, 10.0}) {
        const auto parsed = emby::parseItem(nlohmann::json{{"Id", "score"}, {"CommunityRating", score}}.dump());
        assert(parsed && parsed->communityRating && near(*parsed->communityRating, score));
    }
    const auto highCritic = emby::parseItem(R"json({"Id":"score","CriticRating":101})json");
    assert(highCritic && !highCritic->criticRating);

    // Limits omit oversized text without splitting UTF-8. Array limits
    // count input positions, including malformed entries, and preserve image slots.
    nlohmann::json bounded{{"Id", "bounded"}, {"Overview", std::string(emby::kMaximumOverviewBytes + 1, 'x')}};
    bounded["Genres"] = nlohmann::json::array();
    bounded["BackdropImageTags"] = nlohmann::json::array();
    bounded["People"] = nlohmann::json::array();
    for (std::size_t i = 0; i <= emby::kMaximumMetadataEntries; ++i) {
        bounded["Genres"].push_back("Genre " + std::to_string(i));
        bounded["People"].push_back(nlohmann::json{{"Name", "Person " + std::to_string(i)}, {"Type", "Actor"}});
        bounded["BackdropImageTags"].push_back("tag" + std::to_string(i));
    }
    const auto limited = emby::parseItem(bounded.dump());
    assert(limited && limited->overview.empty() && limited->genres.size() == emby::kMaximumMetadataEntries);
    assert(limited->people.size() == emby::kMaximumMetadataEntries && limited->backdropImageTags.size() == emby::kMaximumBackdropImages);
    assert(!emby::parseItem(std::string(emby::kMaximumItemReplyBytes + 1, ' ')));
    const auto unicode = emby::parseItem(R"json({"Id":"unicode","Overview":"剧情简介 🎬","Genres":["剧情"],"People":[{"Name":"导演","Type":"Director"}]})json");
    assert(unicode && unicode->overview == "剧情简介 🎬" && unicode->genres[0] == "剧情" && unicode->people[0].name == "导演");

    // Standard item MediaSources are sufficient; browsing does not need a
    // PlaybackInfo request. Pick one version and preserve top-level information.
    const auto sourceInfo = emby::parseItem(R"json({"Id":"source","MediaStreams":[{"Type":"Video","Codec":"h264"}],
        "MediaSources":[{"MediaStreams":[{"Type":"Video","Codec":"hevc"},{"Type":"Audio","Codec":"aac","Channels":2}]},
                        {"MediaStreams":[{"Type":"Audio","Codec":"flac","Channels":6}]}]})json");
    assert(sourceInfo && sourceInfo->videoInfo == "H264" && sourceInfo->audioInfo == "AAC \xC2\xB7 2ch");
    const auto onlySource = emby::parseItem(R"json({"Id":"source","MediaSources":[{},
        {"MediaStreams":[{"Type":"Video","Codec":"vp9","Width":1920,"Height":1080,"VideoRange":"SDR"}]}]})json");
    assert(onlySource && onlySource->videoInfo == "VP9 \xC2\xB7 1920\xC3\x97" "1080" && onlySource->audioInfo.empty());

    assert(emby::itemDetailsPath("u /", "film /") ==
           "/Users/u%20%2F/Items/film%20%2F");
    assert(emby::ancestorsPath("film /", "u /") == "/Items/film%20%2F/Ancestors?UserId=u%20%2F");
    const auto ancestors = emby::parseAncestorsArray(R"json([{"Id":"folder","Type":"Folder"},
        {"Id":"library","Type":"CollectionFolder","CollectionType":"movies"},null,{},
        {"Id":"library","Type":"CollectionFolder"}])json");
    assert(ancestors && ancestors->size() == 3);
    const auto libraries = emby::parseItems(R"json([{"Id":"library","Type":"CollectionFolder"},
        {"Id":"virtual","Type":"UserView"},{"Id":"folder","Type":"Folder"}])json");
    assert(libraries && emby::uniqueAncestorLibraryId(*ancestors, libraries->items) == "library");
    auto ambiguous = *ancestors;
    ambiguous.push_back(libraries->items[1]);
    assert(emby::uniqueAncestorLibraryId(ambiguous, libraries->items).empty());
    assert(emby::uniqueAncestorLibraryId({}, libraries->items).empty());
    assert(emby::uniqueAncestorLibraryId(*ancestors, {}).empty());
    assert(emby::parseAncestorsArray("[]") && emby::parseAncestorsArray("[]")->empty());
    assert(!emby::parseAncestorsArray(R"json({"Items":[]})json") && !emby::parseAncestorsArray("null"));
    assert(!emby::parseAncestorsArray("[broken") && !emby::parseAncestorsArray(std::string(emby::kMaximumItemReplyBytes + 1, ' ')));
    nlohmann::json longAncestors = nlohmann::json::array();
    for (std::size_t i = 0; i <= emby::kMaximumMetadataEntries; ++i) longAncestors.push_back(nlohmann::json{{"Id", i}});
    assert(!emby::parseAncestorsArray(longAncestors.dump()));

    // Primary keys retain their existing form. Typed keys carry a separate
    // namespace and the exact image index, including inherited artwork.
    assert(emby::imageKey(*item, 300) == "film|poster|300");
    const auto primary = emby::parseImageKey(emby::imageKey(*item, 300));
    assert(primary && primary->type == emby::ImageType::Primary && primary->index == 0);
    assert(emby::imagePath(*primary) == emby::imagePath("film", 300, "poster"));
    assert(emby::backdropImageKey(*item, 1280).empty());  // missing own slot zero stays missing
    const auto ownBackdrop = emby::parseImageKey(emby::backdropImageKey(*item, 1280, 1));
    assert(ownBackdrop && ownBackdrop->itemId == "film" && ownBackdrop->tag == "own-wide" &&
           ownBackdrop->type == emby::ImageType::Backdrop && ownBackdrop->index == 1);
    assert(emby::imagePath(*ownBackdrop) == "/Items/film/Images/Backdrop/1?quality=90&AutoOrient=true&maxWidth=1280&tag=own-wide");
    auto borrowed = *item;
    borrowed.backdropImageTags.clear(); borrowed.logoImageTag.clear();
    const auto inherited = emby::parseImageKey(emby::backdropImageKey(borrowed, 1280));
    assert(inherited && inherited->itemId == "series" && inherited->tag == "parent-wide" && inherited->index == 0);
    const auto ownLogo = emby::parseImageKey(emby::logoImageKey(*item, 400));
    const auto parentLogo = emby::parseImageKey(emby::logoImageKey(borrowed, 400));
    assert(ownLogo && ownLogo->type == emby::ImageType::Logo && ownLogo->itemId == "film" && ownLogo->tag == "own-logo");
    assert(parentLogo && parentLogo->itemId == "series" && parentLogo->tag == "parent-logo");
    assert(emby::imagePath(*parentLogo) == "/Items/series/Images/Logo/0?quality=90&AutoOrient=true&maxWidth=400&tag=parent-logo");
    assert(emby::imagePath("film /", 300, "tag /", emby::ImageType::Backdrop, 2) ==
           "/Items/film%20%2F/Images/Backdrop/2?quality=90&AutoOrient=true&maxWidth=300&tag=tag%20%2F");
    assert(emby::logoImageKey(emby::Item{}, 400).empty() && emby::backdropImageKey(emby::Item{}, 400).empty());
    assert(emby::backdropImageKey(*item, 1280, -1).empty() && emby::backdropImageKey(*item, 1280, 32).empty());
    for (const char* key : {"emby-image|Unknown|0|film|tag|400", "emby-image|Backdrop|-1|film|tag|400",
                           "emby-image|Backdrop|32|film|tag|400", "emby-image|Logo|1|film|tag|400",
                           "emby-image|Backdrop|0|film|tag|400junk", "film|tag|400junk", "film||400",
                           "emby-image|Backdrop|0||tag|400", "emby-image|Backdrop|0|film|tag|0"}) {
        assert(!emby::parseImageKey(key));
    }
}

// Playlists and the pages the browser lists with /Users/{id}/Items. The
// replies are what an Emby 4.11.0.4 server answered on 2026-09-29, trimmed
// to the keys the parsers read and with the titles replaced.
void playlistAndListTests() {
    // --- a playlist among the items of the playlists library ----------------
    const auto playlists = emby::parseItems(R"json({"Items":[
        {"Name":"Evening","Id":"119996","DateCreated":"2026-09-20T09:49:53.0000000Z","RunTimeTicks":797109933340,
         "IsFolder":true,"ParentId":"119995","Type":"Playlist","ChildCount":403,
         "UserData":{"PlaybackPositionTicks":0,"PlayCount":0,"IsFavorite":false,"Played":false},
         "PrimaryImageAspectRatio":1,"ImageTags":{"Primary":"2c44"}},
        {"Name":"Empty","Id":"39243","RunTimeTicks":13472066660,"IsFolder":true,"ParentId":"39242",
         "Type":"Playlist","ChildCount":0,"ImageTags":{"Primary":"e871"}}],"TotalRecordCount":2})json");
    assert(playlists && playlists->items.size() == 2 && playlists->total == 2);
    const auto& evening = playlists->items[0];
    assert(emby::isPlaylist(evening) && emby::isBrowsable(evening) && !emby::isPlayableVideo(evening));
    assert(evening.childCount == 403 && evening.runTimeTicks == 797109933340 && evening.playlistItemId.empty());
    assert(playlists->items[1].childCount == 0);

    // --- its entries, in its own order ----------------------------------------
    // The second and fourth are the same video put in twice; only the
    // place in the playlist tells them apart.
    const auto entries = emby::parseItems(R"json({"Items":[
        {"Name":"Clip one","Id":"105141","PlaylistItemId":"1","Container":"mp4","RunTimeTicks":558716670,
         "Size":169803653,"IsFolder":false,"ParentId":"102534","Type":"Video",
         "UserData":{"PlaybackPositionTicks":0,"PlayCount":3,"Played":true},
         "ImageTags":{"Primary":"9ae6"},"MediaType":"Video"},
        {"Name":"Clip two","Id":"105151","PlaylistItemId":"2","RunTimeTicks":761383330,"IsFolder":false,
         "ParentId":"102534","Type":"Video","MediaType":"Video"},
        {"Name":"Clip three","Id":"105192","PlaylistItemId":"3","RunTimeTicks":917200000,"IsFolder":false,
         "ParentId":"102534","Type":"Video","MediaType":"Video"},
        {"Name":"Clip two","Id":"105151","PlaylistItemId":"4","RunTimeTicks":761383330,"IsFolder":false,
         "ParentId":"102534","Type":"Video","MediaType":"Video"}],"TotalRecordCount":403})json");
    assert(entries && entries->items.size() == 4 && entries->total == 403);
    assert(entries->items[0].playlistItemId == "1" && entries->items[3].playlistItemId == "4");
    assert(entries->items[0].childCount == -1 && entries->items[0].played);
    assert(emby::isPlayableVideo(entries->items[1]) && !emby::isPlaylist(entries->items[1]));
    // The entry playing is found by its place; without one, the first with the id.
    assert(emby::entryIndex(entries->items, "105151") == 1);
    assert(emby::entryIndex(entries->items, "105151", "4") == 3);
    assert(emby::entryIndex(entries->items, "105151", "2") == 1);
    assert(emby::entryIndex(entries->items, "105151", "99") == 1);
    assert(emby::entryIndex(entries->items, "105192", "3") == 2);
    assert(emby::entryIndex(entries->items, "nope") == -1 && emby::entryIndex({}, "105141") == -1);

    // --- what each page asks for ------------------------------------------------
    const std::string fields = "ParentId,Size";
    emby::ListArrangement byName;
    emby::ListArrangement played;
    played.sort = emby::SortKey::LastPlayed;
    played.descending = true;

    // A playlist sends no SortBy at all, whatever the browser's order is:
    // the server then answers in the playlist's own order. (SortOrder alone
    // does not reverse it; /Playlists/{id}/Items ignores Filters.)
    emby::ListPage playlist;
    playlist.kind = emby::ListKind::Playlist;
    playlist.id = "119996";
    playlist.itemType = "Playlist";
    assert(emby::listInOwnOrder(playlist) && !emby::listTakesFlat(playlist));
    assert(emby::itemsPath("u1", emby::listQuery(playlist, played, 0, 1000, fields)) ==
           "/Users/u1/Items?ParentId=119996&Fields=ParentId%2CSize&StartIndex=0&Limit=1000");
    // Flat means nothing to a playlist; unplayed does.
    emby::ListArrangement flatUnplayed = played;
    flatUnplayed.flat = true;
    flatUnplayed.unplayed = true;
    assert(emby::itemsPath("u1", emby::listQuery(playlist, flatUnplayed, 1000, 1000, fields)) ==
           "/Users/u1/Items?ParentId=119996&Filters=IsUnplayed&Fields=ParentId%2CSize&StartIndex=1000&Limit=1000");
    // Given up for one of the browser's orders, it sorts like any list of
    // videos: no folders to put first.
    playlist.ownOrder = false;
    assert(!emby::listInOwnOrder(playlist));
    assert(emby::itemsPath("u1", emby::listQuery(playlist, played, 0, 1000, fields)) ==
           "/Users/u1/Items?ParentId=119996&SortBy=DatePlayed&SortOrder=Descending"
           "&Fields=ParentId%2CSize&StartIndex=0&Limit=1000");

    // A home-video library by folder, and with its folders left out.
    emby::ListPage home;
    home.kind = emby::ListKind::Library;
    home.id = "47096";
    home.collectionType = "homevideos";
    assert(emby::listTakesFlat(home) && !emby::listInOwnOrder(home));
    assert(emby::itemsPath("u1", emby::listQuery(home, byName, 0, 300, fields)) ==
           "/Users/u1/Items?ParentId=47096&SortBy=IsFolder%2CSortName&SortOrder=Ascending%2CAscending"
           "&Fields=ParentId%2CSize&StartIndex=0&Limit=300");
    emby::ListArrangement flat = played;
    flat.flat = true;
    assert(emby::itemsPath("u1", emby::listQuery(home, flat, 300, 300, fields)) ==
           "/Users/u1/Items?ParentId=47096&IncludeItemTypes=Video%2CPhoto%2CMovie%2CEpisode&Recursive=true"
           "&SortBy=DatePlayed&SortOrder=Descending&Fields=ParentId%2CSize&StartIndex=300&Limit=300");
    // A library without a type of its own is walked the same way.
    emby::ListPage mixed = home;
    mixed.collectionType.clear();
    assert(emby::listTakesFlat(mixed));

    // A folder inside it takes the choice too; a box set lists what it holds.
    emby::ListPage folder;
    folder.id = "101947";
    folder.itemType = "Folder";
    assert(emby::listTakesFlat(folder));
    assert(emby::listQuery(folder, flat, 0, 300, fields).recursive);
    emby::ListPage boxSet = folder;
    boxSet.itemType = "BoxSet";
    assert(!emby::listTakesFlat(boxSet));
    const auto boxSetQuery = emby::listQuery(boxSet, flat, 0, 300, fields);
    assert(!boxSetQuery.recursive && boxSetQuery.includeTypes.empty() &&
           boxSetQuery.sortBy == "IsFolder,DatePlayed");

    // Series and movies are listed by type whatever the folders are, and the
    // libraries of box sets and of playlists list those: flat is not theirs.
    emby::ListPage shows = home;
    shows.collectionType = "tvshows";
    emby::ListPage movies = home;
    movies.collectionType = "movies";
    emby::ListPage boxSets = home;
    boxSets.collectionType = "boxsets";
    emby::ListPage playlistLibrary = home;
    playlistLibrary.collectionType = "playlists";
    assert(!emby::listTakesFlat(shows) && !emby::listTakesFlat(movies) && !emby::listTakesFlat(boxSets) &&
           !emby::listTakesFlat(playlistLibrary));
    const auto showsQuery = emby::listQuery(shows, flat, 0, 300, fields);
    assert(showsQuery.includeTypes == "Series" && showsQuery.recursive && showsQuery.sortBy == "DatePlayed");
    assert(emby::listQuery(movies, byName, 0, 300, fields).includeTypes == "Movie");
    const auto playlistLibraryQuery = emby::listQuery(playlistLibrary, flat, 0, 300, fields);
    assert(!playlistLibraryQuery.recursive && playlistLibraryQuery.includeTypes.empty() &&
           playlistLibraryQuery.sortBy == "IsFolder,DatePlayed" &&
           playlistLibraryQuery.sortOrder == "Ascending,Descending");
    assert(!emby::listQuery(boxSets, flat, 0, 300, fields).recursive);
    // Random never carries an order, flat or not.
    emby::ListArrangement random = flat;
    random.sort = emby::SortKey::Random;
    const auto randomQuery = emby::listQuery(home, random, 0, 300, fields);
    assert(randomQuery.sortBy == "Random" && randomQuery.sortOrder.empty() && randomQuery.recursive);
}

void playbackQueueTests() {
    const auto item = [](const char* id, const char* type = "Video", const char* entry = "") {
        emby::Item value;
        value.id = id;
        value.name = id;
        value.type = type;
        value.playlistItemId = entry;
        return value;
    };
    auto folder = item("folder");
    folder.isFolder = true;
    const std::vector<emby::Item> mixed{folder, item("a", "Movie", "1"), item("audio", "Audio"),
                                      item("photo", "Photo", "2"), item("a", "Episode", "3")};
    auto queue = emby::takeQueue(mixed, 4, "playlist:p1");
    assert(queue.items.size() == 3 && queue.cursor == 2 && queue.page == "playlist:p1");
    assert(queue.items[0].id == "a" && queue.items[1].id == "photo" && queue.items[2].playlistItemId == "3");
    assert(emby::takeQueue(mixed, 3, "photos").cursor == 1);
    assert(emby::takeQueue(mixed, 0, "page").cursor == -1 && emby::takeQueue(mixed, 2, "page").cursor == -1);
    assert(emby::takeQueue(mixed, -1, "page").cursor == -1 && emby::takeQueue(mixed, 99, "page").cursor == -1);
    assert(emby::takeQueue({}, 0, "empty").items.empty());

    // The same video occurs twice; the pane's cursor owns its playlist entry.
    assert(emby::queueIndex(queue, "a") == 2 && emby::queueIndex(queue, "photo") == 1);
    assert(emby::queueIndex(queue, "missing") == -1 && emby::queueIndex(queue, "") == -1);
    auto invalidCursor = queue;
    invalidCursor.cursor = 99;
    assert(emby::queueIndex(invalidCursor, "a") == 0);
    emby::reorderQueue(invalidCursor, [](auto& values) { std::reverse(values.begin(), values.end()); });
    assert(invalidCursor.cursor == -1);

    const auto otherPane = queue;
    emby::reorderQueue(queue, [](auto& values) { std::reverse(values.begin(), values.end()); });
    assert(queue.cursor == 0 && queue.items[0].playlistItemId == "3" && queue.page == "playlist:p1");
    assert(otherPane.cursor == 2 && otherPane.items[2].playlistItemId == "3");
    // Removing the current entry does not silently choose the other occurrence.
    auto removed = queue;
    emby::reorderQueue(removed, [](auto& values) { values.erase(values.begin()); });
    assert(removed.cursor == -1 && removed.items.back().id == "a" && removed.items.back().playlistItemId == "1");

    queue = otherPane;
    auto expanded = queue.items;
    expanded.insert(expanded.begin(), item("new"));
    expanded.insert(expanded.begin() + 3, folder);
    expanded.push_back(item("last"));
    expanded[1].name = "Fresh title";
    assert(emby::adoptQueue(queue, expanded, "a", "playlist:p1"));
    assert(queue.items.size() == 5 && queue.cursor == 3 && queue.items[3].playlistItemId == "3");
    assert(queue.items[1].name == "Fresh title");
    assert(otherPane.items.size() == 3 && otherPane.items[0].name == "a");
    const auto held = queue;
    const auto unchanged = [&] {
        assert(queue.cursor == held.cursor && queue.page == held.page && emby::sameListing(queue.items, held.items));
    };
    assert(!emby::adoptQueue(queue, expanded, "a", "playlist:other"));
    assert(!emby::adoptQueue(queue, expanded, "a", "playlist:other", true));
    unchanged();
    // An earlier page cannot truncate an already expanded season/playlist.
    assert(!emby::adoptQueue(queue, otherPane.items, "a", "playlist:p1"));
    assert(!emby::adoptQueue(queue, otherPane.items, "a", "playlist:p1", true));
    unchanged();
    auto replaced = held.items;
    replaced[0] = item("replacement");
    assert(!emby::adoptQueue(queue, replaced, "a", "playlist:p1"));
    assert(!emby::adoptQueue(queue, replaced, "a", "playlist:p1", true));
    unchanged();
    auto missingEntry = held.items;
    missingEntry[3] = item("a", "Episode", "different-entry");
    assert(!emby::adoptQueue(queue, missingEntry, "a", "playlist:p1", true));
    assert(!emby::adoptQueue(queue, expanded, "missing", "playlist:p1"));
    unchanged();

    auto reversed = held.items;
    std::reverse(reversed.begin(), reversed.end());
    assert(!emby::adoptQueue(queue, reversed, "a", "playlist:p1"));
    unchanged();
    assert(emby::adoptQueue(queue, reversed, "a", "playlist:p1", true));
    assert(queue.cursor == 1 && queue.items[1].playlistItemId == "3");

    emby::PlaybackQueue empty;
    empty.page = "former-page";
    assert(!emby::adoptQueue(empty, mixed, "audio", "new-page"));
    assert(!emby::adoptQueue(empty, mixed, "", "new-page"));
    assert(empty.items.empty() && empty.cursor == -1 && empty.page == "former-page");
    assert(emby::adoptQueue(empty, mixed, "a", "new-page"));
    assert(empty.items.size() == 3 && empty.cursor == 0 && empty.page == "new-page");

    // Repeated entries without entry ids still cannot lose a duplicate by
    // replacing it with an unrelated video of the same total list length.
    const std::vector<emby::Item> duplicates{item("a"), item("b"), item("a")};
    auto repeated = emby::takeQueue(duplicates, 2, "repeated");
    auto larger = duplicates;
    larger.push_back(item("c"));
    assert(emby::adoptQueue(repeated, larger, "a", "repeated") && repeated.cursor == 2);
    assert(!emby::adoptQueue(repeated, {item("a"), item("b"), item("c"), item("d")}, "a", "repeated", true));
    assert(repeated.cursor == 2 && repeated.items.size() == 4);
}

// A page asked for again: when that is due, and what it may do to the list
// being played.
void refreshTests() {
    const auto item = [](const char* id, const char* place = "") {
        emby::Item entry;
        entry.id = id;
        entry.playlistItemId = place;
        return entry;
    };
    // --- the order of what two lists share -------------------------------------
    const std::vector<emby::Item> held{item("a"), item("b"), item("c")};
    assert(emby::sameRelativeOrder(held, held));
    assert(emby::sameRelativeOrder(held, {}) && emby::sameRelativeOrder({}, held));
    // New entries anywhere, and entries gone, leave the order as it was.
    assert(emby::sameRelativeOrder(held, {item("n"), item("a"), item("b"), item("m"), item("c")}));
    assert(emby::sameRelativeOrder(held, {item("a"), item("c")}));
    assert(emby::sameRelativeOrder(held, {item("x"), item("y")}));
    // Sorted by last played, the one just started comes first: not the same.
    assert(!emby::sameRelativeOrder(held, {item("b"), item("a"), item("c")}));
    assert(!emby::sameRelativeOrder(held, {item("c"), item("n"), item("a")}));
    // Two entries of one video are told apart by their places.
    const std::vector<emby::Item> twice{item("a", "1"), item("b", "2"), item("a", "3")};
    assert(emby::sameRelativeOrder(twice, {item("a", "1"), item("b", "2"), item("a", "3"), item("d", "4")}));
    assert(!emby::sameRelativeOrder(twice, {item("a", "3"), item("b", "2"), item("a", "1")}));

    // --- whether a page asked for again has news ----------------------------------
    {
        auto watched = held;
        assert(emby::sameListing(held, watched) && emby::sameListing({}, {}));
        watched[1].positionTicks = 300000000;
        assert(!emby::sameListing(held, watched));
        watched = held;
        watched[2].played = true;
        assert(!emby::sameListing(held, watched));
        watched = held;
        watched[0].name = "Renamed";
        assert(!emby::sameListing(held, watched));
        watched = held;
        watched[0].primaryImageTag = "new";
        assert(!emby::sameListing(held, watched));
        watched = held;
        std::swap(watched[0], watched[1]);
        assert(!emby::sameListing(held, watched));
        watched = held;
        watched.push_back(item("d"));
        assert(!emby::sameListing(held, watched) && !emby::sameListing(watched, held));
    }

    // --- when the page shown is asked for again ------------------------------------
    emby::ListRefreshState state;
    state.shown = true;
    state.keepsOrder = true;
    state.held = 200;
    state.asked = 100'000;
    state.now = state.asked + 1'000;
    assert(!emby::listRefreshDue(state));
    // Half a minute on, unasked.
    state.now = state.asked + emby::kListRefreshIntervalMs - 1;
    assert(!emby::listRefreshDue(state));
    state.now = state.asked + emby::kListRefreshIntervalMs;
    assert(emby::listRefreshDue(state));
    // News: as soon as it is wanted, but not within two seconds of the last asking.
    state.now = state.asked + 1'000;
    state.wanted = state.asked + 500;
    assert(!emby::listRefreshDue(state));
    state.now = state.asked + emby::kListRefreshSpacingMs;
    assert(emby::listRefreshDue(state));
    state.wanted = state.now + 250;
    assert(!emby::listRefreshDue(state));
    state.now += 250;
    assert(emby::listRefreshDue(state));
    // Not while something is pressed or asked for, not when the browser is
    // away, not where the order would not be kept, not a page never asked for.
    auto blocked = state;
    blocked.busy = true;
    assert(!emby::listRefreshDue(blocked));
    blocked = state;
    blocked.shown = false;
    assert(!emby::listRefreshDue(blocked));
    blocked = state;
    blocked.keepsOrder = false;
    assert(!emby::listRefreshDue(blocked));
    blocked = state;
    blocked.asked = 0;
    assert(!emby::listRefreshDue(blocked));
    // A list grown long by scrolling is asked for again on news only.
    auto longList = state;
    longList.held = emby::kListRefreshLargest + 1;
    longList.wanted = 0;
    longList.now = longList.asked + 10 * emby::kListRefreshIntervalMs;
    assert(!emby::listRefreshDue(longList));
    longList.wanted = longList.now;
    assert(emby::listRefreshDue(longList));
    longList.held = emby::kListRefreshLargest;
    longList.wanted = 0;
    assert(emby::listRefreshDue(longList));

    // --- pictures that failed are tried again at the viewer's word ---------------
    ThumbnailCache cache;
    cache.insert("a|t|440", "jpeg bytes");
    cache.insert("b|t|440", "");
    cache.insert("c|t|440", "");
    assert(cache.size() == 3 && cache.contains("b|t|440"));
    assert(cache.forgetFailures() == 2);
    assert(cache.size() == 1 && cache.contains("a|t|440") && !cache.contains("b|t|440"));
    assert(cache.bytes() == 10 && cache.forgetFailures() == 0);
    // What was forgotten can be kept again, and the oldest still go first.
    cache.insert("b|t|440", "png");
    assert(cache.size() == 2 && cache.bytes() == 13 && *cache.find("b|t|440") == "png");
}

void localPlaylistTests() {
    // Entries arrive sorted by name, as Explorer shows them.
    const auto folder = [] {
        std::vector<LocalEntry> entries(4);
        entries[0] = {L"D:\\Clips\\a (2).mp4", 300, 40};
        entries[1] = {L"D:\\Clips\\a (10).mp4", 100, 10};
        entries[2] = {L"D:\\Clips\\b.mkv", 400, 30};
        entries[3] = {L"D:\\Clips\\c.mov", 200, 20};
        return entries;
    };
    const auto names = [](const std::vector<LocalEntry>& entries) {
        std::wstring joined;
        for (const auto& entry : entries) joined += localFileName(entry.path) + L"|";
        return joined;
    };
    auto entries = folder();
    orderLocalEntries(entries, emby::SortKey::Name, false, 1);
    assert(names(entries) == L"a (2).mp4|a (10).mp4|b.mkv|c.mov|");        // left as listed
    orderLocalEntries(entries, emby::SortKey::Name, true, 1);
    assert(names(entries) == L"c.mov|b.mkv|a (10).mp4|a (2).mp4|");
    entries = folder();
    orderLocalEntries(entries, emby::SortKey::Size, true, 1);
    assert(names(entries) == L"b.mkv|a (2).mp4|c.mov|a (10).mp4|");        // largest first
    entries = folder();
    orderLocalEntries(entries, emby::SortKey::Size, false, 1);
    assert(names(entries) == L"a (10).mp4|c.mov|a (2).mp4|b.mkv|");
    entries = folder();
    orderLocalEntries(entries, emby::SortKey::DateAdded, true, 1);
    assert(names(entries) == L"a (2).mp4|b.mkv|c.mov|a (10).mp4|");        // newest first
    // What a folder cannot answer stays by name.
    entries = folder();
    orderLocalEntries(entries, emby::SortKey::LastPlayed, false, 1);
    assert(names(entries) == L"a (2).mp4|a (10).mp4|b.mkv|c.mov|");
    // By length: no length read yet leaves the names as they are; read
    // ones run shortest or longest first, and one not read yet comes last
    // either way.
    entries = folder();
    orderLocalEntries(entries, emby::SortKey::Runtime, false, 1);
    assert(names(entries) == L"a (2).mp4|a (10).mp4|b.mkv|c.mov|");
    entries = folder();
    entries[0].duration = 90.0;
    entries[2].duration = 30.0;
    entries[3].duration = 600.0;
    orderLocalEntries(entries, emby::SortKey::Runtime, false, 1);
    assert(names(entries) == L"b.mkv|a (2).mp4|c.mov|a (10).mp4|");
    orderLocalEntries(entries, emby::SortKey::Runtime, true, 1);
    assert(names(entries) == L"c.mov|a (2).mp4|b.mkv|a (10).mp4|");
    // Random: the same seed is the same order, and nothing is lost.
    auto first = folder();
    auto second = folder();
    orderLocalEntries(first, emby::SortKey::Random, false, 42);
    orderLocalEntries(second, emby::SortKey::Random, true, 42);
    assert(names(first) == names(second) && first.size() == 4);

    assert(localFileName(L"D:\\Clips\\a (2).mp4") == L"a (2).mp4" && localFileName(L"plain.mp4") == L"plain.mp4");
    assert(localFileName(L"//nas/share/x.mkv") == L"x.mkv");
    assert(localDirectory(L"D:\\Clips\\a (2).mp4") == L"D:\\Clips" && localDirectory(L"plain.mp4").empty());
    assert(sameLocalDirectory(L"D:\\Clips", L"d:\\clips") && !sameLocalDirectory(L"", L""));
    assert(!sameLocalDirectory(L"D:\\Clips", L"D:\\Clips2"));
    entries = folder();
    assert(localEntryIndex(entries, L"d:\\other spelling\\B.MKV") == 2);   // the name identifies the file
    assert(localEntryIndex(entries, L"D:\\Clips\\missing.mp4") == -1);

    // The sidecar rule, and the video a subtitle file opened by itself
    // belongs to.
    assert(localStem(L"D:\\Clips\\movie.chs.ass") == L"movie.chs" && localStem(L"D:\\Clips\\.ass") == L".ass");
    assert(localStem(L"plain") == L"plain");
    assert(subtitleNameFitsVideo(L"movie", L"movie") && subtitleNameFitsVideo(L"Movie.CHS", L"movie"));
    assert(subtitleNameFitsVideo(L"movie.en.forced", L"movie"));
    assert(!subtitleNameFitsVideo(L"movie2", L"movie") && !subtitleNameFitsVideo(L"movie_chs", L"movie"));
    assert(!subtitleNameFitsVideo(L"mov", L"movie") && !subtitleNameFitsVideo(L"movie", L""));
    std::vector<LocalEntry> videos(4);
    videos[0].path = L"D:\\Show\\Show.mkv";
    videos[1].path = L"D:\\Show\\Show.S01E01.mkv";
    videos[2].path = L"D:\\Show\\Show.S01E01.mp4";
    videos[3].path = L"D:\\Show\\Show.S01E02.mkv";
    assert(videoForSubtitle(L"D:\\Show\\Show.S01E01.chs.ass", videos) == 1);   // longest name, then folder order
    assert(videoForSubtitle(L"D:\\Show\\show.s01e02.srt", videos) == 3);
    assert(videoForSubtitle(L"D:\\Show\\Show.sc.ass", videos) == 0);
    assert(videoForSubtitle(L"D:\\Show\\Other.ass", videos) == -1);
    assert(videoForSubtitle(L"D:\\Show\\Show.S01E03.ass", videos) == 0);       // "Show." still fits
    assert(videoForSubtitle(L"D:\\Show\\Show.ass", {}) == -1);
    // The list marks every video a subtitle file of the folder would be
    // loaded for, by that rule: "Show.S01E01.chs.ass" for "Show.mkv" too.
    markSubtitleFiles(videos, {L"D:\\Show\\Show.S01E01.chs.ass", L"D:\\Show\\Show.S01E0.srt"});
    assert(videos[0].subtitleFile && videos[1].subtitleFile && videos[2].subtitleFile && !videos[3].subtitleFile);
    markSubtitleFiles(videos, {L"D:\\Show\\SHOW.S01E02.ASS"});
    // Marked afresh, in any case; "Show." still fits.
    assert(videos[0].subtitleFile && !videos[1].subtitleFile && !videos[2].subtitleFile && videos[3].subtitleFile);
    markSubtitleFiles(videos, {});
    assert(std::none_of(videos.begin(), videos.end(), [](const LocalEntry& video) { return video.subtitleFile; }));
    // A text stream inside counts as much as a file beside.
    LocalEntry inside;
    assert(!localHasSubtitles(inside));
    inside.subtitleStream = true;
    assert(localHasSubtitles(inside));
    inside.subtitleStream = false;
    inside.subtitleFile = true;
    assert(localHasSubtitles(inside));

    assert(formatFileSize(0) == L"0 B" && formatFileSize(1023) == L"1023 B");
    assert(formatFileSize(12 * 1024) == L"12 KB" && formatFileSize(594140373) == L"567 MB");
    assert(formatFileSize(1503238553ull) == L"1.4 GB" && formatFileSize(10887169071ull) == L"10 GB");

    // The sheet's four sorts and where they sit among the browser's keys.
    for (int choice = 0; choice < kLocalSortCount; ++choice) {
        assert(localSortChoice(localSortKey(choice)) == choice);
    }
    assert(kLocalSortCount == 5);
    assert(localSortKey(1) == emby::SortKey::Size && localSortKey(2) == emby::SortKey::DateAdded &&
           localSortKey(3) == emby::SortKey::Runtime && localSortKey(4) == emby::SortKey::Random &&
           localSortKey(99) == emby::SortKey::Name);
    assert(localSortChoice(emby::SortKey::Release) == -1 && localSortChoice(emby::SortKey::LastPlayed) == -1);
    assert(std::wstring(localSortName(2)) == L"Date" && std::wstring(localSortName(3)) == L"Length");

    // Picture keys: a path, a width, and no confusion with an Emby key.
    const std::wstring path = L"\\\\NAS\\\x5BB6\x5EAD\\clip (1).mp4";
    const std::string key = localThumbnailKey(path, 440);
    assert(key.rfind("file|440|", 0) == 0);
    const auto parsed = parseLocalThumbnailKey(key);
    assert(parsed && parsed->width == 440 && parsed->path == path);
    assert(localThumbnailKey(L"", 440).empty());
    assert(!parseLocalThumbnailKey("109672|abc|440") && !parseLocalThumbnailKey("file|x|C:\\a.mp4") &&
           !parseLocalThumbnailKey("file|440|") && !parseLocalThumbnailKey("file|0|C:\\a.mp4") &&
           !parseLocalThumbnailKey(""));
    assert(!emby::parseImageKey(key));
}

void subtitleTests() {
    const std::string srt =
        "\xEF\xBB\xBF" "1\r\n00:00:07,550 --> 00:00:09,180\r\n<i>Hi</i> {\\an8}there\r\n\r\n"
        "2\r\n00:00:09,600 --> 00:00:11,320\r\nLine one\r\nLine &amp; two\r\n\r\n"
        "garbage line without timing\r\n\r\n"
        "3\r\n00:01:00,000 --> 00:00:59,000\r\nEnds before it starts\r\n";
    const auto track = parseSubtitles(srt);
    assert(track && track->cues.size() == 3);
    assert(near(track->cues[0].start, 7.55) && near(track->cues[0].end, 9.18) && track->cues[0].text == L"Hi there");
    assert(track->cues[1].text == L"Line one\nLine & two");
    // A cue that ends before it starts is given a short life rather than dropped.
    assert(near(track->cues[2].start, 60.0) && track->cues[2].end > 60.0);
    assert(subtitleTextAt(*track, 8.0) == L"Hi there");
    assert(subtitleTextAt(*track, 9.3) == L"");
    assert(subtitleTextAt(*track, 10.0) == L"Line one\nLine & two");
    assert(subtitleTextAt(*track, 11.32) == L"");  // the end is exclusive
    assert(subtitleTextAt(*track, 0.0) == L"");

    const std::string vtt =
        "WEBVTT\n\nNOTE a comment\n\n1\n00:00:01.000 --> 00:00:02.500 align:start position:10%\nHello\n\n"
        "00:01:02.000 --> 00:01:03.000\n<v Bob>Bye<br>now\n";
    const auto vttTrack = parseSubtitles(vtt);
    assert(vttTrack && vttTrack->cues.size() == 2);
    assert(near(vttTrack->cues[0].start, 1.0) && near(vttTrack->cues[0].end, 2.5) && vttTrack->cues[0].text == L"Hello");
    assert(near(vttTrack->cues[1].start, 62.0) && vttTrack->cues[1].text == L"Bye\nnow");
    // WebVTT's inline timing tags are markup too.
    const auto karaoke = parseSubtitles("WEBVTT\n\n00:00:01.000 --> 00:00:03.000\nOne <00:00:02.000>two <b>three</b>\n");
    assert(karaoke && karaoke->cues.size() == 1 && karaoke->cues[0].text == L"One two three");
    // A UTF-16 file with a byte-order mark, either way round, becomes UTF-8.
    assert(decodeSubtitleBytes(std::string("\xFF\xFE" "H\0i\0", 6)) == "Hi");
    assert(decodeSubtitleBytes(std::string("\xFE\xFF" "\0H\0i", 6)) == "Hi");
    assert(decodeSubtitleBytes("plain") == "plain" && decodeSubtitleBytes("").empty());

    const std::string ass =
        "[Script Info]\nTitle: t\nPlayResX: 704\n\n[V4+ Styles]\nFormat: Name, Fontname\nStyle: Default,Arial\n\n"
        "[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:10.50,0:00:12.00,Default,,0,0,0,,{\\an8}{\\fad(200,200)}Second, with comma\\NNext line\n"
        "Comment: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,ignored\n"
        "Dialogue: 0,0:00:01.00,0:00:03.00,Default,,0,0,0,,First\\hline\n";
    const auto assTrack = parseSubtitles(ass);
    assert(assTrack && assTrack->cues.size() == 2);
    // Sorted by start even though the file listed them the other way round.
    assert(near(assTrack->cues[0].start, 1.0) && assTrack->cues[0].text == L"First line");
    assert(near(assTrack->cues[1].start, 10.5) && near(assTrack->cues[1].end, 12.0) &&
           assTrack->cues[1].text == L"Second, with comma\nNext line");
    // No Format line: the default v4+ field order applies.
    const auto bareAss = parseSubtitles("[Events]\nDialogue: 0,0:00:05.00,0:00:06.00,Default,,0,0,0,,Plain\n");
    assert(bareAss && bareAss->cues.size() == 1 && bareAss->cues[0].text == L"Plain" && near(bareAss->cues[0].start, 5.0));

    // Overlapping cues show together, in start order.
    SubtitleTrack overlap;
    overlap.cues = {{1.0, 3.0, L"A"}, {2.0, 4.0, L"B"}};
    assert(subtitleTextAt(overlap, 2.5) == L"A\nB");
    assert(subtitleTextAt(overlap, 3.5) == L"B");
    assert(subtitleTextAt(overlap, 0.5).empty() && subtitleTextAt(overlap, 4.0).empty());

    assert(!parseSubtitles("") && !parseSubtitles("   \r\n"));
    const auto none = parseSubtitles("just some text\nwithout any timing\n");
    assert(none && none->cues.empty());
    assert(subtitle_detail::parseTimestamp(L"1:02:03.45").has_value() && near(*subtitle_detail::parseTimestamp(L"1:02:03.45"), 3723.45));
    assert(near(*subtitle_detail::parseTimestamp(L"02:03.456"), 123.456));
    assert(!subtitle_detail::parseTimestamp(L"abc") && !subtitle_detail::parseTimestamp(L"1:2:3:4") && !subtitle_detail::parseTimestamp(L"5"));
}

void subtitleLookupTests() {
    // E08: sixteen intervening cues have ended, but the first sign is still
    // on screen. Use the parser so this exercises the playback lookup index.
    std::string srt = "1\n00:00:00,000 --> 00:01:40,000\nLong sign\n\n";
    for (int cue = 1; cue <= 16; ++cue) {
        const std::string second = std::to_string(cue);
        srt += std::to_string(cue + 1) + "\n00:00:" + second + ",000 --> 00:00:" +
               second + ",250\nShort " + second + "\n\n";
    }
    const auto longSign = parseSubtitles(srt);
    assert(longSign && longSign->cues.size() == 17);
    assert(subtitleTextAt(*longSign, 17.0) == L"Long sign");
    assert(subtitleTextAt(*longSign, 100.0).empty());
    assert(subtitleTextAt(*longSign, 16.0) == L"Long sign\nShort 16");
    assert(subtitleTextAt(*longSign, 16.25) == L"Long sign");
    assert(subtitleTextAt(*longSign, 0.0) == L"Long sign");
    assert(subtitleTextAt(*longSign, -0.25).empty());
    assert(subtitleTextAt(*longSign, 99.75) == L"Long sign");

    // There is no limit on simultaneous cues. Equal start times retain file
    // order, and end times stay exclusive after seeking forward and backward.
    std::string ass = "[Events]\n";
    std::wstring allLayers;
    for (int cue = 0; cue < 40; ++cue) {
        ass += "Dialogue: 0,0:00:05.00,0:00:20.00,Default,,0,0,0,,Layer " +
               std::to_string(cue) + "\n";
        if (!allLayers.empty()) allLayers += L'\n';
        allLayers += L"Layer " + std::to_wstring(cue);
    }
    const auto layers = parseSubtitles(ass);
    assert(layers && layers->cues.size() == 40);
    assert(subtitleTextAt(*layers, 20.0).empty());
    assert(subtitleTextAt(*layers, 5.0) == allLayers);
    assert(subtitleTextAt(*layers, 4.75).empty());
    assert(subtitleTextAt(*layers, 19.75) == allLayers);

    // Compare nested and crossing intervals with an independent full-scan
    // oracle, visiting times in a fixed shuffled order instead of assuming
    // that every query moves forward. Adjacent cues also share start times.
    SubtitleTrack varied;
    for (int cue = 0; cue < 300; ++cue) {
        const double start = static_cast<double>(cue / 3) * 0.5;
        const double duration = cue % 23 == 0 ? 60.0 : static_cast<double>((cue * 17) % 35 + 1) * 0.25;
        varied.cues.push_back({start, start + duration, L"Cue " + std::to_wstring(cue)});
    }
    varied.rebuildLookupIndex();
    SubtitleTrack unindexed;
    unindexed.cues = varied.cues;  // manually assembled tracks remain usable
    auto expectedAt = [&](double time) {
        std::wstring expected;
        for (const auto& cue : varied.cues) {
            if (cue.start <= time && time < cue.end) {
                if (!expected.empty()) expected += L'\n';
                expected += cue.text;
            }
        }
        return expected;
    };
    for (int sample = 0; sample < 997; ++sample) {
        const double time = static_cast<double>((sample * 37) % 997) * 0.125 - 1.0;
        const auto expected = expectedAt(time);
        assert(subtitleTextAt(varied, time) == expected);
        assert(subtitleTextAt(unindexed, time) == expected);
    }
    assert(subtitleTextAt(SubtitleTrack{}, 0.0).empty());
}

// Where a line sits, what is not a line at all, and a track read a packet
// at a time.
void subtitlePlacementTests() {
    const std::string ass =
        "[Script Info]\nScriptType: v4.00+\nPlayResX: 864\nPlayResY: 480\n\n"
        "[V4+ Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, "
        "Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, "
        "MarginR, MarginV, Encoding\n"
        "Style: Default,SimHei,32,&H00FFFFFF,&H00FFFFFF,&H20514300,&H0049400D,1,0,0,0,100,100,0,0.00,1,2,1,2,10,10,10,1\n"
        "Style: op_jap1,Gothic,22,&H000761FE,&H0088EAF5,&H00336198,&H0049A7E8,0,1,0,0,100,100,0,0.00,1,1,1,9,10,10,5,1\n\n"
        "[Events]\nFormat: Layer, Start, End, Style, Actor, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,Speech\n"
        "Dialogue: 2,0:00:01.00,0:00:02.00,op_jap1,,0,0,0,,{\\k17}Break {\\k20}Out\n"
        "Dialogue: 1,0:00:01.00,0:00:02.00,op_jap1,,0,0,0,,{\\bord3}Break Out\n"
        "Dialogue: 0,0:00:01.00,0:00:02.00,*Default,,0,0,0,,{\\fad(200,200)\\an8}Note\n"
        "Dialogue: 0,0:00:03.00,0:00:04.00,Default,,0,0,0,,{\\fad(2100,50)}{\\an1}{\\pos(140,122)}Sign above\n"
        "Dialogue: 0,0:00:03.00,0:00:04.00,op_jap1,,0,0,0,,{\\move(864,400,854,400,0,500)}Crawl below\n"
        "Dialogue: 0,0:00:05.00,0:00:06.00,Default,,0,0,0,,{\\p1}m 0 0 l 100 0 100 100 0 100{\\p0}\n"
        "Dialogue: 0,0:00:05.00,0:00:06.00,Default,,0,0,0,,Left{\\p1}m 0 0 l 9 9{\\p0} right{\\t(0,500,\\fs40)}\n";
    const auto track = parseSubtitles(ass);
    assert(track && track->cues.size() == 7);
    // A style's alignment, an override of it, and a position in the upper
    // or the lower half each decide; layered copies of a line are one line.
    auto lines = subtitleLinesAt(*track, 1.5);
    assert(lines.bottom == L"Speech" && lines.top == L"Break Out\nNote");
    assert(subtitleTextAt(*track, 1.5) == L"Speech\nBreak Out\nNote");
    lines = subtitleLinesAt(*track, 3.5);
    assert(lines.top == L"Sign above" && lines.bottom == L"Crawl below");
    // A drawing is not a line, and what is drawn inside a line goes.
    lines = subtitleLinesAt(*track, 5.5);
    assert(lines.bottom == L"Left right" && lines.top.empty() && !lines.empty());
    assert(subtitleLinesAt(*track, 9.0).empty());

    // An SSA script counts alignment its own way: 6 is top centre, 10 middle.
    const std::string ssa =
        "[Script Info]\nScriptType: v4.00\n\n[V4 Styles]\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, TertiaryColour, BackColour, Bold, Italic, "
        "BorderStyle, Outline, Shadow, Alignment, MarginL, MarginR, MarginV, AlphaLevel, Encoding\n"
        "Style: Default,Arial,20,1,2,3,4,0,0,1,1,0,2,10,10,10,0,1\n"
        "Style: Title,Arial,20,1,2,3,4,0,0,1,1,0,6,10,10,10,0,1\n\n"
        "[Events]\nFormat: Marked, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\n"
        "Dialogue: Marked=0,0:00:01.00,0:00:02.00,Title,,0,0,0,,Above\n"
        "Dialogue: Marked=0,0:00:01.00,0:00:02.00,Default,,0,0,0,,Below\n"
        "Dialogue: Marked=0,0:00:01.00,0:00:02.00,Default,,0,0,0,,{\\a10}Middle\n"
        "Dialogue: Marked=0,0:00:01.00,0:00:02.00,Title,,0,0,0,,{\\a2}Back down\n";
    const auto legacy = parseSubtitles(ssa);
    assert(legacy && legacy->cues.size() == 4);
    lines = subtitleLinesAt(*legacy, 1.5);
    assert(lines.top == L"Above\nMiddle" && lines.bottom == L"Below\nBack down");
    assert(subtitle_detail::alignmentFromLegacy(1) == 1 && subtitle_detail::alignmentFromLegacy(7) == 9 &&
           subtitle_detail::alignmentFromLegacy(9) == 4 && subtitle_detail::alignmentFromLegacy(4) == 0);
    // Styles with no Format line: each script's own column for the alignment.
    const auto bareLegacy = parseSubtitles(
        "[Script Info]\n\n[V4 Styles]\nStyle: Title,Arial,20,1,2,3,4,0,0,1,1,0,6,10,10,10,0,1\n\n"
        "[Events]\nDialogue: Marked=0,0:00:01.00,0:00:02.00,Title,,0,0,0,,Above\n");
    assert(bareLegacy && bareLegacy->cues.size() == 1 && bareLegacy->cues[0].top);
    const auto bareModern = parseSubtitles(
        "[Script Info]\n\n[V4+ Styles]\n"
        "Style: Title,Arial,20,&H0,&H0,&H0,&H0,0,0,0,0,100,100,0,0,1,1,0,8,10,10,10,1\n\n"
        "[Events]\nDialogue: 0,0:00:01.00,0:00:02.00,Title,,0,0,0,,Above\n");
    assert(bareModern && bareModern->cues.size() == 1 && bareModern->cues[0].top);
    // A run of digits no time could have is not a time.
    assert(!subtitle_detail::parseTimestamp(L"0:00:99999999999999999999") &&
           !subtitle_detail::parseTimestamp(L"99999999999999999999:00:01") &&
           !subtitle_detail::parseTimestamp(L"0:00:01.99999999999999999999"));
    // Bytes that are UTF-8 as they stand, and a legacy code page's that are not.
    assert(isUtf8Text("plain") && isUtf8Text("\xE7\xAE\x80\xE4\xBD\x93") && isUtf8Text("") &&
           !isUtf8Text("\xBC\xF2\xCC\xE5") && !isUtf8Text("\xC1\x63\xC5\xE9"));

    // What a server's conversion from ASS leaves in an SRT: {\anN} for the
    // place, the same line once per layer, a shape's outline as a "line".
    const std::string converted =
        "1\n00:00:18,550 --> 00:00:19,920\n<font face=\"Gothic\" size=\"22\" color=\"#fe6107\"><i>{\\an9}Break Out\xEF\xBC\x81</i></font>\n\n"
        "2\n00:00:18,550 --> 00:00:19,920\n<font face=\"Gothic\" size=\"22\" color=\"#f4d751\"><i>{\\an9}Break Out\xEF\xBC\x81</i></font>\n\n"
        "3\n00:00:18,550 --> 00:00:19,920\nSpoken\n\n"
        "4\n00:00:18,550 --> 00:00:19,920\nm 0 0 l 864 0 864 480 0 480\n";
    const auto srt = parseSubtitles(converted);
    assert(srt && srt->cues.size() == 3);
    lines = subtitleLinesAt(*srt, 19.0);
    assert(lines.top == L"Break Out\xFF01" && lines.bottom == L"Spoken");
    assert(subtitle_detail::looksLikeDrawing(L"m 0 0 l 100 0 100 100") &&
           !subtitle_detail::looksLikeDrawing(L"m 0 0 later") && !subtitle_detail::looksLikeDrawing(L"mmm"));

    // A stream inside a container: FFmpeg hands the header over once and
    // each event without its times.
    const auto header = parseAssHeader(
        "[Script Info]\r\nPlayResY: 360\r\n\r\n[V4+ Styles]\r\n"
        "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, OutlineColour, BackColour, Bold, Italic, "
        "Underline, StrikeOut, ScaleX, ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, MarginL, "
        "MarginR, MarginV, Encoding\r\n"
        "Style: Default,Arial,16,&Hffffff,&Hffffff,&H0,&H0,0,0,0,0,100,100,0,0,1,1,0,2,10,10,10,1\r\n"
        "Style: staff,simhei,16,&H0,&H0,&H0,&H0,-1,0,0,0,100,100,0,0.00,1,0,2,8,30,30,10,1\r\n\r\n"
        "[Events]\r\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text\r\n");
    assert(near(header.playResY, 360.0) && header.styles.size() == 2 && header.alignment(L"staff") == 8 &&
           header.alignment(L"Default") == 2 && header.alignment(L"missing") == 2);
    auto cue = cueFromDecodedAss(header, "12,0,staff,,0,0,0,,Hello, {\\i1}world{\\i0}\\Nagain", 10.0, 12.5);
    assert(cue && cue->text == L"Hello, world\nagain" && cue->top && near(cue->start, 10.0) && near(cue->end, 12.5));
    cue = cueFromDecodedAss(header, "13,0,Default,,0,0,0,,{\\pos(289,210)}Low", 1.0, 1.0);
    assert(cue && !cue->top && near(cue->end, 3.0));  // no duration: a short life, not none
    assert(!cueFromDecodedAss(header, "too,few,fields", 1.0, 2.0));
    assert(!cueFromDecodedAss(header, "14,0,Default,,0,0,0,,{\\p1}m 0 0 l 5 5{\\p0}", 1.0, 2.0));
    assert(!cueFromDecodedAss(header, "15,0,Default,,0,0,0,,Early", -1.0, 2.0));
    assert(parseAssHeader("").styles.empty() && near(parseAssHeader("").playResY, 288.0));

    // Cues arrive in the order the demuxer passes them, not always by time,
    // and a seek back passes the same ones again.
    SubtitleTrack growing;
    assert(insertSubtitleCue(growing, {5.0, 6.0, L"five"}));
    assert(insertSubtitleCue(growing, {1.0, 9.0, L"long"}));
    assert(insertSubtitleCue(growing, {3.0, 3.5, L"three"}));
    assert(insertSubtitleCue(growing, {3.0, 3.5, L"three", true}));  // the same words elsewhere are another line
    assert(!insertSubtitleCue(growing, {3.0, 3.5, L"three"}) && !insertSubtitleCue(growing, {1.0, 9.0, L"long"}));
    assert(insertSubtitleCue(growing, {7.0, 8.0, L"seven"}));
    assert(growing.cues.size() == 5 && growing.prefixMaxEnd.size() == 5);
    for (std::size_t i = 1; i < growing.cues.size(); ++i) assert(growing.cues[i - 1].start <= growing.cues[i].start);
    SubtitleTrack rebuilt;
    rebuilt.cues = growing.cues;
    rebuilt.rebuildLookupIndex();
    assert(rebuilt.prefixMaxEnd == growing.prefixMaxEnd);
    assert(subtitleTextAt(growing, 3.25) == L"long\nthree\nthree" && subtitleTextAt(growing, 7.5) == L"long\nseven");
    assert(subtitleLinesAt(growing, 3.25).top == L"three" && subtitleTextAt(growing, 9.0).empty());
    // A track put together by hand, without its index, still takes cues.
    SubtitleTrack unindexed;
    unindexed.cues = {{1.0, 2.0, L"a"}, {4.0, 5.0, L"c"}};
    assert(insertSubtitleCue(unindexed, {3.0, 3.5, L"b"}) && unindexed.prefixMaxEnd.size() == 3 &&
           unindexed.cues[1].text == L"b");
}

// Which of several subtitles suits the viewer.
void subtitleChoiceTests() {
    // Codes from containers, from the server and from Windows' own list.
    assert(subtitleLanguageTag("chi") == "zh" && subtitleLanguageTag("zho") == "zh" && subtitleLanguageTag("zh") == "zh");
    assert(subtitleLanguageTag("zh-CN") == "zh-Hans" && subtitleLanguageTag("zh-Hans-CN") == "zh-Hans" &&
           subtitleLanguageTag("zh_SG") == "zh-Hans" && subtitleLanguageTag("chs") == "zh-Hans");
    assert(subtitleLanguageTag("zh-TW") == "zh-Hant" && subtitleLanguageTag("zh-HK") == "zh-Hant" &&
           subtitleLanguageTag("zh-Hant-TW") == "zh-Hant" && subtitleLanguageTag("cht") == "zh-Hant");
    assert(subtitleLanguageTag("eng") == "en" && subtitleLanguageTag("en-NZ") == "en" &&
           subtitleLanguageTag("jpn") == "ja" && subtitleLanguageTag("ja") == "ja" &&
           subtitleLanguageTag("kor") == "ko" && subtitleLanguageTag("fre") == "fr" && subtitleLanguageTag("deu") == "de");
    // A title settles what the code leaves open, and stands in for no code.
    assert(subtitleLanguageTag("chi", L"SC") == "zh-Hans" && subtitleLanguageTag("chi", L"TC") == "zh-Hant" &&
           subtitleLanguageTag("chi", L"Commentary") == "zh");
    assert(subtitleLanguageTag("", L"SC (\x9ED8\x8BA4 ASS)") == "zh-Hans" &&
           subtitleLanguageTag("", L"unibig5 (ASS)") == "zh-Hant" &&
           subtitleLanguageTag("und", L"\x7B80\x65E5\x53CC\x8BED") == "zh-Hans" &&
           subtitleLanguageTag("", L"\x7E41\x9AD4\x4E2D\x6587") == "zh-Hant" &&
           subtitleLanguageTag("", L"\x4E2D\x65E5\x53CC\x8BED") == "zh" &&
           subtitleLanguageTag("", L"Chinese Simplified (SSA)") == "zh-Hans" &&
           subtitleLanguageTag("", L"chs&jpn") == "zh-Hans" && subtitleLanguageTag("", L"English (SRT)") == "en" &&
           subtitleLanguageTag("", L"\x65E5\x672C\x8A9E") == "ja" && subtitleLanguageTag("", L"1").empty() &&
           subtitleLanguageTag("", L"").empty() && subtitleLanguageTag("und", L"(ASS)").empty());
    // "sc" is a language only as a word of its own.
    assert(subtitleLanguageTag("", L"script").empty() && subtitleLanguageTag("", L"tcp dump").empty());

    // What Windows lists, other than English first; a choice before it all.
    using Tags = std::vector<std::string>;
    assert(subtitleLanguagePreference(SubtitleLanguage::Auto, {"en-US", "zh-Hans-CN"}) == (Tags{"zh-Hans", "en"}));
    assert(subtitleLanguagePreference(SubtitleLanguage::Auto, {"zh-Hant-TW", "en-US", "ja"}) ==
           (Tags{"zh-Hant", "ja", "en"}));
    assert(subtitleLanguagePreference(SubtitleLanguage::Auto, {}) == (Tags{"en"}));
    assert(subtitleLanguagePreference(SubtitleLanguage::English, {"en-US", "zh-Hans-CN"}) == (Tags{"en", "zh-Hans"}));
    assert(subtitleLanguagePreference(SubtitleLanguage::Japanese, {"en-US"}) == (Tags{"ja", "en"}));
    assert(subtitleLanguagePreference(SubtitleLanguage::ChineseTraditional, {"zh-Hans-CN"}) ==
           (Tags{"zh-Hant", "zh-Hans"}));
    assert(clampSubtitleLanguage(99) == SubtitleLanguage::Korean && clampSubtitleLanguage(-1) == SubtitleLanguage::Auto);
    assert(std::wstring(subtitleLanguageName(SubtitleLanguage::Auto)) == L"Auto" &&
           std::wstring(subtitleLanguageName(SubtitleLanguage::ChineseSimplified)) == L"\x7B80\x4F53\x4E2D\x6587");

    const Tags wanted{"zh-Hans", "en"};
    const auto rank = [&](const char* language, const wchar_t* title, bool external) {
        return subtitleLanguageRank({language, title, false, false, external}, wanted);
    };
    // The wanted script, the language with its script unsaid, the other
    // script, a file that names no language, the next language, a stream
    // that names none, a language nobody asked for.
    assert(rank("chi", L"SC", false) == 0 && rank("chi", L"", false) == 1 && rank("zh-TW", L"", false) == 2 &&
           rank("", L"", true) == 3 && rank("eng", L"", false) == 4 && rank("", L"", false) == 8 &&
           rank("fre", L"", false) == 9);

    // The streams of videos on the server this was written against.
    std::vector<SubtitleCandidate> clannad{
        {"", L"SC (\x9ED8\x8BA4 ASS)", true, false, false}, {"", L"TC (ASS)", false, false, false}};
    assert(chooseSubtitle(clannad, wanted) == 0);
    assert(chooseSubtitle(clannad, {"zh-Hant", "en"}) == 1);
    // One language twice: the stream marked default, then a file.
    std::vector<SubtitleCandidate> twice{
        {"chi", L"", false, false, false}, {"chi", L"", true, false, false}, {"zh-HK", L"", false, false, true}};
    assert(chooseSubtitle(twice, wanted) == 1);
    twice[1].isDefault = false;
    assert(chooseSubtitle(twice, wanted) == 0);
    assert(chooseSubtitle(twice, {"zh-Hant"}) == 2);
    // A forced track only carries the lines the audio leaves untranslated.
    std::vector<SubtitleCandidate> forced{{"eng", L"Signs", true, true, false}, {"eng", L"Full", false, false, false}};
    assert(chooseSubtitle(forced, {"en"}) == 1);
    // A file the viewer put beside the video, its language unsaid, before
    // a stream in a language they read second; not before one they read first.
    std::vector<SubtitleCandidate> mixed{{"eng", L"", true, false, false}, {"", L"", false, false, true}};
    assert(chooseSubtitle(mixed, wanted) == 1);
    mixed.push_back({"chi", L"", false, false, false});
    assert(chooseSubtitle(mixed, wanted) == 2);
    // Anything rather than nothing, and nothing from an empty list.
    assert(chooseSubtitle({{"fre", L"", false, false, false}}, wanted) == 0);
    assert(chooseSubtitle({}, wanted) == -1);
    // Many files of one kind: the first, as they are listed.
    const std::vector<SubtitleCandidate> many(186, SubtitleCandidate{"zh-CN", L"Chinese Simplified (SSA)", false, false, true});
    assert(chooseSubtitle(many, wanted) == 0);
}

// What libass is given: a file's text kept beside its cues, an SRT turned
// into a script, and the colour correction of a VSFilter-era script.
void subtitleScriptTests() {
    const std::string srt =
        "\xEF\xBB\xBF" "1\r\n00:00:01,234 --> 00:00:03,000\r\n<i>Hi</i> <b>there</b>, <u>you</u>\r\n"
        "<font color=\"#FF8000\" face=\"Comic Sans MS\">orange</font> &amp; {braces}\r\n\r\n"
        "2\r\n00:01:02,000 --> 01:00:00,010\r\n{\\an8}Top\r\nline two\r\n\r\n"
        "3\r\n00:00:05,000 --> 00:00:06,000\r\n<v Bob>Voiced <c.yellow>class</c> <00:00:05.500>timed\r\n";
    const auto parsed = parseSubtitles(srt);
    assert(parsed && !parsed->ass && parsed->text.size() == srt.size() - 3 && parsed->text.front() == '1');
    assert(parseSubtitles("[Script Info]\nDialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,x\n")->ass);

    assert(assTextFromSrt(L"<i>Hi</i>\nthere") == L"{\\i1}Hi{\\i}\\Nthere");
    assert(assTextFromSrt(L"<B>bold</B><s>x</s>") == L"{\\b700}bold{\\b}{\\s1}x{\\s}");
    assert(assTextFromSrt(L"<font color=red>r</font>") == L"{\\c&H0000FF&}r{\\c\\fn}");
    assert(assTextFromSrt(L"<font color=\"#FF8000\" face=\"Comic Sans MS\">o</font>") ==
           L"{\\c&H0080FF&\\fnComic Sans MS}o{\\c\\fn}");
    assert(assTextFromSrt(L"<font size=\"20\">s</font>") == L"s{\\c\\fn}");
    assert(assTextFromSrt(L"{\\an8}kept {not an override} &lt;b&gt;") == L"{\\an8}kept \\{not an override\\} <b>");
    assert(assTextFromSrt(L"<v Bob>a <c.yellow>b</c> <00:00:05.500>c") == L"a b c");
    assert(assTextFromSrt(L"line<br>break") == L"line\\Nbreak");
    // A "<" that opens no tag is text; a backslash before n or h is one.
    assert(assTextFromSrt(L"a<b and c>d") == L"a<b and c>d");
    assert(assTextFromSrt(L"x < y > z <lang en>w</lang>") == L"x < y > z w");
    assert(assTextFromSrt(L"C:\\new\\hold \\N") == L"C:\\\x2060new\\\x2060hold \\N");

    PlainSubtitleStyle style;
    style.font = "Microsoft YaHei";
    const std::string script = plainSubtitleScript(srt, style);
    assert(script.rfind("[Script Info]\n", 0) == 0);
    // FFmpeg's height, which the sizes a stream's decoder writes assume.
    assert(script.find("PlayResX: 384\nPlayResY: 288\n") != std::string::npos);
    assert(script.find("Style: Default,Microsoft YaHei,13.87,") != std::string::npos);
    assert(script.find(",1,0.96,0,2,19,19,17,1\n") != std::string::npos);
    assert(script.find("YCbCr Matrix: None\n") != std::string::npos);
    assert(script.find("Dialogue: 0,0:00:01.23,0:00:03.00,Default,,0,0,0,,{\\i1}Hi{\\i} {\\b700}there{\\b}, "
                       "{\\u1}you{\\u}\\N{\\c&H0080FF&\\fnComic Sans MS}orange{\\c\\fn} & \\{braces\\}\n") !=
           std::string::npos);
    assert(script.find("Dialogue: 0,0:01:02.00,1:00:00.01,Default,,0,0,0,,{\\an8}Top\\Nline two\n") !=
           std::string::npos);
    assert(script.find("Voiced class timed") != std::string::npos);
    style.box = true;
    style.font = "Bad,Name";
    const std::string boxed = plainSubtitleScript(srt, style);
    assert(boxed.find("Style: Default,BadName,13.87,") != std::string::npos);
    assert(boxed.find(",4,0.8,3.73,2,19,19,17,1\n") != std::string::npos);
    assert(plainSubtitleScript("no cue at all\n", {}).empty());
    assert(plainSubtitleScript("WEBVTT\n\n00:01.000 --> 00:02.000 line:0\nvtt line\n", {})
               .find("Dialogue: 0,0:00:01.00,0:00:02.00,Default,,0,0,0,,vtt line\n") != std::string::npos);

    assert(plainSubtitleFont({"zh-Hans", "en"}) == "Microsoft YaHei");
    assert(plainSubtitleFont({"zh-Hant"}) == "Microsoft JhengHei");
    assert(plainSubtitleFont({"ja"}) == "Yu Gothic");
    assert(plainSubtitleFont({"ko"}) == "Malgun Gothic");
    assert(plainSubtitleFont({"en"}) == "Segoe UI" && plainSubtitleFont({}) == "Segoe UI");

    // The correction a script's colours take: none for None or the same
    // matrix; white, black and grey stay; a saturated colour moves, and
    // there and back again it returns.
    using M = SubtitleMatrix;
    assert(subtitleColourForVideo(0x123456U, M::None, M::Bt709Tv) == 0x123456U);
    assert(subtitleColourForVideo(0x123456U, M::Bt601Tv, M::None) == 0x123456U);
    assert(subtitleColourForVideo(0x123456U, M::Bt709Tv, M::Bt709Tv) == 0x123456U);
    for (const std::uint32_t neutral : {0xFFFFFFU, 0x000000U, 0x808080U}) {
        assert(subtitleColourForVideo(neutral, M::Bt601Tv, M::Bt709Tv) == neutral);
    }
    const std::uint32_t red = subtitleColourForVideo(0xFF0000U, M::Bt601Tv, M::Bt709Tv);
    assert(red != 0xFF0000U && ((red >> 16) & 0xFF) > 200);
    const std::uint32_t back = subtitleColourForVideo(0x3C8CC8U, M::Bt601Tv, M::Bt709Tv);
    const std::uint32_t again = subtitleColourForVideo(back, M::Bt709Tv, M::Bt601Tv);
    for (int shift : {0, 8, 16}) {
        const int a = static_cast<int>((again >> shift) & 0xFF);
        const int b = static_cast<int>((0x3C8CC8U >> shift) & 0xFF);
        assert(std::abs(a - b) <= 1);
    }
    // TV range read as full range is the classic washed-out mistake.
    assert(subtitleColourForVideo(0x000000U, M::Bt709Tv, M::Bt709Pc) == 0x101010U);
}

void encodingTests() {
    const std::string utf8 = "ascii \xE3\x82\x88\xE3\x81\x86 \xF0\x9F\x8E\xAC end";  // ようこ + clapper
    const std::wstring wide = utf8ToWideText(utf8);
    assert(wide == L"ascii \x3088\x3046 \xD83C\xDFAC end");
    assert(wideToUtf8Text(wide) == utf8);
    // A stray continuation byte and a truncated sequence become U+FFFD, the rest survives.
    const std::wstring damaged = utf8ToWideText("a\x80" "b\xE3\x82");
    assert(damaged == L"a\xFFFD" L"b\xFFFD");
    assert(utf8ToWideText("").empty() && wideToUtf8Text(L"").empty());
}

}  // namespace

int main() {
    localPlaylistTests();
    try {
        protocolTests();
        movieTvMetadataTests();
        detailMetadataTests();
        playlistAndListTests();
        playbackQueueTests();
        refreshTests();
        subtitleTests();
        subtitleLookupTests();
        subtitlePlacementTests();
        subtitleChoiceTests();
        subtitleScriptTests();
        encodingTests();
    } catch (const std::exception& error) {
        std::cerr << "Emby tests threw: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Emby protocol, subtitle and encoding tests passed\n";
    return 0;
}
