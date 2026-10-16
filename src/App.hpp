#pragma once

#include "AssSubtitles.hpp"
#include "AudioOutput.hpp"
#include "Core.hpp"
#include "D3DRenderer.hpp"
#include "EmbyClient.hpp"
#include "EmbyPlayback.hpp"
#include "MainThreadQueue.hpp"
#include "Subtitles.hpp"
#include "Thumbnails.hpp"
#include "LocalPlaylist.hpp"
#include "LocalThumbnails.hpp"
#include "FileAssociations.hpp"
#include "HangWatchdog.hpp"
#include "Overlay.hpp"
#include "OverlayLayout.hpp"
#include "Session.hpp"
#include "SettingsPanel.hpp"
#include "SingleInstance.hpp"
#include "SourceReaper.hpp"
#include "SyncState.hpp"
#include "VideoSource.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

namespace quaddeck {

// How a timeline move trades latency against precision.
//
// Synchronized freezes the clock until every pane has decoded its own exact
// destination frame, so playback resumes with all participating panes aligned.
// The keyframe interval bounds this latency: useful when starting playback,
// while timeline scrubbing needs the faster presentation path below.
//
// Fast keeps the clock running: each pane shows the nearest keyframe at once
// and converges on the exact position as it decodes. Panes disagree for a
// moment afterwards, which is the price of the timeline responding
// immediately to a drag.
enum class SeekStyle { Synchronized, Fast };

// The one-line boxes typed into where the sheet draws them
// (AppTextFields.cpp): the Emby browser's search and the Emby sign-in's
// server, user and password.
enum class TextFieldId { None, EmbySearch, EmbyServer, EmbyUser, EmbyPassword };

class App {
public:
    int run(HINSTANCE instance, int showCommand, const std::vector<std::wstring>& initialFiles,
            SingleInstance* singleInstance = nullptr);

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK videoWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    // A short on-screen notice for actions that otherwise only change the
    // title bar: audio selection, speed, mute, volume, seeks from the keyboard.
    void showNotice(const std::wstring& text);
    void expireNotice(ULONGLONG now);
    void toggleMute();
    void toggleSolo(int pane);
    // Setting A arms the master loop to the end of the timeline until B
    // narrows it; both report what they did as a notice.
    void setMasterLoopA(double seconds);
    void setMasterLoopB(double seconds);
    // DPI. Every pixel constant in AppInternal.hpp is a 96-DPI value; dp()
    // converts one to this window's pixels. applyDpi re-derives the scale,
    // fonts and layout when the window moves to a monitor with another DPI.
    int dp(int logical) const {
        return static_cast<int>(std::lround(logical * uiScale_));
    }
    void applyDpi(unsigned dpi);
    // The drawn interface (AppChrome.cpp).
    // The pill's video number, e.g. "V1". The only video has no pill.
    std::wstring paneLabel(std::size_t pane) const;
    // How much of the cell's bottom the transport bar covers now, or -- with
    // `settled` -- will cover once it is fully in.
    float paneCoveredBottom(const RectF& cell, bool settled = false) const;
    void refreshChromeGeometry(const RECT& client);
    OverlayHit hitTestAt(POINT local) const;
    void buildOverlayScene();
    // PanelScroll drags the sheet's content; PanelScrollbar drags its thumb.
    enum class ControlDrag { None, MasterSeek, PaneSeek, Volume, PaneVolume, PanelSlider, PanelScroll, PanelScrollbar };
    void beginControlDrag(ControlDrag kind, int pane, POINT local);
    void updateControlDrag(POINT local);
    void endControlDrag(bool commit);
    void activateBarItem(BarItem item);
    void showBarSubtitleMenu();
    void activateChip(std::size_t pane, PaneChip chip);
    // The window caption drawn in place of the Windows title bar, in a
    // window and in fullscreen alike, as PotPlayer's: up with the bar or the
    // sheet. In fullscreen it moves and sizes no window.
    float captionAlpha() const;
    // How far down the caption's strip reaches. The sheet and the pane
    // pills keep below it.
    float captionInset() const;
    // A point in the video window on the caption's strip while it is up.
    bool onCaptionStrip(POINT local) const;
    void activateCaptionItem(CaptionItem item);
    // The empty window's drop hint and its Open Emby button: there while
    // nothing is loaded, the button only while signed in to Emby; it fades
    // under the sheet and answers the pointer only while no sheet takes it.
    bool emptyHintShown() const;
    bool emptyEmbyButtonActive() const;
    // The control at `local` whose clicks add up or set what is under the
    // pointer: an Offset, delay or zoom stepper, Previous and Next, a rail
    // or slider. The second of two quick clicks there is a press again.
    // Anything that opens, plays, navigates or flips has none, so a
    // double-click on it still acts once.
    struct RepeatControl {
        int area{};
        int kind{};
        int id{};
        int part{-1};
        int param{-1};
        bool operator==(const RepeatControl&) const = default;
    };
    std::optional<RepeatControl> repeatControlAt(POINT local) const;
    // What WM_NCHITTEST answers for a point in the client: the top resize
    // band and the caption's drag area, HTCLIENT elsewhere and everywhere
    // in fullscreen.
    LRESULT frameHitTest(POINT screen) const;
    // A video dragged where no pane can take it moves the window instead.
    bool paneDragMovesWindow() const;
    // The settings sheet (AppPanel.cpp).
    void toggleSettingsPanel();
    void closeSettingsPanel();
    // The sheet is settings, the Emby browser or a folder's list in turn;
    // each keeps its own place, left where it was when it comes back.
    float& sheetScrollSlot();
    void rememberSheetScroll();
    void restoreSheetScroll();
    // F6: the Emby browser, docked beside a playing video like PotPlayer's
    // playlist, or the whole window when nothing plays.
    void toggleEmbyBrowser();
    // What the F6 sheet lists: the Emby library, or the folder of the only
    // local video (AppLocal.cpp).
    enum class BrowserSource { Emby, Local };
    void openLocalList();
    void refreshLocalList(const std::wstring& path, bool force = false);
    void applyLocalList(std::uint64_t serial, std::wstring directory, std::vector<LocalEntry> entries, std::wstring error = {});
    void showLocalToolbarMenu(int part);
    void retryLocalList();
    void embyToolbarAction(int part);
    void applyLocalProbes(std::uint64_t serial, std::wstring directory,
                          std::vector<std::pair<std::wstring, LocalProbe>> batch);
    static LocalProbe probeLocalFile(const std::wstring& path, const std::atomic<std::uint64_t>& token,
                                     std::uint64_t serial);
    void orderLocalList();
    // What a browser list row's marks come from besides its own entry,
    // gathered once a frame: the items and files being played, and whether
    // a search still reads (its items take no press until it is whole).
    struct ListRowMarks {
        std::set<std::string> embyPlaying;
        PaneArray<int> localPlaying{};
        bool embyItemsEnabled{true};
    };
    std::vector<PanelRow> buildLocalRows(float sheetWidth, std::vector<PanelListRun>* runs = nullptr) const;
    void fillLocalRowContent(PanelRow& row, const ListRowMarks& marks) const;
    void localNavigate(int part);
    void localSetView(int part);
    void activateLocalItem(int index, bool add = false);
    void localPlayItem(const std::wstring& path, const std::vector<LocalEntry>& queue,
                       std::size_t pane, bool add = false, bool selectTarget = true);
    void localCancelReplacement();
    // Explorer/command-line opens retain launch boundaries through the
    // replacement chooser. Only this entry point deduplicates local paths;
    // ordinary F6 Add may still open the same file in several panes.
    void enqueueExternalFiles(std::vector<std::wstring> files, SingleInstance::RequestId id = {},
                              bool startup = false);
    void drainExternalRequests();
    void openExternalDeck();
    void processExternalFiles();
    void completeExternalBatch();
    int externalLocalPane(const std::wstring& path) const;
    void focusExternalPane(std::size_t pane);
    // A subtitle file from Explorer is shown on the video it belongs to by
    // the sidecar rule, else on the pane subtitle keys act on. A video of
    // its name still queued, or arriving shortly after (Explorer starts one
    // launch per file), takes it when it fits better than any open one.
    // Into an empty deck it brings that video from its folder, read off this
    // thread.
    int externalSubtitleVideoPane(const std::wstring& path, std::size_t* length = nullptr) const;
    bool deferExternalSubtitle(const std::wstring& path, std::size_t openLength);
    void openExternalSubtitle(const std::wstring& path);
    void findExternalSubtitleVideo(const std::wstring& requested, const std::wstring& subtitle);
    void applyExternalSubtitleVideo(std::uint64_t serial, const std::wstring& requested,
                                    const std::wstring& subtitle, const std::wstring& video);
    void activateExternalWindow();
    void loadExternalLocalQueue(std::size_t pane, const std::wstring& path,
                                emby::SortKey sort, bool descending);
    void localApplyAdoptedQueue(std::uint64_t serial, const std::wstring& path, std::vector<LocalEntry> queue);
    void localConfirmReplacement(std::size_t pane);
    std::vector<PanelRow> buildLocalReplacementRows(float sheetWidth) const;
    static std::vector<LocalEntry> scanLocalFolder(const std::wstring& directory, std::wstring* error = nullptr);
    // Explorer's file types: registered at the user's request only.
    void refreshFileTypesStatus();
    void fileTypesAction(int part);
    // Docked only with something to browse: the sign-in page is a sheet
    // like the settings, over the videos.
    bool embyBrowserDocked() const;
    float embyDockWidth(float clientWidth) const;
    // The width the transport bar spans: the client, or what the docked
    // browser leaves uncovered, so its buttons stay reachable. The video
    // itself is never shrunk: the browser lies over its right part.
    float videoAreaWidth(float clientWidth) const;
    // The rows of the sheet for a client this wide: the browser's grid
    // takes as many tiles per line as fit. Given `runs`, a browser list's
    // rows are left out and described there (PanelListRun) for
    // refreshPanelGeometry to keep and fill near the screen; without, every
    // row comes whole.
    std::vector<PanelRow> buildSettingsRows(float clientWidth, std::vector<PanelListRun>* runs = nullptr) const;
    ListRowMarks listRowMarks() const;
    void fillListRow(PanelRow& row, const ListRowMarks& marks) const;
    std::vector<PanelRow> withListRuns(std::vector<PanelRow> rows, std::vector<PanelListRun> listRuns,
                                       std::vector<PanelListRun>* runs) const;
    void composePanelRows(std::vector<PanelRow> rows, std::vector<PanelListRun> runs);
    void fillListWindow();
    void refreshPanelGeometry(const RECT& client);
    // Whether the sheet takes the pointer: only while it is open and
    // drawn. Closing, it still fades out for a moment, but a click, a
    // right-click, a double-click or the wheel then reaches what lies
    // beneath; the fade (settingsAlpha_) is for the painting alone.
    bool sheetTakesPointer() const;
    PanelHit panelHitAt(POINT local) const;
    void scrollSettingsPanel(int wheelDelta);
    void applyPanelSlider(int rowIndex, float fraction);
    void activatePanelHit(const PanelHit& hit);
    unsigned showPanelMenu(HMENU menu, SettingId id, int part, int param = -1);
    void selectSettingsTab(SettingsTab tab);
    // The header's switches in their order, each the face it leads to; a
    // press on the `part`-th shows that face. Emby is shown as Ctrl+E shows
    // it (the sign-in dialog when signed out).
    std::vector<SheetFace> sheetSwitches() const;
    void switchSheet(int part);
    void showSheetFace(SheetFace face);
    // Emby (AppEmby.cpp): one page of the browser, and what a pane playing
    // an Emby item remembers for its progress reports.
    struct EmbyDetails;
    struct EmbyPage {
        enum class Kind { Home, Library, Series, Season, Folder, Search, Playlist, Favorites, Person };
        Kind kind{Kind::Home};
        std::string id, title, seriesId, collectionType, term;
        // What was opened to get here: Folder, BoxSet, PhotoAlbum, ...
        std::string itemType;
        // A playlist opens in the order it was put together; choosing one
        // of the browser's orders on its page gives that up for the page.
        bool ownOrder{true};
        // Where the page was left when another was opened from it.
        float scroll{};
        // A Search page's box: what has been typed there, which becomes
        // `term` once the typing pauses; kept with the page, so Back to it
        // finds the box as it was left.
        TextFieldState field;
        // A Person page's person: first what the credit said (name,
        // picture), then their own item (birth, biography) once it comes;
        // `expanded` is its biography's More / Less.
        emby::Item person;
        bool expanded{};
        // The information page a Person page was opened from, as it was
        // left, which Back from the person returns to.
        std::shared_ptr<const EmbyDetails> fromDetails;
    };
    struct EmbyPane {
        std::string itemId, mediaSourceId, playSessionId, seriesId, seasonId, parentId;
        std::string serverId, serverUrl, userId;
        std::string imageKey;
        emby::PlaybackQueue queue;
        std::uint64_t accountSerial{};
        std::uint64_t queueReorderSerial{};
        std::wstring title;
        std::int64_t runTimeTicks{};
        std::int64_t resumeTicks{};
        std::vector<emby::MediaStream> streams;
        int subtitleStream{-1};
        // The account's favorite mark as the item reply gave it, then as
        // QuadDeck set it; nothing until the item has come.
        std::optional<bool> favorite;
        bool photo{};
        bool autoplay{};
        bool startPlaying{};
        bool endHandled{};
        bool addedMuted{};  // automatic handoff cannot enable a newly added pane
        bool fromBeginning{};  // explicit details action wins over a fresh resume position
        // True from the item request until the stream opens or fails; the
        // deck treats such a pane as a source still opening.
        bool resolving{};
        bool started{};
        bool lastPaused{};
        std::uint64_t serial{};
        std::uint64_t embyAccountSerial{};
        std::chrono::steady_clock::time_point lastReport{};
    };
    struct EmbyPendingPlay {
        emby::Item item;
        emby::PlaybackQueue queue;
        std::uint64_t accountSerial{};
        bool fromBeginning{};
        float scroll{};
        PaneArray<std::wstring> paths;
        PaneArray<std::uint64_t> mediaSerials{};
    };
    struct LocalPanePlayback {
        std::vector<LocalEntry> queue;
        bool endHandled{};
        bool addedMuted{};
        bool suppressInheritedRepeat{};
        std::uint64_t queueLoadSerial{};
    };
    struct LocalPendingPlay {
        std::wstring path;
        std::vector<LocalEntry> queue;
        bool external{};
        bool sheetOpen{};
        bool browserOpen{};
        bool browserByEdge{};
        BrowserSource browserSource{BrowserSource::Local};
        bool queueNeedsLoad{};
        emby::SortKey queueSort{emby::SortKey::Name};
        bool queueDescending{};
        float scroll{};
        PaneArray<std::wstring> paths;
        PaneArray<std::uint64_t> mediaSerials{};
    };
    // A details page is layered over an unchanged browser list. Its own
    // requests cannot replace the source list or a playlist already playing.
    struct EmbyDetails {
        emby::Item item;
        std::string libraryId;
        std::vector<emby::Item> sourceItems;
        int sourceChosen{-1};
        std::string sourcePage;
        float sourceScroll{};
        float scroll{};
        bool loading{};
        bool expanded{};
        // Every one of its cast and crew shown, not only the first lines.
        bool castExpanded{};
        std::wstring error;
        std::vector<emby::Item> seasons, episodes;
        std::string seasonId;
        std::string preferredSeasonId;
        bool playlistAdopted{};
        bool childrenLoading{};
        bool childrenBlocked{};
        int childrenTotal{};
        std::wstring childrenError;
        std::uint64_t serial{};
    };
    std::filesystem::path embyAuthPath() const;
    void embyLoadAuth();
    void embySaveAuth() const;
    std::wstring embyStatusLine() const;
    std::vector<PanelRow> buildEmbyRows(float clientWidth, std::vector<PanelListRun>* runs = nullptr) const;
    void fillEmbyRowContent(PanelRow& row, const ListRowMarks& marks) const;
    void embyConfigure(const emby::Session& session);
    // The browser's arrangement: view, order, filters. A change of order or
    // filter asks the server again; a change of view only redraws.
    void embySetView(int view);
    void embySetSort(int sort);
    // A segment of the Sort row: on a playlist's page the first one is the
    // playlist's own order and the browser's orders follow it.
    void embyChooseSort(int part);
    // PotPlayer's playlist sort keys: the browser's order, and the list
    // being played re-sorted at once, the same key again reversing it.
    void sortListBy(int sort);
    // The list on screen becomes the list being played when it holds the
    // playing item and is the page that item was chosen from, or when
    // nothing was chosen from a list at all.
    void embyAdoptPlaylist();
    // The playable part of `list` becomes the list being played, the entry
    // at `chosen` (an index into `list`, -1 for none) the one playing.
    void embyTakePlaylist(const std::vector<emby::Item>& list, int chosen, std::size_t pane,
                          const std::string& page = {});
    // Where the playing item is in the list being played, -1 when it is not.
    int embyPlaylistIndex(const std::string& itemId, std::size_t pane) const;
    // Plays the entry of the list being played at `index`.
    void embyPlayFromPlaylist(int index, std::size_t pane, bool selectTarget = true);
    // Re-orders the list being played; the entry playing stays the one playing.
    void embyReorderPlaylist(const std::function<void(std::vector<emby::Item>&)>& order);
    void embyToggleUnplayed();
    // Folders, or every video under the page with its folders left out.
    void embySetFlat(bool flat);
    void embyToggleFlat();
    bool embyPageAcceptsFlat() const;
    bool embyPageAcceptsFilters() const;
    // Lists and TV hierarchy pages are asked for page by page.
    bool embyPageIsPaged() const;
    // What tells one page from another: the list being played remembers
    // the page it was taken from.
    static std::string embyPageKey(const EmbyPage& page);
    static emby::ListPage embyListPage(const EmbyPage& page);
    // The request a list or TV hierarchy page makes, without the server.
    std::string embyListPath(const EmbyPage& page, int startIndex, int limit) const;
    std::string embySearchPath(const std::string& libraryId, const std::string& term) const;
    // A list reply for the page shown, `startIndex` on from its start, read
    // here for the home page's immediate reply.
    void embyListArrived(std::uint64_t serial, int startIndex, const EmbyClient::Response& response);
    // What tells one list reply from another without reading it, kept for
    // the reply the list shown came from: a refresh that brings the same
    // bytes is not read again.
    struct EmbyListReplyPrint {
        std::size_t hash{}, bytes{}, items{};
        int total{};
    };
    struct EmbyListParse {
        std::optional<emby::ItemList> list;
        std::size_t hash{}, bytes{};
        // The same bytes as the list shown came from: not read; the body is
        // kept should the list have moved on meanwhile.
        bool unchanged{};
        std::string body;
    };
    static EmbyListParse embyParseList(std::string body, const std::optional<EmbyListReplyPrint>& shown);
    // A list page's reply: a failure is answered here, a body read off the
    // window thread and taken by embyListParsed while its serial is current.
    void embyReceiveList(std::uint64_t serial, int startIndex, EmbyClient::Response& response);
    void embyListParsed(std::uint64_t serial, int startIndex, const EmbyClient::Response& response,
                        EmbyListParse parsed);
    // A search asks every library at once; what the answers so far came to.
    struct EmbySearch {
        std::size_t libraries{};
        std::size_t remaining{};
        std::size_t failed{};
        std::string firstError;
        std::set<std::string> seen;
        emby::SortKey sortKey{};
    };
    // One library's answer to a search; the last shows what came, says
    // when some libraries could not be searched and fails the page only
    // when none could.
    void embySearchArrived(std::uint64_t serial, const std::shared_ptr<EmbySearch>& search,
                           const std::string& library, const EmbyClient::Response& response);
    // The libraries, kept for this account even when another page has
    // been opened since they were asked for.
    void embyViewsArrived(std::uint64_t serial, std::uint64_t account, const EmbyClient::Response& response);
    // Asks for the next page when the list is scrolled near its end.
    void embyLoadMoreNearEnd();
    // Asks again for the page shown, at the viewer's word: what changed on
    // the server since is shown, and pictures that failed are tried again.
    void embyRefreshShown();
    // Which videos of a list have subtitles to show, asked for by id once
    // the list has come (emby::subtitledItemsPath); `again` asks for those
    // already answered too.
    void embyAskSubtitles(const std::vector<emby::Item>& items, bool again);
    // Sends queued batches while fewer than a few are out; forgets the
    // queued ones of a page that has been left.
    void embyAskNextSubtitles();
    void embyDropSubtitleQuestions();
    void embySubtitlesArrived(std::uint64_t account, const std::vector<std::string>& asked,
                              const EmbyClient::Response& response);
    bool embyHasSubtitles(const emby::Item& item) const;
    // Favorites: an item marked or unmarked for this account, the server's
    // answer applied to every copy of the item QuadDeck holds -- lists,
    // the information page, the panes and their queues -- once it comes.
    // One change per item is out at a time.
    void embySetFavorite(const std::string& itemId, const std::wstring& title, bool favorite);
    void embyFavoriteArrived(std::uint64_t account, const std::string& itemId, const std::wstring& title,
                             bool wanted, const EmbyClient::Response& response);
    void embyApplyFavorite(const std::string& itemId, bool favorite);
    // The item under a right-click in the browser: a list row or tile, or on
    // an information page its title's row or an episode's, anywhere on them;
    // nothing elsewhere, and nothing the favorites page would not list.
    std::optional<emby::Item> embyItemAtHit(const PanelHit& hit) const;
    std::optional<emby::Item> embyItemAt(POINT local) const;
    // A right-click on a browser item offers to mark or unmark it.
    void showEmbyItemMenu(POINT local);
    // Something says the server has news for the page shown -- the window
    // came back to the front, a video was reported started or stopped --
    // so it is asked for again, in place, `delayMs` from now at the
    // earliest; embyTick does the asking when emby::listRefreshDue agrees.
    void embyWantRefresh(unsigned delayMs = 0);
    emby::ListRefreshState embyRefreshState(unsigned long long now) const;
    void embyRefreshWhenDue(unsigned long long now);
    // The item a press is on, empty for none, and the press given up when
    // the row it is on has come to name another item.
    std::string embyPressedItem() const;
    void embyKeepPressOn(const std::string& item);
    // Asks for the pictures of the tiles on screen that are not cached yet.
    void embyRequestVisibleImages();
    void embyAccountAction(int part);
    void embyNavigate(int part);
    // PotPlayer's habit: while an Emby item plays, the pointer at the
    // window's right edge brings the browser up, docked, and it goes again
    // once the pointer has left it for a moment. Called every hover update.
    void embyEdgeHover(POINT local, const RECT& client, bool inside);
    bool pointerOnPlayer(POINT cursor) const;
    void embyActivateItem(int param);
    std::string embyCurrentLibraryId() const;
    std::string embyLibraryType(const std::string& libraryId) const;
    std::string embyItemLibraryId(const emby::Item& item) const;
    void embyRememberLibrary(const emby::Item& item, const std::string& libraryId);
    void embyResolveResumeLibraries();
    // Which library each movie and series of a list from every library
    // (Continue watching, Favorites, a person's titles) is in, for its information page; at
    // most so many asked at once, the next asked as each answer comes for
    // the list shown (embyResolveShownLibraries).
    void embyResolveLibraries(const std::vector<emby::Item>& items);
    void embyResolveShownLibraries();
    void embyLibraryLookupArrived(std::uint64_t account, const emby::Item& item,
                                  const EmbyClient::Response& response);
    void embyToggleLibraryDetails(const std::string& libraryId = {});
    bool embyTryOpenDetails(const emby::Item& item, int param);
    void embyCloseDetails();
    void embyRequestDetails();
    void embyDetailsArrived(std::uint64_t serial, const EmbyClient::Response& response);
    void embyRequestDetailChildren(int startIndex = 0);
    void embyDetailChildrenArrived(std::uint64_t serial, std::uint64_t childSerial,
                                  const std::string& seasonId, int startIndex,
                                  const EmbyClient::Response& response);
    void embyChooseDetailSeason(int index);
    void embyDetailAction(int action);
    void embyDetailEpisodeAction(int index, int action);
    std::vector<PanelRow> buildEmbyDetailRows(float clientWidth) const;
    // People (AppEmbyDetails.cpp). The information page's cast and crew, in
    // emby::castAndCrew's order; a tile's param is its index there.
    std::vector<emby::Person> embyDetailPeople() const;
    // A tile pressed: the person's page, every title of every library that
    // credits them, with Back to this information page.
    void embyOpenPerson(int index);
    // Back from a Person page opened from an information page: that page
    // again as it was left, over the list it was opened from.
    void embyReturnToDetails(const std::shared_ptr<const EmbyDetails>& details);
    // The person's own item for the Person page shown: their picture, birth
    // and biography. Every page of that person takes the reply.
    void embyRequestPerson();
    void embyPersonArrived(std::uint64_t account, const std::string& personId,
                           const EmbyClient::Response& response);
    // The person page's information row; `action` 2 is its More / Less.
    PanelRow embyPersonRow(const EmbyPage& page) const;
    void embyPersonAction(int action);
    void embyLoadMore();
    // Signed out, the Emby side of the sheet is the sign-in page: its boxes
    // filled from the session and the first empty one typed into.
    void embyOpenSignIn();
    // The browser is the sign-in page; and it is on the sheet.
    bool embySignInPage() const;
    bool embySignInShown() const;
    PanelRow embySignInRow(TextFieldId id) const;
    // The page's Sign in button, or Enter in one of its boxes.
    void embySubmitSignIn();
    // The sign-in stopped short: the page says why and `field` takes the
    // typing with what it holds selected, to be typed over.
    void embySignInFailed(std::wstring note, TextFieldId field);
    void embySignIn(const std::string& serverUrl, const std::string& userName, const std::string& password);
    void embyServerInfoArrived(std::uint64_t serial, const std::string& userName,
                               const std::string& password, const EmbyClient::Response& response);
    void embyAuthenticationArrived(std::uint64_t serial, const EmbyClient::Response& response);
    void embySignOut();
    void embySessionExpired();
    void embyFailure(const EmbyClient::Response& response);
    void embyOpenBrowser();
    void embyGoHome();
    // Every item the account marked as a favorite, from every library, as
    // one list that plays and pages like a library's.
    void embyOpenFavorites();
    void embyPushPage(EmbyPage page);
    // `inPlace` asks again for the page shown without emptying it first:
    // the list and its place stay until the reply replaces them.
    void embyRequestPage(int startIndex = 0, bool inPlace = false);
    // The Search page and its box (AppEmbySearch.cpp). Search opens the
    // page, or on it selects what was typed, and puts the typing there.
    void embyOpenSearch();
    // The box is on the sheet, and is being typed into.
    bool embySearchShown() const;
    bool embySearchTyping() const;
    void embyClearSearch();
    PanelRow embySearchRow() const;
    void embySearchEdited();
    // What the box holds becomes the page's term and is searched for,
    // unless it already is; empty, the results go.
    void embySearchCommit();
    // The typing has paused long enough.
    void embySearchWhenDue(unsigned long long now);
    // The text boxes (AppTextFields.cpp). One at most has the typing
    // (typingField_), and keeps it while it is on the sheet: the keys but
    // F5 and F6, the characters and an input method's composition are its,
    // none of them a player shortcut.
    TextFieldState* fieldState(TextFieldId id);
    const TextFieldState* fieldState(TextFieldId id) const;
    bool fieldShown(TextFieldId id) const;
    // The box typed into while it is on the sheet, None otherwise.
    TextFieldId fieldTyping() const;
    static TextFieldId fieldOfRow(const PanelRow& row);
    // The box a hit falls on, its clear button included.
    TextFieldId fieldAt(const PanelHit& hit) const;
    void focusField(TextFieldId id, bool selectAll);
    // A press on the box at `x` (client pixels): the caret goes there.
    void focusFieldAt(TextFieldId id, float x);
    // Typing leaves the box: a search typed and not yet run runs.
    void blurField();
    // The typing's look on a box's row: its focus, composition and caret.
    void fieldDecorate(PanelRow& row, TextFieldId id) const;
    void fieldKey(WPARAM key, bool ctrl, bool shift);
    void fieldCharacter(wchar_t character);
    void fieldEdited(TextFieldId id);
    void fieldCancelComposition();
    const PanelRowGeometry* fieldGeometry(TextFieldId id, const PanelRow** row = nullptr) const;
    // The drawn caret's line in client pixels, measured as it is drawn.
    std::optional<OverlayRect> fieldCaretBox();
    // Tells the input method where the caret is, for its candidate list.
    void fieldPlaceIme();
    // The input method off while the password has the typing, on after.
    void fieldSyncIme();
    void fieldImeStart();
    void fieldImeComposition(LPARAM flags);
    void fieldImeEnd();
    void fieldLostKeyboard();
    // From embyTick: a box gone from the sheet loses the typing, and a
    // search typed and paused long enough runs.
    void fieldsWhenDue(unsigned long long now);
    void embyPlayItem(const emby::Item& item, std::size_t pane, bool fromBeginning = false, bool add = false,
                      bool selectTarget = true);
    void embyRequestPlay(const emby::Item& item, const emby::PlaybackQueue& queue,
                         bool add = false, bool fromBeginning = false);
    void embyAddItem(int param);
    std::size_t embyPlaybackTarget() const;
    void embySelectTarget(std::size_t pane);
    std::wstring embyTargetLabel() const;
    int embyPlayingPane(const std::string& itemId) const;
    bool embyPaneAccountMatches(std::size_t pane) const;
    void embyConfirmReplacement(std::size_t pane);
    void embyCancelReplacement();
    std::vector<PanelRow> buildEmbyReplacementRows(float clientWidth) const;
    void embyBeginResolve(std::size_t pane);
    // The item a pane is to play, as the server has it now.
    void embyItemArrived(std::uint64_t serial, const EmbyClient::Response& response);
    void embyResolveFailed(std::size_t pane, const std::wstring& reason);
    void embyStartResolved(std::size_t pane, const emby::Item& item, const emby::PlaybackInfo& info);
    void embyStartPhoto(std::size_t pane, const emby::Item& item);
    void embyOpenAdjacent(std::size_t pane, int step, bool selectTarget = true);
    std::optional<emby::Report> embyReportForPane(std::size_t pane) const;
    void embyReport(std::size_t pane, const char* kind, const std::string& eventName = {});
    // At exit: drops what is queued on emby_ and not yet sent, then queues
    // every pane's Stopped report, so the drain that follows waits only for
    // the request already out and the reports.
    void embyReportStoppedAtExit();
    // Each pane's origin, from which deckTimeline and the repeat rule follow.
    PaneOrigins paneOrigins() const;
    // True while any F6 or Emby pane gives every pane its own timeline.
    bool perPaneTimelines() const;
    void detachPaneSeekBarrier(std::size_t pane);
    void finishEmbyPanes();
    void embyFinishPane(std::size_t pane);
    void finishLocalPanes();
    void localFinishPane(std::size_t pane);
    void embyTick();
    void embyClearPane(std::size_t pane);
    // A reply finds its pane by the serial it was queued with: a swap may
    // have moved the pane, a new session or a sign-out retired it.
    std::optional<std::size_t> embyPaneWithSerial(std::uint64_t serial) const;
    // Ends every pane's server session before the account changes.
    void embyDetachPlayback();
    // A source not yet ready, or an Emby item still being resolved.
    bool paneOpening(std::size_t pane) const;
    // Everything a pane holds about its media besides the decoder.
    void resetPaneMediaState(std::size_t pane);
    void releasePanelPress(POINT local);
    // Subtitles (AppSubtitles.cpp). A pane's subtitles come from one of
    // three places: a text stream the Emby server hands over, a file -- one
    // found beside a local video or loaded by hand -- or a text stream
    // inside the container, read as the video is. An Emby item's streams
    // inside its file are read so too, from the file as the server sends
    // it; only those beside it, and one the video turns out not to carry,
    // are asked of the server.
    enum class SubtitleKind { None, EmbyStream, File, Embedded };
    struct SubtitleSelection {
        SubtitleKind kind{SubtitleKind::None};
        int stream{-1};              // the server's stream index, or the container's
        std::filesystem::path file;
        friend bool operator==(const SubtitleSelection&, const SubtitleSelection&) = default;
    };
    // One entry of the Subtitles menu.
    struct SubtitleOption {
        SubtitleSelection what;
        std::wstring label;
        SubtitleCandidate candidate;
        // A stream of pictures is listed so the menu can say it is there,
        // but only text is shown.
        bool usable{true};
        // A file that could not be read when it was last tried: still on
        // the menu, but neither the player nor Alt+L picks it.
        bool unreadable{};
    };
    struct SidecarScan {
        std::vector<std::filesystem::path> files;
        // The file among them that suits the wanted languages best, read
        // and parsed; -1 and nothing when there is none.
        int chosen{-1};
        std::shared_ptr<const SubtitleTrack> track;
        // The better ranked files read on the way, and why each was passed
        // over.
        std::vector<std::pair<std::filesystem::path, SubtitleRead>> failed;
    };
    static SidecarScan scanSidecars(const std::wstring& path, const std::vector<std::string>& wanted);
    void applySidecarScan(std::size_t pane, std::uint64_t serial, SidecarScan scan);
    void loadSidecarSubtitles(std::size_t pane, const std::wstring& path);
    // A scan or a read that comes back finds its pane by the serial it left
    // with: a swap may have moved the pane, new media retired the serial.
    std::optional<std::size_t> subtitlePaneWithSerial(std::uint64_t serial) const;
    // What reading a file of the pane's video found when it could not be
    // shown (Lines when it was not tried or could be), and the record of a
    // read: a file that reads now forgets its failure.
    SubtitleRead subtitleFileState(std::size_t pane, const std::filesystem::path& file) const;
    void noteSubtitleRead(std::size_t pane, const std::filesystem::path& file, SubtitleRead read);
    // Fetches an Emby stream's lines: in its own format when that is ASS,
    // otherwise -- and again with `converted` when that fails -- as SRT.
    void embyLoadSubtitle(std::size_t pane, int streamIndex, bool converted = false);
    // The pane still showing that stream of the item the serial names, or
    // nothing when the item, the stream or the choice changed meanwhile.
    std::optional<std::size_t> embySubtitleWanted(std::uint64_t serial, int streamIndex) const;
    // A stream's reply: a 401 and a failure are answered here, a body is
    // read off the window thread and comes back to embySubtitleRead.
    void embySubtitleArrived(std::uint64_t serial, int streamIndex, bool ass, EmbyClient::Response& response);
    // What a reply came to, on the window thread: the track, a notice, or
    // for an ASS request that gave nothing the same stream as SRT.
    void embySubtitleRead(std::size_t pane, int streamIndex, bool ass, SubtitleReadResult read,
                          const std::string& error);
    std::vector<SubtitleOption> subtitleOptions(std::size_t pane) const;
    // An Emby item's text stream inside its file, which the source reads:
    // as the server lists it until the source is open, then as the source
    // found it.
    bool embyStreamReadWithVideo(std::size_t pane, const emby::MediaStream& stream) const;
    // Shows the option chosen, or none. `byViewer` marks a choice made at
    // the menu or the keyboard, which nothing found later may replace; a
    // subtitle the viewer chooses also turns hidden lines back on.
    void selectSubtitle(std::size_t pane, const SubtitleSelection& selection, bool byViewer);
    // What the player picks when the viewer has not: on an Emby item the
    // stream the server names (`serverDefault`), or the one that suits the
    // wanted languages best; on a local video a file before a stream.
    void autoChooseSubtitle(std::size_t pane, int serverDefault = -1);
    // That pick's place in `options`, whatever the viewer has chosen; -1
    // when nothing on offer can be shown.
    int preferredSubtitle(std::size_t pane, const std::vector<SubtitleOption>& options, int serverDefault) const;
    // A local video's embedded stream, chosen as it opened, becomes what is
    // shown once the source is ready -- unless a file beside it already is.
    // An Emby item's stream taken to be inside the file and not found there
    // is asked of the server instead.
    void adoptEmbeddedSubtitles();
    // Reads and parses a subtitle file off the window thread, then shows
    // it if the pane still asks for it.
    void loadSubtitleFile(std::size_t pane, const std::filesystem::path& file);
    // Adds a file to the pane's subtitles and shows it: the dialog, a drop.
    void addSubtitleFile(std::size_t pane, const std::filesystem::path& file);
    void loadSubtitleDialog(std::size_t pane);
    static bool isSubtitleFile(const std::wstring& path);
    // The pane a subtitle key acts on: the one keyboard commands go to,
    // else the only video, else the first with subtitles on offer; -1.
    int subtitlePane() const;
    std::wstring subtitleLabel(std::size_t pane) const;
    // Every pane's lines shown or hidden: the Show subtitles setting.
    void toggleSubtitlesShown();
    // The bar's subtitle switch and Alt+H. On, it hides the lines and keeps
    // each pane's choice; off, it shows them, choosing for the video the
    // keys act on when it has nothing chosen.
    void toggleSubtitles();
    // Whether the switch is on: lines shown and, with a video, one chosen
    // for the video the keys act on.
    bool subtitlesSwitchedOn() const;
    void cycleSubtitle(std::size_t pane);
    // Positive shows the lines later. PotPlayer's `,` `.` and `/`.
    void nudgeSubtitleDelay(std::size_t pane, double seconds);
    void resetSubtitleDelay(std::size_t pane);
    void setSubtitleSize(float size);
    void setSubtitlePosition(float position);
    void setSubtitleLanguage(int language);
    // The languages wanted, most wanted first, and Windows' own list.
    std::vector<std::string> subtitleWanted() const;
    std::vector<std::string> systemLanguages() const;
    // The most wanted language as DirectWrite names it, for the font that
    // draws Chinese, Japanese or Korean lines; empty for any other.
    std::wstring subtitleLocale() const;
    // How a local file is opened: its text subtitle streams read alongside
    // the video, the one that suits the wanted languages shown.
    SourceOptions localSourceOptions() const;
    // How a subtitle with no look of its own is drawn: the face of the most
    // wanted language, and the box when the viewer asked for one.
    PlainSubtitleStyle plainSubtitleStyle() const;
    // Brings what the pane shows into its libass: the script of a file or a
    // server's stream, or the header and the events read so far of a stream
    // inside the file, and the fonts the video brought. False when libass
    // cannot draw them -- not set up, no event it could read, a stream not
    // yet open -- and the plain lines are drawn instead.
    bool syncAssSubtitles(std::size_t pane);
    // The pane's subtitles at `seconds` of its video as libass draws them
    // against its picture, and the window position of libass's frame. Sets
    // `drawn` when libass is what shows them; the picture is null when
    // nothing is on screen.
    std::shared_ptr<const SubtitleBitmap> renderAssSubtitles(std::size_t pane, double seconds, bool& drawn,
                                                             POINT& origin);
    // The Subtitles submenu, shared by the pane menu and the bar's button.
    void appendSubtitleMenu(HMENU menu, std::size_t pane) const;
    void showSubtitleMenu(int pane, POINT screenPoint);
    // The pane menu's audio tracks; returns how many it lists.
    std::size_t appendAudioTrackMenu(HMENU menu, std::size_t pane);
    // The pane a menu's command acts on: the one playing the video the menu
    // was built for (`menuSerial`), wherever a swap moved it, or nothing --
    // said -- when that video has gone. A command for a pane no menu
    // recorded acts on the pane as it is.
    std::optional<std::size_t> menuCommandPane(int menuPane, std::uint64_t menuSerial, int pane);
    // The arrangement and seek-bar states, changed from the sheet, the
    // popup menu and the bar's arrangement button alike.
    void appendLayoutMenu(HMENU menu) const;
    void appendSeekModeMenu(HMENU menu) const;
    // The pane menu's Emby favorite line, for a pane playing an Emby item.
    void appendEmbyFavoriteMenu(HMENU menu, std::size_t pane) const;
    void showLayoutMenu(POINT screenPoint);
    void chooseLayout(LayoutMode mode);
    void chooseSeekMode(SeekMode mode);
    bool handleAltKey(WPARAM key);
    void selectAudioTrack(std::size_t pane, int streamIndex);
    void retireSource(std::size_t pane);
    void startSourceStream(std::size_t pane, const std::wstring& url, const SourceOptions& options,
                          int audioStream, double position, bool playing);
    LRESULT handleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleVideoMessage(UINT message, WPARAM wParam, LPARAM lParam);
    bool initializeWindow(HINSTANCE instance, int showCommand);
    void loadFiles(const std::vector<std::wstring>& files);
    void addFiles(const std::vector<std::wstring>& files, int targetPane = -1);
    void openSource(std::size_t pane, const std::wstring& path, bool resetAdjustment = true);
    void openFilesDialog();
    void openFileForPane(std::size_t pane);
    int commandPane() const;
    void openAdjacentFile(int step, int pane = -1);
    // The media files beside `path` in the order the list is sorted by, and
    // where `path` is among them (-1 when it is not). False when that folder
    // has not been read yet: it is never read here, on the window thread.
    bool siblingFiles(const std::wstring& path, std::vector<std::wstring>& files, int& position) const;
    // Reads the folder of a video opened by hand off this thread, unless the
    // F6 list is that folder's or a read of it is already on its way.
    void requestSiblingFolder(const std::wstring& path);
    void applySiblingFolder(std::uint64_t serial, const std::wstring& directory, std::vector<LocalEntry> entries);
    // A step or an end that found its folder unread waits for it, and is
    // done when it arrives if the pane still holds the same video.
    void waitForSiblingFolder(std::size_t pane, int step, bool atEnd);
    void runPendingFolderStep();
    void stepToSiblingFile(std::size_t pane, const std::vector<std::wstring>& files, int position, int step);
    void advanceToSiblingFile(std::size_t pane, const std::vector<std::wstring>& files, int position);
    // The one loaded pane, or -1 when there are none or several.
    int singleLoadedPane() const;
    bool severalPanesLoaded() const;
    // The seek mode in force. One video has nothing to be independent of:
    // it is the timeline, so it follows the master clock whatever the saved
    // choice, which is kept for when there are several again. Without this
    // a repeating video wrapped under a clock that ran on, and the only bar
    // a single video has stayed at its end.
    SeekMode activeSeekMode() const;
    // Browser panes keep independent timelines and end rules. This separate
    // choice links only user-initiated seek bars and arrow-key seeks.
    bool linkedBrowserSeekBars() const;
    // Several browser-managed panes have no meaningful aggregate clock until
    // the viewer opts into linked seek bars.
    bool bottomTimelineVisible() const;
    int linkedBrowserReferencePane() const;
    bool anyPaneLoaded() const;
    // At the end of the only video: what the playback order says. False
    // when several videos are loaded and the set's own rule applies.
    bool finishSingleVideo();
    // The master clock at the deck's end: finishSingleVideo, the deck's
    // repeat, or a pause; nothing for per-pane decks or a held only video.
    void finishMasterTimeline(bool sourceOpening, bool hasRepeatingSource);
    // The only video of a shared deck when it is paused on its own (its
    // pane's pause, the clock running on), else -1. Its bar is its own
    // rail and the arrow keys move its held picture.
    int heldOnlyPane() const;
    void advanceAtEnd(std::size_t pane);
    // A file that has just replaced the only video starts from its own
    // beginning, playing or paused as the deck was, rather than at the time
    // the replaced one had reached.
    void restartReplacedOnlyVideo(bool resume);
    void pausePlaybackAtEnd();
    void setPlayOrder(PlayOrder order);
    void toggleMasterLoop();
    // `pointerPane` is the pane under the drop whatever is loaded in it: a
    // subtitle file goes to that video rather than to an empty slot.
    void onDrop(HDROP drop, int targetPane = -1, int pointerPane = -1);
    void togglePlayback();
    void stopPlayback();
    void seekRelative(double delta);
    void seekAbsolute(double position, SeekStyle style = SeekStyle::Synchronized,
                      bool keepEmbyPhotos = false);
    void alignBrowserPanesToTime(double target);
    void toggleAudioPane(std::size_t pane);
    void setAudioMask(unsigned mask);
    void refreshAudioControls();
    bool audioPaneEnabled(std::size_t pane) const;
    bool shouldOutputAudio(std::size_t pane, double timeline, bool playing) const;
    bool playbackIntended() const;
    void adjustVolumeFromWheel(short wheelDelta);
    void selectNextUnfinishedAudio(double timelinePosition);
    void createControls();
    void layoutControls(unsigned width, unsigned height);
    void updateDockAnimation();
    void updateControls();
    void loadShaderDialog();
    bool applyShaderPreset(ShaderPreset preset);
    void restoreShader();
    void changeDecodeMode(DecodeMode mode);
    void setPaneOffset(std::size_t pane, double seconds);
    void applyPaneOffset(std::size_t pane, double seconds);
    double paneOffsetSeconds(std::size_t pane) const;
    bool anyPaneOffset() const;
    void resetPaneOffset(std::size_t pane);
    void resetAllOffsets();
    void setPaneVolume(std::size_t pane, float volume);
    void nudgePaneVolume(std::size_t pane, float delta);
    void togglePaneMute(std::size_t pane);
    void setSourceAutoRepeat(std::size_t pane, bool enabled);
    void realignPaneAfterMappingChange(
        std::size_t pane, double timeline, double previousMappedTime);
    void seekFromSourceBar(std::size_t pane, double target);
    bool paneSeekAffectsAll(std::size_t pane, double target) const;
    // What the transport bar and title show: the master clock and deck length,
    // the only browser pane's own time, or the linked browser target time and
    // longest source length (zero until a source is ready).
    struct BarTime {
        double position{};
        double duration{};
        int pane{-1};
    };
    BarTime barTime() const;
    void seekBarTimelinePane(std::size_t pane, double target);
    void seekPaneTo(std::size_t pane, double target, bool forceIndependent = false);
    void toggleControls();
    void toggleSourcePause(std::size_t pane);
    void closePane(std::size_t pane);
    void swapPanes(std::size_t first, std::size_t second);
    double mappedSourceTime(std::size_t pane, double timeline) const;
    double currentSourceTime(std::size_t pane) const;
    PaneTiming paneTiming(std::size_t pane) const;
    bool paneRepeatSavedButIdle(std::size_t pane) const;
    PaneTimings paneTimings() const;
    PaneArray<SeekBarrier::PaneStatus> paneStatuses() const;
    long long seekBarrierElapsedMs() const;
    bool paneLogicallyLoaded(std::size_t pane) const;
    PaneArray<bool> activePanes() const;
    PaneArray<float> paneAspectRatios() const;
    void refreshAutoLayoutFocus(const PaneArray<float>& aspects);
    PaneArray<RectF> currentLayoutCells(
        float width, float height, const PaneArray<float>& aspects) const;
    PaneArray<int> panesByPosition() const;
    std::size_t paneForPosition(std::size_t position) const;
    std::size_t positionForPane(std::size_t pane) const;
    void updateHoverControls();
    void updateHoverControls(POINT screenPoint);
    void layoutHoverControls();
    void layoutEmptyHint(unsigned width, unsigned height);
    void updateEmptyHint();
    void layoutPaneTimelineControls(unsigned width, unsigned height);
    void updateAutoHideControls();
    void setControlsVisible(bool visible);
    void showContextMenu(int pane, POINT screenPoint);
    void handleContextCommand(unsigned command, int pane);
    void saveSessionDialog();
    void loadSessionDialog();
    void saveStyleDialog();
    void loadStyleDialog();
    bool saveSession(const std::wstring& path);
    bool loadSession(const std::wstring& path);
    bool saveStyle(const std::wstring& path);
    bool loadStyle(const std::wstring& path);
    SessionState captureSession() const;
    void applySession(const SessionState& state);
    StyleState captureStyle() const;
    bool applyStyle(const StyleState& state);
    std::filesystem::path settingsPath() const;
    AppSettings captureAppSettings() const;
    bool loadAppSettings();
    bool saveAppSettings() const;
    void scheduleAppSettingsSave();
    void flushScheduledAppSettingsSave();
    void tick();
    void renderFrame();
    void recoverLostDevice();
    void reportPerformance();
    void updateAudioDriftCorrection(double timelinePosition);
    void reportAudioStart(std::size_t pane, double audioSeconds, double expected,
                          std::chrono::steady_clock::time_point now);
    void reaimDriftedAudio(std::size_t pane, double audioSeconds, double expected,
                           std::chrono::steady_clock::time_point now);
    void publishPreferredSizes(const PaneArray<float>& aspects);
    void updateTitle();
    void toggleFullscreen();
    void beginSynchronizedSeek(double position, bool showKeyframePreview, bool resume);
    void cancelSeekBarrier();
    bool seekBarrierReady() const;
    void completeSeekBarrier();
    HWND window_{};
    HWND videoWindow_{};
    HINSTANCE instance_{};
    D3DRenderer renderer_;
    AudioOutput audio_;
    PlaybackClock clock_;
    PaneArray<std::unique_ptr<VideoSource>> sources_{};
    PaneArray<std::wstring> paths_{};
    PaneArray<double> startDelays_{};
    PaneArray<double> syncAdjustments_{};
    PaneArray<double> playbackRates_{1.0, 1.0, 1.0, 1.0, 1.0};
    PaneArray<bool> sourcePaused_{};
    PaneArray<double> sourcePausedTimes_{};
    PaneArray<bool> sourceAutoRepeat_{};
    PaneArray<bool> sourceLoopEnabled_{};
    PaneArray<double> sourceLoopA_{};
    PaneArray<double> sourceLoopB_{};
    // Last local time for which the video decoder was actually advanced or
    // sought. Audio-only operations must never consume this discontinuity
    // detector or a repeat wrap can leave video parked at EOF.
    PaneArray<double> lastVideoMappedTimes_{-1.0, -1.0, -1.0, -1.0, -1.0};
    PaneArray<PaneView> paneViews_{};
    bool masterLoopEnabled_{};
    double masterLoopA_{};
    double masterLoopB_{};
    unsigned audioMask_{1};
    int soloPane_{-1};
    LayoutMode layoutMode_{LayoutMode::Grid2x2};
    DecodeMode decodeMode_{DecodeMode::Automatic};
    bool nasCacheEnabled_{true};
    VideoEnhancementSettings rtxVideo_;
    // Emby: the client, the browser's page stack and its current list, and
    // per-pane playback state. Subtitles and audio-track choices belong to
    // any pane, Emby or local.
    EmbyClient emby_;
    // Pictures go through their own client so a page of thumbnails never
    // queues ahead of a click.
    EmbyClient embyImages_;
    // So do subtitle streams: the first time one inside a file is asked
    // for, the server takes up to half a minute to pull it out.
    EmbyClient embySubtitles_;
    ThumbnailCache thumbnails_;
    std::set<std::string> embyImagesInFlight_;
    // A local file's picture is made by the one shell worker
    // (LocalThumbnailer), so its keys have a queue of their own, apart from
    // the server's pictures: one job running and one waiting. A deeper
    // queue makes nothing sooner, only work gone stale after a scroll, a
    // folder or view change; each free place goes to what shows then.
    static constexpr std::size_t kLocalThumbnailQueue = 2;
    std::set<std::string> localThumbnailsInFlight_;
    EmbyBrowserPrefs embyBrowser_;
    EmbyLibraryPrefs embyLibraries_;
    std::optional<EmbyDetails> embyDetails_;
    std::uint64_t embyDetailsSerial_{}, embyDetailChildrenSerial_{}, embyAccountSerial_{};
    std::unordered_map<std::string, std::set<std::string>> embyItemLibraries_;
    std::set<std::string> embyLibraryLookups_;
    // Titles whose library a lookup could not prove: not asked again until
    // the account changes or the page is refreshed by hand.
    std::set<std::string> embyLibraryUnknown_;
    bool embyBrowserOpen_{};
    std::vector<EmbyPage> embyPages_;
    // Explicit picture clicks choose a playback destination, never hover or
    // audio selection. Queues live with each EmbyPane and follow its media.
    int embyTargetPane_{0};
    std::optional<EmbyPendingPlay> embyPendingPlay_;
    std::optional<LocalPendingPlay> localPendingPlay_;
    struct ExternalOpenBatch {
        SingleInstance::RequestId id{};
        std::vector<std::wstring> files;
        std::size_t next{};
        bool startup{};
        ULONGLONG arrived{};   // GetTickCount64 when it was queued
        // Beside `files` once a subtitle was moved in to wait for a video:
        // true for each entry moved; entries added later count as false.
        std::vector<bool> deferred;
    };
    // Transport reservations remain held until a whole batch completes or
    // is canceled, bounding both this deque and the pipe's receive queue.
    std::deque<ExternalOpenBatch> externalOpenBatches_;
    SingleInstance* singleInstance_{};
    bool externalOpenReady_{true};
    bool externalFilesProcessing_{};
    // The folder of a subtitle file opened into an empty deck is being read
    // for its video; later launches wait behind it. 0 when none is.
    std::uint64_t externalSubtitleLookup_{};
    // When that folder read began. A launch queued behind it waits at most
    // kExternalSubtitleLookupHoldMs; then the subtitle steps aside for it.
    ULONGLONG externalSubtitleLookupSince_{};
    static constexpr ULONGLONG kExternalSubtitleLookupHoldMs = 10'000;
    std::uint64_t externalSubtitleSerial_{};
    PaneArray<std::optional<LocalPanePlayback>> localPanes_{};
    std::uint64_t localQueueLoadSerial_{};
    BrowserSource browserSource_{BrowserSource::Emby};
    // The explicitly targeted local video's folder: as read and as ordered
    // for the list, the folder they are of, and the scan in flight.
    std::vector<LocalEntry> localEntries_;
    std::vector<LocalEntry> localList_;
    std::wstring localDirectory_;
    std::uint64_t localListSerial_{};
    bool localListLoading_{};
    std::wstring localListError_;
    // The folders of videos opened by hand (they have no F6 queue: giving
    // them one would put a linked deck on per-pane timelines) when F6 shows
    // another: read off the window thread when the video opens, kept as
    // read, and ordered as the list is when a step key or the end walks
    // them. Only the folders panes are in are kept.
    struct SiblingFolder {
        std::wstring directory;
        std::vector<LocalEntry> entries;
        std::uint64_t serial{};
        bool listed{};
        bool listing{};
    };
    std::vector<SiblingFolder> siblingFolders_;
    std::uint64_t siblingFolderSerial_{};
    // A step key or the only video's end that found its folder unread. The
    // video is known by its media serial, which follows it through a swap;
    // presses made meanwhile add up, and lapse when the folder takes longer
    // than kFolderStepWaitMs, as a jump much later would surprise.
    struct PendingFolderStep {
        std::size_t pane{};
        std::wstring path;
        std::uint64_t mediaSerial{};
        int step{};
        bool atEnd{};
        ULONGLONG asked{};
    };
    std::optional<PendingFolderStep> pendingFolderStep_;
    // The files read so far, their lengths and subtitle streams, by path, so
    // a refresh does not read them again.
    std::unordered_map<std::wstring, LocalProbe> localProbes_;
    // The scan that counts: a thread reading lengths stops when this is no
    // longer its serial, and when the player is gone.
    struct ScanToken {
        std::shared_ptr<std::atomic<std::uint64_t>> value{std::make_shared<std::atomic<std::uint64_t>>(0)};
        ~ScanToken() { value->store(~std::uint64_t{0}); }
    };
    ScanToken localScanToken_;
    // The normal sheet's tab, and each tab's own scroll.
    SettingsTab settingsTab_{SettingsTab::Playback};
    std::array<float, kSettingsTabCount> settingsTabScroll_{};
    float embyListScroll_{};
    float localListScroll_{};
    // The place to return to once the page asked for by Back has arrived.
    float embyPendingScroll_{-1.0F};
    LocalThumbnailer localThumbnails_;
    AssociationStatus fileTypes_{};
    std::vector<emby::Item> embyItems_;
    std::vector<emby::Item> embyResume_;
    std::vector<emby::Item> embyViews_;
    int embyTotal_{};
    bool embyLoading_{};
    // The next page of the list shown is on its way; said at the list's
    // end, not above it, so the rows being looked at do not move.
    bool embyLoadingMore_{};
    // A next page failed or came back empty: no more are asked for by
    // scrolling until the page is asked for again or Show more is pressed.
    bool embyAutoMoreBlocked_{};
    // The request out for the page shown asks for it again in place, and
    // does so unasked: a failure is logged, not written over the list.
    bool embyRefreshing_{};
    bool embyRefreshQuiet_{};
    // When the page shown was last asked for, 0 for never, and from when
    // on it is to be asked for again, 0 for no such wish (GetTickCount64).
    unsigned long long embyListAskedTick_{};
    unsigned long long embyRefreshWantedTick_{};
    // The page whose unasked refreshes the log already speaks of.
    std::string embyRefreshLoggedPage_;
    // The reply the list shown came from, while it is all the list holds.
    std::optional<EmbyListReplyPrint> embyListReply_;
    // Whether a video has a text subtitle stream, by item id, as the server
    // answered for this account; the ids whose question is out; and that
    // the next list to come is to be asked about anew (Refresh).
    std::unordered_map<std::string, bool> embySubtitled_;
    std::unordered_set<std::string> embySubtitleAsking_;
    bool embySubtitlesAgain_{};
    // The batches of those questions not yet sent, and how many are out: a
    // few at a time, the next sent when one is answered, so a click or a
    // report never waits on the one queue behind a long list's worth.
    std::deque<std::vector<std::string>> embySubtitleQuestions_;
    std::size_t embySubtitleQuestionsOut_{};
    // The items whose favorite change is on its way to the server.
    std::unordered_set<std::string> embyFavoriteAsking_;
    // The favorite entry a pane's menu offered, kept for the command it
    // returns as the subtitle entries are: the pane can go on to its next
    // video under an open menu.
    struct EmbyFavoriteOffer {
        std::uint64_t account{};
        int pane{-1};
        std::string itemId;
        std::wstring title;
        bool wanted{};
    };
    mutable std::optional<EmbyFavoriteOffer> embyFavoriteMenuOffer_;
    // The box with the typing, the input method's unfinished text there
    // and its caret, when the caret last moved -- it shows solid then, and
    // blinks after -- the hidden system caret kept under the drawn one for
    // input methods that follow that instead, and whether the input method
    // is off for the password. The main window having the keyboard at all.
    // When the search box was last edited and not yet searched for (0 for
    // not).
    TextFieldId typingField_{TextFieldId::None};
    std::wstring fieldComposition_;
    std::size_t fieldCompositionCaret_{};
    unsigned long long fieldCaretTick_{};
    bool fieldSystemCaret_{};
    bool fieldImeOff_{};
    bool keyboardFocus_{};
    unsigned long long embySearchEditedTick_{};
    // What the sign-in page's boxes hold; the password until a sign-in
    // with it succeeds.
    struct EmbySignInForm {
        TextFieldState server, user, password;
    };
    EmbySignInForm embySignInForm_;
    // Set once the search it was handed to has been overtaken: the
    // libraries not yet asked are then not asked (EmbyClient::request).
    std::shared_ptr<std::atomic<bool>> embySearchDropped_;
    std::wstring embyNote_;
    bool embyNoteError_{};
    std::uint64_t embyRequestSerial_{};
    std::uint64_t embyPaneSerial_{};
    PaneArray<std::optional<EmbyPane>> embyPanes_{};
    // The parsed track of an Emby stream or of a file; an embedded stream's
    // cues stay with its VideoSource, which is still reading them.
    PaneArray<std::shared_ptr<const SubtitleTrack>> subtitles_{};
    PaneArray<SubtitleSelection> subtitleSelection_{};
    // The files a pane offers: those found beside a local video, then any
    // loaded by hand.
    PaneArray<std::vector<std::filesystem::path>> subtitleFiles_{};
    // Those of them that were read and could not be shown, and why: one
    // with no line or too large is listed, not offered; one that could not
    // be read is offered at the menu only.
    PaneArray<std::vector<std::pair<std::filesystem::path, SubtitleRead>>> subtitleFilesFailed_{};
    // What Alt+L last chose, so the next step goes on from it even when it
    // showed nothing; cleared by any other choice.
    PaneArray<SubtitleSelection> subtitleCycleAnchor_{};
    // The entries of the Subtitles menu that is open, the pane they are of
    // and that pane's video then (its subtitleSerial_): a command names an
    // entry by its place, and what a pane offers -- or the video it plays --
    // can change under an open menu.
    mutable std::vector<SubtitleOption> subtitleMenuOptions_;
    mutable int subtitleMenuPane_{-1};
    mutable std::uint64_t subtitleMenuSerial_{};
    // The same for the pane menu's audio tracks: the streams listed, by
    // place.
    std::vector<int> audioMenuTracks_;
    int audioMenuPane_{-1};
    std::uint64_t audioMenuSerial_{};
    // An Alt+letter subtitle key was taken: its character message, which
    // would only make the default handler beep, is dropped.
    bool altCharTaken_{};
    // Retired whenever the pane's media changes, so a scan or a read that
    // finishes late finds the pane has moved on.
    PaneArray<std::uint64_t> subtitleSerial_{};
    std::uint64_t subtitleSerialCounter_{};
    // The viewer chose this pane's subtitles, or turned them off: nothing
    // found later chooses for them.
    PaneArray<bool> subtitleChosenByViewer_{};
    // A video whose embedded streams are still to be looked at once it is
    // open: a local one's choice to take up, an Emby one's to confirm.
    PaneArray<bool> subtitleEmbeddedPending_{};
    // Seconds the lines are shown later than their file says; negative, earlier.
    PaneArray<double> subtitleDelay_{};
    SubtitleSettings subtitleSettings_;
    // Each pane's libass, made for one video's media (the serial it was made
    // under) and kept while that media stays: the fonts it brought, the
    // script shown and what that was made from.
    struct AssPane {
        std::unique_ptr<AssSubtitles> subtitles;
        std::uint64_t serial{};
        bool fontsTaken{};
        // A file's or a server's stream's parsed track; or, for a stream
        // inside the file, its index and how many of its events libass has.
        std::shared_ptr<const SubtitleTrack> track;
        int stream{-1};
        std::size_t events{};
        // A plain script is made again when its look changes.
        bool plain{};
        PlainSubtitleStyle plainStyle;
        // libass took nothing from it: the plain lines are drawn instead.
        bool failed{};
    };
    PaneArray<AssPane> assPanes_{};
    mutable std::optional<std::vector<std::string>> systemLanguagesRead_;
    std::shared_ptr<MainThreadQueue> mainQueue_{std::make_shared<MainThreadQueue>()};
    // Set when settings.qconfig names a newer version than this build reads,
    // or could not be kept aside after a failed read: this session runs on
    // defaults and saves nothing, so the file another build wrote survives.
    bool settingsReadOnly_{};
    ShaderPreset shaderPreset_{ShaderPreset::Normal};
    SeekMode seekMode_{SeekMode::Linked};
    SeekMode browserSeekMode_{SeekMode::Independent};
    bool browserSeekAligned_{};
    bool fullscreen_{};
    bool controlsVisible_{true};
    bool controlsPinned_{};
    float dockProgress_{1.0F};
    ULONGLONG dockAnimationTick_{};
    ULONGLONG controlsLastInteraction_{};
    ULONGLONG lastRenderTick_{};
    // Writes every thread's stack to QuadDeck.log when renderFrame stops
    // being called: the window thread is waiting on something.
    HangWatchdog hangWatchdog_;
    ULONGLONG pointerLastMoved_{};
    POINT lastPointerScreen_{std::numeric_limits<LONG>::min(), std::numeric_limits<LONG>::min()};
    bool cursorHidden_{};
    int wheelDeltaRemainder_{};
    bool repeatAll_{true};
    PlayOrder playOrder_{PlayOrder::InOrder};
    // One video seeks to keyframes (instant, up to a keyframe interval
    // off) unless switched off; several seek exactly, for their sync.
    bool keyframeSeek_{true};
    // The direction of the relative seek being issued, for the keyframe
    // to land on; 0 for an absolute one.
    int pendingSeekDirection_{};
    std::mt19937 shuffleRandom_{std::random_device{}()};
    // A folder's random order holds for the session, so Page Up/Down walk
    // one shuffle rather than reshuffling at each step.
    const std::uint64_t sessionSeed_{std::random_device{}()};
    int expandedPane_{-1};
    AutoLayoutFocus autoLayoutFocus_{AutoLayoutFocus::Dynamic};
    int autoLayoutFocusPane_{-1};
    PaneArray<bool> autoLayoutActivePanes_{};
    int hoverPane_{-1};
    int lastPointerPane_{-1};
    int contextPane_{-1};
    bool contextMenuOpen_{};
    int dragSourcePane_{-1};
    bool draggingPane_{};
    POINT dragStart_{};
    PaneArray<bool> sourceInitialAlignmentPending_{};
    PaneArray<double> sourceProvisionalTargets_{};
    bool settingsSavePending_{};
    unsigned settingsSaveRetryCount_{};
    PaneArray<float> lastLayoutAspects_{};
    unsigned contentHeight_{};
    // The drawn interface: the painter, the scene it paints and the geometry
    // the pointer is tested against. All rebuilt every frame from state.
    Overlay overlay_;
    OverlayScene scene_;
    TransportBarLayout barLayout_{};
    CaptionLayout captionLayout_{};
    int hotCaptionItem_{-1};
    int pressedCaptionItem_{-1};
    EmptyHintLayout emptyHint_{};
    bool hotEmptyEmby_{};
    bool pressedEmptyEmby_{};
    // What updateTitle last gave the window, also drawn on the caption.
    std::wstring windowTitle_{L"QuadDeck"};
    PaneArray<PaneChromeLayout> paneChrome_{};
    PaneArray<RectF> chromeCells_{};
    PaneArray<std::wstring> pillLabels_{};
    PaneArray<float> pillWidths_{-1.0F, -1.0F, -1.0F, -1.0F, -1.0F};
    PaneArray<float> paneChromeAlpha_{};
    PaneArray<bool> paneChromeWanted_{};
    PaneArray<float> paneTimelineAlpha_{};
    PaneArray<bool> paneTimelineWanted_{};
    bool volumeOpen_{};
    int hotBarItem_{-1};
    int pressedBarItem_{-1};
    int hotChipPane_{-1};
    int hotChip_{-1};
    int pressedChipPane_{-1};
    int pressedChip_{-1};
    int hotPillPane_{-1};
    int pressedPillPane_{-1};
    int hotRailPane_{-1};
    int hotVolumePane_{-1};
    bool seekHot_{};
    ULONGLONG hotSince_{};
    std::wstring tooltipText_;
    OverlayRect tooltipAnchor_{};
    POINT lastLocalPointer_{-1, -1};
    bool pointerInsidePlayer_{};
    ControlDrag controlDrag_{ControlDrag::None};
    int dragPane_{-1};
    float dragFraction_{};
    int dropTargetPane_{-1};
    // The settings sheet.
    bool settingsOpen_{};
    float settingsAlpha_{};
    float settingsScroll_{};
    // The sheet's rows, laid out in panelLayout_. A browser list's rows
    // (panelRuns_) stay from frame to frame while the list keeps its shape;
    // only those near the screen are filled (panelFilledRows_), the others
    // are bare.
    std::vector<PanelRow> panelRows_;
    std::vector<PanelListRun> panelRuns_;
    std::vector<std::size_t> panelFilledRows_;
    PanelLayout panelLayout_{};
    PanelHitKind hotPanelKind_{PanelHitKind::None};
    int hotPanelRow_{-1};
    int hotPanelPart_{-1};
    PanelHitKind pressedPanelKind_{PanelHitKind::None};
    int pressedPanelRow_{-1};
    int pressedPanelPart_{-1};
    // A press on the sheet that may become a drag of its content: where it
    // started and the scroll then. The thumb drag keeps where it was grabbed.
    bool panelPressArmed_{};
    POINT panelPressPoint_{};
    float panelPressScroll_{};
    float panelThumbGrab_{};
    // The browser the right edge opened closes itself; one F6 opened stays.
    bool embyBrowserByEdge_{};
    bool embyEdgeArmed_{true};
    ULONGLONG embyEdgeLeftSince_{};
    // What the pressed row was, so a release on a row that was rebuilt
    // underneath the pointer does not act on a different item.
    SettingId pressedPanelId_{SettingId::None};
    int pressedPanelParam_{-1};
    // Where the last click let go, for the double-click that follows it.
    std::optional<RepeatControl> releasedControl_;
    int panelDragRow_{-1};
    std::wstring noticeText_;
    ULONGLONG noticeUntil_{};
    float uiScale_{1.0F};
    SmartVibranceSettings smartVibrance_;
    std::wstring customShaderPath_;
    // The custom shader file is still the choice but could not be read at the
    // last start or device rebuild; the preset runs until it can be.
    bool customShaderUnloaded_{};
    std::wstring sessionPath_;
    WINDOWPLACEMENT previousPlacement_{sizeof(WINDOWPLACEMENT)};
    double duration_{};
    SeekBarrier seekBarrier_;
    std::chrono::steady_clock::time_point seekBarrierStarted_{};
    PaneArray<std::chrono::steady_clock::time_point> lastDriftReports_{};
    // The first error of each audio generation is logged once, half a
    // second after it can first be measured.
    struct AudioStartReport {
        std::uint64_t generation{};
        std::chrono::steady_clock::time_point measuredSince{};
        bool reported{};
    };
    PaneArray<AudioStartReport> audioStartReports_{};
    // A pane's audio re-aimed for drift beyond correction, at most every
    // few seconds: a stream whose timestamps never agree with its picture
    // must not be sought again on every frame.
    PaneArray<std::chrono::steady_clock::time_point> lastAudioReaim_{};
    // When the next rebuild may run; a display change brings it forward.
    std::chrono::steady_clock::time_point nextDeviceRecovery_{};
    int deviceRecoveryAttempts_{};
    bool deviceRecoveryPending_{};
    bool deviceRecoveryResume_{};
    double deviceRecoveryPosition_{};
    double deviceRecoveryBrowserPosition_{};
    PaneArray<double> deviceRecoverySourceDurations_{};
    std::chrono::steady_clock::time_point lastPerfReport_{};
    bool perfPlaying_{};
    double frameWaitMs_{};
    PaneArray<std::uint64_t> presentedFrames_{};
    PaneArray<std::uint64_t> lastShownSerial_{};
    PaneArray<std::uint64_t> lastDecodedFrames_{};
    PaneArray<std::uint64_t> lastCapacityWaits_{};
    // Last, so it is destroyed first: it waits for every source it is
    // closing while the audio output and the renderer they use still exist.
    SourceReaper sourceReaper_;
};

}  // namespace quaddeck
