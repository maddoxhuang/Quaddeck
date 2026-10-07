// Live Emby probe: signs in to a server through the same EmbyClient the
// player uses, lists the libraries, and can write the player's sign-in file
// so a run of QuadDeck.exe with an emby:// locator starts signed in. Needs
// a reachable server, so it is not a CTest.
//
//   QuadDeckEmbyProbe <server-url> <user> [password] [--save <emby.qauth path>]
//
// With a sign-in file it asks as the player would, without a password, and
// only reads: it never signs out, which would end the player's own sign-in.
//
//   QuadDeckEmbyProbe --auth <emby.qauth path> [--playlist <id>] [--folder <id>] [--flat <id>]
//                     [--library <id> <collection type>] [--limit <n>] [--names]
//                     [--subtitles] [--subtitle-flags] [--subtitle-streams <item id>]
//                     [--save-subtitles <folder>]
//
// --playlist lists a playlist's entries in its own order, --folder a
// folder's children, --flat every video under a library or folder, and
// --library a library the way its type is listed; each through
// emby::listQuery, the request the browser makes. Titles are printed only
// with --names. --subtitles counts the subtitle streams of every video the
// account sees, by codec and by whether they are in the file or beside it;
// --subtitle-flags checks which videos the browser marks as having
// subtitles, asked for by id as it asks, against those streams;
// --subtitle-streams lists one item's and fetches each text one as SRT, the
// request the player makes, to say how many cues it parses into (and as ASS
// where that is the stream's own format); --save-subtitles keeps what came.

#include "EmbyApi.hpp"
#include "EmbyClient.hpp"
#include "FilePersistence.hpp"
#include "Subtitles.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace quaddeck;

namespace {

// Runs one request to completion on this thread's pump. The completion
// state is shared with the handler, so a reply that arrives after the
// wait gave up writes into memory that is still there.
EmbyClient::Response call(EmbyClient& client, const std::string& method, const std::string& path,
                          const std::string& body = {}) {
    struct State {
        EmbyClient::Response result;
        bool done{};
    };
    auto state = std::make_shared<State>();
    client.request(method, path, body, [state](const EmbyClient::Response& response) {
        if (state->done) return;
        state->result = response;
        state->done = true;
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(40);
    while (!state->done && std::chrono::steady_clock::now() < deadline) {
        client.pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!state->done) {
        state->done = true;  // a late reply is ignored
        state->result.error = "timed out";
    }
    return state->result;
}

void printViews(EmbyClient& client, const emby::Session& session) {
    const auto views = call(client, "GET", emby::viewsPath(session.userId));
    if (const auto list = views.ok ? emby::parseItems(views.body) : std::nullopt) {
        for (const auto& view : list->items) {
            std::cout << "view " << view.id << " " << view.name << " [" << view.collectionType << "]\n";
        }
    } else {
        std::cerr << "views: " << views.error << '\n';
    }
}

// One page of a list, asked for as the browser asks.
bool printList(EmbyClient& client, const emby::Session& session, const emby::ListPage& page,
               const emby::ListArrangement& arrangement, int limit, bool names) {
    const std::string path = emby::itemsPath(
        session.userId, emby::listQuery(page, arrangement, 0, limit, "ParentId,DateCreated,Size,ChildCount"));
    std::cout << "GET " << path << '\n';
    const auto reply = call(client, "GET", path);
    if (!reply.ok) {
        std::cerr << "list: " << reply.status << ' ' << reply.error << '\n';
        return false;
    }
    const auto list = emby::parseItems(reply.body);
    if (!list) {
        std::cerr << "list: unreadable reply\n";
        return false;
    }
    std::cout << "total " << list->total << ", returned " << list->items.size() << '\n';
    for (std::size_t i = 0; i < list->items.size(); ++i) {
        const auto& item = list->items[i];
        std::cout << i << "  id " << item.id << "  type " << item.type;
        if (!item.playlistItemId.empty()) std::cout << "  place " << item.playlistItemId;
        if (item.childCount >= 0) std::cout << "  holds " << item.childCount;
        if (item.runTimeTicks > 0) std::cout << "  seconds " << emby::secondsFromTicks(item.runTimeTicks);
        std::cout << (emby::isPlayableVideo(item) ? "  playable" : emby::isBrowsable(item) ? "  browsable" : "");
        if (names) std::cout << "  " << item.name;
        std::cout << '\n';
    }
    return true;
}

// Every video's subtitle streams, counted: what a subtitle feature has to
// read on this server. Nothing is printed of any one item but its id.
bool surveySubtitles(EmbyClient& client, const emby::Session& session) {
    // What the account asks the server to choose at playback.
    const auto user = call(client, "GET", "/Users/" + emby::urlEncode(session.userId));
    if (const auto j = user.ok ? nlohmann::json::parse(user.body, nullptr, false) : nlohmann::json();
        j.is_object() && j.contains("Configuration")) {
        const auto& configuration = j["Configuration"];
        std::cout << "account: subtitle mode " << emby::detail::str(configuration, "SubtitleMode")
                  << ", subtitle language '" << emby::detail::str(configuration, "SubtitleLanguagePreference")
                  << "', audio language '" << emby::detail::str(configuration, "AudioLanguagePreference") << "'\n";
    }
    struct Count {
        int streams{};
        int items{};
        std::string example;
    };
    std::map<std::string, Count> counts;
    int videos = 0, withAny = 0, withText = 0, onlyBitmap = 0, withDefault = 0;
    for (int start = 0;; start += 500) {
        emby::ItemsQuery query;
        query.includeTypes = "Movie,Episode,Video,MusicVideo";
        query.recursive = true;
        query.fields = "MediaStreams";
        query.startIndex = start;
        query.limit = 500;
        const auto reply = call(client, "GET", emby::itemsPath(session.userId, query));
        if (!reply.ok) {
            std::cerr << "survey: " << reply.status << ' ' << reply.error << '\n';
            return false;
        }
        const auto page = nlohmann::json::parse(reply.body, nullptr, false);
        if (page.is_discarded() || !page.contains("Items") || !page["Items"].is_array()) {
            std::cerr << "survey: unreadable reply\n";
            return false;
        }
        const auto& items = page["Items"];
        for (const auto& item : items) {
            ++videos;
            const auto streams = item.find("MediaStreams");
            if (streams == item.end() || !streams->is_array()) continue;
            bool any = false, text = false, isDefault = false;
            std::map<std::string, bool> seen;
            for (const auto& entry : *streams) {
                const auto stream = emby::detail::streamFromJson(entry);
                if (stream.type != "Subtitle") continue;
                any = true;
                text = text || stream.isTextSubtitle;
                isDefault = isDefault || stream.isDefault;
                const std::string key = (stream.codec.empty() ? "?" : stream.codec) +
                    std::string(stream.isExternal ? " beside the file" : " in the file") +
                    (stream.isTextSubtitle ? " (text)" : " (bitmap)");
                auto& count = counts[key];
                ++count.streams;
                if (!seen[key]) {
                    seen[key] = true;
                    ++count.items;
                    if (count.example.empty()) count.example = emby::detail::str(item, "Id");
                }
            }
            withAny += any;
            withText += text;
            onlyBitmap += any && !text;
            withDefault += isDefault;
        }
        const int total = static_cast<int>(emby::detail::integer(page, "TotalRecordCount", 0));
        if (items.empty() || start + 500 >= total) break;
    }
    std::cout << "videos " << videos << ", with subtitles " << withAny << ", with a text stream " << withText
              << ", bitmap only " << onlyBitmap << ", with a default stream " << withDefault << '\n';
    for (const auto& [key, count] : counts) {
        std::cout << "  " << key << ": " << count.streams << " streams in " << count.items
                  << " videos, e.g. item " << count.example << '\n';
    }
    return true;
}

// What the browser marks as having subtitles, against what the server says
// of each video's streams: every video is listed with its MediaStreams, and
// then asked about by id the way the browser asks (emby::subtitleBatches,
// emby::subtitledItemsPath). Also whether a list carries HasSubtitles at
// all. Nothing is printed of any one item but its id.
bool compareSubtitleFlags(EmbyClient& client, const emby::Session& session) {
    struct Seen {
        bool flagged{}, anyStream{}, textStream{};
    };
    std::map<std::string, Seen> seen;
    std::size_t listBytes = 0;
    for (int start = 0;; start += 500) {
        emby::ItemsQuery query;
        query.includeTypes = "Movie,Episode,Video,MusicVideo";
        query.recursive = true;
        query.fields = std::string(emby::kBrowserItemFields) + ",MediaStreams";
        query.startIndex = start;
        query.limit = 500;
        const auto reply = call(client, "GET", emby::itemsPath(session.userId, query));
        if (!reply.ok) {
            std::cerr << "flags: " << reply.status << ' ' << reply.error << '\n';
            return false;
        }
        listBytes += reply.body.size();
        const auto page = nlohmann::json::parse(reply.body, nullptr, false);
        if (page.is_discarded() || !page.contains("Items") || !page["Items"].is_array()) {
            std::cerr << "flags: unreadable reply\n";
            return false;
        }
        const auto& items = page["Items"];
        for (const auto& item : items) {
            auto& entry = seen[emby::detail::str(item, "Id")];
            entry.flagged = item.contains("HasSubtitles");
            const auto streams = item.find("MediaStreams");
            if (streams == item.end() || !streams->is_array()) continue;
            for (const auto& raw : *streams) {
                const auto stream = emby::detail::streamFromJson(raw);
                if (stream.type != "Subtitle") continue;
                entry.anyStream = true;
                entry.textStream = entry.textStream || stream.isTextSubtitle;
            }
        }
        const int total = static_cast<int>(emby::detail::integer(page, "TotalRecordCount", 0));
        if (items.empty() || start + 500 >= total) break;
    }
    std::vector<std::string> ids;
    int flagged = 0, withStreams = 0, withText = 0;
    for (const auto& [id, entry] : seen) {
        ids.push_back(id);
        flagged += entry.flagged;
        withStreams += entry.anyStream;
        withText += entry.textStream;
    }
    std::cout << "videos " << ids.size() << " (" << listBytes << " bytes with MediaStreams), HasSubtitles in "
              << flagged << "; with subtitle streams " << withStreams << ", with a text stream " << withText << '\n';
    std::set<std::string> answered;
    std::size_t requests = 0, bytes = 0, largest = 0;
    for (const auto& batch : emby::subtitleBatches(ids)) {
        const auto reply = call(client, "GET", emby::subtitledItemsPath(session.userId, batch));
        const auto list = reply.ok ? emby::parseItems(reply.body) : std::nullopt;
        if (!list) {
            std::cerr << "flags: " << reply.status << ' ' << reply.error << '\n';
            return false;
        }
        ++requests;
        bytes += reply.body.size();
        largest = std::max(largest, reply.body.size());
        for (const auto& item : list->items) answered.insert(item.id);
    }
    int markedWithout = 0, textUnmarked = 0;
    for (const auto& [id, entry] : seen) {
        const bool marked = answered.contains(id);
        if (marked && !entry.textStream) {
            std::cout << "  marked without a text stream: " << id << (entry.anyStream ? " (pictures only)" : "") << '\n';
            ++markedWithout;
        }
        if (!marked && entry.textStream) {
            std::cout << "  text stream, not marked: " << id << '\n';
            ++textUnmarked;
        }
    }
    std::cout << "asked by id: " << requests << " requests, " << bytes << " bytes (largest " << largest << "), "
              << answered.size() << " marked; marked without a text stream " << markedWithout
              << ", text stream not marked " << textUnmarked << '\n';
    return markedWithout == 0 && textUnmarked == 0;
}

// One item's subtitle streams, and each text one fetched as the player does.
bool printSubtitleStreams(EmbyClient& client, const emby::Session& session, const std::string& itemId,
                          bool names, const std::filesystem::path& saveTo) {
    const auto reply = call(client, "GET", emby::itemPath(session.userId, itemId));
    if (!reply.ok) {
        std::cerr << "item: " << reply.status << ' ' << reply.error << '\n';
        return false;
    }
    const auto item = nlohmann::json::parse(reply.body, nullptr, false);
    if (item.is_discarded() || !item.contains("MediaSources") || !item["MediaSources"].is_array() ||
        item["MediaSources"].empty()) {
        std::cerr << "item: no media source in the reply\n";
        return false;
    }
    const auto& source = item["MediaSources"][0];
    const std::string sourceId = emby::detail::str(source, "Id");
    std::cout << "item " << itemId << " source " << sourceId << " container "
              << emby::detail::str(source, "Container") << " default subtitle "
              << emby::detail::integer(source, "DefaultSubtitleStreamIndex", -1) << '\n';
    // What the server would choose for this account at playback: the same
    // reply the player reads, asked for with GET so that nothing is opened.
    const auto playback = call(client, "GET", emby::playbackInfoPath(itemId, session.userId));
    if (const auto info = playback.ok ? emby::parsePlaybackInfo(playback.body) : std::nullopt) {
        if (const auto* chosen = emby::directPlaySource(*info)) {
            std::cout << "  playback info: default subtitle " << chosen->defaultSubtitleStreamIndex
                      << ", default audio " << chosen->defaultAudioStreamIndex << '\n';
        }
    } else {
        std::cout << "  playback info: " << playback.status << ' ' << playback.error << '\n';
    }
    const auto streams = source.find("MediaStreams");
    if (streams == source.end() || !streams->is_array()) return true;
    bool ok = true;
    for (const auto& entry : *streams) {
        const auto stream = emby::detail::streamFromJson(entry);
        if (stream.type != "Subtitle" && stream.type != "Video" && stream.type != "Audio") continue;
        std::cout << "  stream " << stream.index << ' ' << stream.type << ' ' << stream.codec << ' '
                  << stream.language << (stream.isExternal ? " external" : " internal")
                  << (stream.isDefault ? " default" : "") << (stream.isForced ? " forced" : "");
        if (stream.type != "Subtitle") { std::cout << '\n'; continue; }
        std::cout << (stream.isTextSubtitle ? " text" : " bitmap");
        if (names) {
            const auto file = std::filesystem::path(utf8ToWideText(emby::detail::str(entry, "Path"))).filename();
            std::cout << "  [" << stream.displayTitle << "] [" << emby::detail::str(entry, "Title") << "] ["
                      << wideToUtf8Text(file.wstring()) << "]";
        }
        if (!stream.isTextSubtitle) { std::cout << '\n'; continue; }
        // As SRT, the request the player makes, and in the stream's own
        // format when that is ASS, which keeps what the conversion drops.
        std::vector<std::string> formats{"srt"};
        if (stream.codec == "ass" || stream.codec == "ssa") formats.push_back("ass");
        for (const auto& format : formats) {
            const auto started = std::chrono::steady_clock::now();
            const auto text = call(client, "GET", emby::subtitlePath(itemId, sourceId, stream.index, format));
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            if (!text.ok) {
                std::cout << "  -> " << format << ' ' << text.status << ' ' << text.error << " after " << ms << " ms";
                ok = false;
                continue;
            }
            const std::string decoded = decodeSubtitleBytes(text.body);
            const auto track = parseSubtitles(decoded);
            const auto atTop = track ? std::count_if(track->cues.begin(), track->cues.end(),
                                                     [](const SubtitleCue& cue) { return cue.top; }) : 0;
            std::cout << "  -> " << format << ' ' << text.body.size() << " bytes, "
                      << (track ? track->cues.size() : 0) << " cues (" << atTop << " at the top), " << ms << " ms"
                      << (decoded.size() != text.body.size() ? ", UTF-16" : "")
                      << (isUtf8Text(decoded) ? "" : ", NOT UTF-8");
            if (!saveTo.empty()) {
                std::ofstream out(saveTo / (itemId + "-" + std::to_string(stream.index) + "." + format),
                                  std::ios::binary);
                out.write(text.body.data(), static_cast<std::streamsize>(text.body.size()));
            }
        }
        std::cout << '\n';
    }
    return ok;
}

int probeWithSavedSignIn(int argc, char** argv) {
    const std::filesystem::path authPath = argv[2];
    std::wstring text;
    if (!readUtf8File(authPath, text)) {
        std::cerr << "cannot read " << authPath.string() << '\n';
        return 1;
    }
    std::wistringstream input(text);
    input.imbue(std::locale::classic());
    emby::StoredAuth stored;
    if (!emby::readStoredAuth(input, stored)) {
        std::cerr << "not a sign-in file\n";
        return 1;
    }
    emby::Session session;
    session.serverUrl = stored.serverUrl;
    session.serverId = stored.serverId;
    session.serverName = stored.serverName;
    session.userId = stored.userId;
    session.userName = stored.userName;
    session.deviceId = stored.deviceId;
    session.token = EmbyClient::unprotectSecret(stored.protectedToken);
    if (!session.signedIn()) {
        std::cerr << "the file holds no sign-in this account can read\n";
        return 1;
    }
    EmbyClient client;
    client.configure(session);
    std::cout << "signed in as " << session.userName << " on " << session.serverUrl << '\n';

    int limit = 20;
    bool names = false;
    std::filesystem::path saveTo;
    for (int i = 3; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--limit" && i + 1 < argc) limit = std::max(1, std::atoi(argv[++i]));
        else if (argument == "--names") names = true;
        else if (argument == "--save-subtitles" && i + 1 < argc) saveTo = argv[++i];
    }
    bool listed = false;
    bool ok = true;
    for (int i = 3; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--limit" || argument == "--save-subtitles") {
            ++i;
        } else if (argument == "--names") {
            continue;
        } else if ((argument == "--playlist" || argument == "--folder" || argument == "--flat") && i + 1 < argc) {
            emby::ListPage page;
            page.kind = argument == "--playlist" ? emby::ListKind::Playlist : emby::ListKind::Folder;
            page.id = argv[++i];
            page.itemType = argument == "--playlist" ? "Playlist" : "Folder";
            emby::ListArrangement arrangement;
            arrangement.flat = argument == "--flat";
            ok = printList(client, session, page, arrangement, limit, names) && ok;
            listed = true;
        } else if (argument == "--subtitles") {
            ok = surveySubtitles(client, session) && ok;
            listed = true;
        } else if (argument == "--subtitle-flags") {
            ok = compareSubtitleFlags(client, session) && ok;
            listed = true;
        } else if (argument == "--subtitle-streams" && i + 1 < argc) {
            ok = printSubtitleStreams(client, session, argv[++i], names, saveTo) && ok;
            listed = true;
        } else if (argument == "--library" && i + 2 < argc) {
            emby::ListPage page;
            page.kind = emby::ListKind::Library;
            page.id = argv[++i];
            page.collectionType = argv[++i];
            ok = printList(client, session, page, emby::ListArrangement{}, limit, names) && ok;
            listed = true;
        } else {
            std::cerr << "unknown argument " << argument << '\n';
            return 2;
        }
    }
    if (!listed) printViews(client, session);
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--auth") return probeWithSavedSignIn(argc, argv);
    if (argc < 3) {
        std::cerr << "usage: QuadDeckEmbyProbe <server-url> <user> [password] [--save <path>]\n"
                     "       QuadDeckEmbyProbe --auth <emby.qauth> [--playlist <id>] [--folder <id>] [--flat <id>]\n"
                     "                         [--library <id> <collection type>] [--limit <n>] [--names]\n"
                     "                         [--subtitles] [--subtitle-flags] [--subtitle-streams <item id>]\n"
                     "                         [--save-subtitles <folder>]\n";
        return 2;
    }
    std::string password;
    std::string savePath;
    for (int i = 3; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--save" && i + 1 < argc) savePath = argv[++i];
        else password = argument;
    }
    EmbyClient client;
    emby::Session session;
    session.serverUrl = emby::normalizeServerUrl(argv[1]);
    session.deviceId = EmbyClient::newDeviceId();
    client.configure(session);

    const auto info = call(client, "GET", emby::publicInfoPath());
    if (!info.ok) { std::cerr << "public info: " << info.error << '\n'; return 1; }
    const auto server = emby::parseServerInfo(info.body);
    if (!server) { std::cerr << "public info: unreadable reply\n"; return 1; }
    std::cout << "server " << server->name << " " << server->version << " id " << server->id << '\n';
    session.serverId = server->id;
    session.serverName = server->name;
    client.configure(session);

    const auto signIn = call(client, "POST", emby::authenticatePath(), emby::authenticateBody(argv[2], password));
    if (!signIn.ok) { std::cerr << "sign-in: " << signIn.error << '\n'; return 1; }
    const auto auth = emby::parseAuthentication(signIn.body);
    if (!auth) { std::cerr << "sign-in: unreadable reply\n"; return 1; }
    session.token = auth->token;
    session.userId = auth->userId;
    session.userName = auth->userName;
    client.configure(session);
    std::cout << "signed in as " << session.userName << " user " << session.userId << '\n';

    printViews(client, session);

    if (!savePath.empty()) {
        emby::StoredAuth stored;
        stored.serverUrl = session.serverUrl;
        stored.serverId = session.serverId;
        stored.serverName = session.serverName;
        stored.userId = session.userId;
        stored.userName = session.userName;
        stored.deviceId = session.deviceId;
        stored.protectedToken = EmbyClient::protectSecret(session.token);
        if (stored.protectedToken.empty()) { std::cerr << "could not protect the token\n"; return 1; }
        if (EmbyClient::unprotectSecret(stored.protectedToken) != session.token) {
            std::cerr << "protected token does not round-trip\n";
            return 1;
        }
        std::error_code code;
        std::filesystem::create_directories(std::filesystem::path(savePath).parent_path(), code);
        if (!writeUtf8FileAtomically(savePath, [&](std::wostream& out) { return emby::writeStoredAuth(out, stored); })) {
            std::cerr << "could not write " << savePath << '\n';
            return 1;
        }
        std::cout << "saved " << savePath << '\n';
    } else {
        call(client, "POST", emby::logoutPath());
        std::cout << "logged out\n";
    }
    return 0;
}
