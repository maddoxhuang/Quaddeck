# QuadDeck 1.0.0 architecture

## Single-instance external file opening

`main.cpp` handles the existing explicit file-registration switches before
single-instance election. All other launches use `SingleInstance` before
constructing App or a player window. Its lifetime mutex is in the Windows
session namespace and its mutex/pipe names include the current user SID and
session ID. Production has one fixed namespace; a separate constructor accepts
an explicit unique suffix for isolated process tests, with no production
command-line or environment override.

The local named pipe has a logon-SID DACL, rejects remote clients and verifies
the peer process's user and session on both ends. Client rights are specified
individually rather than granting generic write/create-instance access. The
first-pipe-instance flag prevents a second listener from silently sharing the
endpoint. This follows Microsoft's
[named-pipe security guidance](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights).
No persisted secret, TCP listener or association/settings change is needed.

The versioned, length-delimited protocol carries only a request ID and a bounded
list of UTF-16 filesystem paths, or an empty list to activate the window.
Paths are normalized syntactically in the sender's working directory; URLs,
Emby locators, device paths and control switches are not playback requests.
`main.cpp` leaves such an argument out with one warning rather than failing
the launch. A device is refused only as a whole path component (trailing dots
and spaces dropped): `Con.Air.1997.mkv` is an ordinary file on Windows 11 and
on shares, and where older Windows maps a device name with an extension to
`\\.\CON`, `GetFullPathNameW` has already produced a path the syntax check
refuses.
Drive and UNC filesystem paths are supported without resolving a network file
on the window thread. Limits are 256 paths and 512 KiB per request, 64 pending
requests and 2 MiB of pending payload. An ACK means the complete request has
been reserved and queued in memory. Its reservation survives UI dequeue until
`completeRequest`; pending IDs and completed IDs retained for 60 seconds make
bounded sender retries idempotent. A full queue/cache returns a failure rather
than discarding files. Delivery is not durable across a receiving-process crash.

Only the elected owner creates the player. A secondary waits through startup
and retries with the same ID for a bounded interval; a dead owner's abandoned
mutex allows takeover. An alive but unavailable receiver causes a clear error
instead of another player window. The sender grants foreground permission only
to the verified receiver PID; the window thread restores a minimized receiver,
tries to foreground it and flashes its taskbar entry when Windows refuses.
Shutdown stops and joins the listener before clearing the App queue, while the
mutex remains owned through App teardown. Recoverable early client disconnects
restart the accept loop instead of stranding an alive owner without a listener.

App drains accepted batches on its window thread. The startup files are the
first batch. While no pane is loaded (a fresh launch, or a window whose videos
were all closed), `openExternalDeck` gives the batch to `loadFiles` as the
command line did before single-instance -- up to five panes, the saved seek
mode and audio mask, the rest reported -- and then starts it as Space does
(the command line used to leave it paused). Several files handed to one launch
are a comparison deck, and an empty running window behaves like no window at
all. Otherwise the batch takes the local Plan A Add path. It never replaces an
active manual local/Emby chooser. Existing normalized local paths select their
pane without reopening or changing audio, pause or progress, and make that
pane visible. New paths occupy empty panes, muted, with independent local
queues; the first Add adopts a startup pane into its folder queue as it does
for a manual open. A full five-pane deck pauses on the existing local
thumbnail/title chooser. Cancel discards the current external batch's remaining
files while preserving later independent batches and all playing panes.
Manual F6 Add retains its same-file duplication and cancellation behavior.
A subtitle file (`isSubtitleExtension`) in a batch is never a pane of its own.
With video showing, `openExternalSubtitle` puts it on the local pane whose
video it belongs to by the sidecar rule (same folder, `subtitleNameFitsVideo`,
the longest name, `externalSubtitleVideoPane`), else on `subtitlePane()`,
through `addSubtitleFile` as a drop does: the viewer's choice. A
`subtitlePane()` that is still opening (an Emby item resolving) gets a
notice, never another pane. When a video it fits better than any open one is
still queued -- later in its batch, or in a batch behind it, since Explorer
starts one launch per selected file -- `deferExternalSubtitle` moves the
subtitle after that video and after the subtitles moved there before it
(`ExternalOpenBatch::deferred`), ahead of those that came with the video, so
they go on it in the order they came. Because the video's launch can arrive
a few ticks later, a subtitle with no fitting open video, or only one whose
name is shorter than another it could fit (a dot after the open name), holds
its batch for up to `kExternalSubtitleWaitMs` (750 ms) from the batch's
arrival before falling back. `openExternalDeck` opens the batch's videos and
leaves its subtitles at the end of the batch, for the passes above. A batch of subtitles alone into an empty deck
first defers to a queued video of its name, as above; failing that, after the
wait, `findExternalSubtitleVideo` lists its folder on a detached thread
(`std::filesystem::path::parent_path`, so a drive root is the root;
`scanLocalFolder`, `videoForSubtitle`), later batches wait behind it
(`externalSubtitleLookup_`), and the video found is put into the batch ahead
of the subtitle, so the deck opens it and then the subtitle goes on it. None
found is a notice; a video opened meanwhile takes the subtitle instead, and a
video launched meanwhile is taken over the folder's.
The first launch of a single `.qdeck` still restores the session; a forwarded
session request gives a notice to use the existing Open Session action, so IPC
cannot replace the deck. The old executable has no listener: users must exit
that version themselves once before starting the new one, and file types still
registered to an older copy keep opening it until Register... is pressed again.

## UI and layout

When several browser-managed panes have independent seek bars, there is no coherent aggregate duration. `transportBarLayout` omits the bottom time and seek rectangles while retaining the left and right button groups; the absent rail has no hit target. The window title also omits aggregate time, and each pane's own rail remains. Selecting browser Linked restores the bottom rail against source seconds and the longest source duration.

The player has two HWNDs: the main window and one video child that always receives the full client area and owns the swap chain. Everything the user sees besides the video is drawn, not a control. `Overlay` (Direct2D on the renderer's D3D11 device, DirectWrite for text) paints an `OverlayScene` onto the swap chain's back buffer each frame, invoked by the renderer between the last pane and `Present` inside the device lock; the picture is never covered by a control's background, every element is translucent and fades, and there is no child-window z-order to manage. `OverlayLayout.hpp` holds the geometry -- the transport bar along the bottom, each pane's chrome, hit-testing -- as pure functions on floats with the DPI factor applied once, so the core test binary exhausts them without a device or a font. `AppChrome.cpp` builds the scene from player state, tracks hover, press and drag state, and turns pointer messages on the video child into commands.

The transport bar is a gradient scrim over the bottom 112 pixels with one 40-pixel row of controls: play/pause, stop, master time, the master seek rail taking whatever width is left, mute, a volume rail that unfolds while the pointer is on the speaker or on it, the menu, fullscreen and settings. When even the preferred rail does not fit, controls leave as complete units in a fixed order (settings, arrangement, menu, subtitles, stop, time, mute, fullscreen, then the step buttons), because each remains reachable from the keyboard, the popup menu or the settings sheet; play and the rail are the final pair, and a control that is out never returns at a narrower width. The row's footprint, from its top edge to the bottom of the window, excludes the panes beneath it from pointer targeting; the scrim above the row is still the video. A moving pointer entering the bottom 22-pixel strip reveals the bar; while it is up, its whole band keeps it up, and it fades 1.1 seconds after the last interaction unless pinned (`U`). A press or drag on any control counts as interaction and holds the bar.

The window has no Windows title bar, as PotPlayer with "auto hide main skin while video screen is active". `WM_NCCALCSIZE` lets `DefWindowProc` lay out the standard frame and then puts the client's top back at the window's top, so the sides and the bottom keep Windows' own resize borders outside the picture, the shadow and snapping, and only the caption is gone; a maximized window pulls its top in by the frame it hangs off the monitor (`SM_CYSIZEFRAME` + `SM_CXPADDEDBORDER` at the window's DPI) and stops two pixels short of any auto-hide taskbar's edge. In its place `captionLayout` (`OverlayLayout.hpp`) draws a 32-pixel strip over the top of the picture: the window title (`updateTitle`'s text, still also the taskbar's) and Windows 11's 46-pixel Minimize, Maximize/Restore and Close, which post the matching `WM_SYSCOMMAND`. `windowFrameHitTest` decides the top: a resize band of the frame's thickness (not when maximized), and the strip outside its buttons as `HTCAPTION`, so dragging, double-click maximize, the system menu and Aero Snap are Windows' own. The video child covers the whole client, so it answers `HTTRANSPARENT` there and the main window answers for it, after `DefWindowProc` has claimed the real frame. The caption has no state of its own: it shows with the bar (the top 22 pixels reveal both, its strip keeps both up, `U` pins both) and with the sheet, whichever is further in, stays up with nothing loaded, and is absent in fullscreen. The sheet's column still reaches the top edge but its header starts below the strip, pane pills sit below the strip whether or not it is up -- a fixed offset, so a pill never moves away from a pointer that wakes the caption -- the notice drops below it while it shows, and the right-edge zone that brings up the F6 list or the Emby browser leaves the strip out, so heading for Close does not open it. With one visible pane there is nothing to swap with, so a drag on the picture starts `SC_MOVE` instead, as PotPlayer drags its window by the picture.

Each active pane wears a label pill in its top-left corner (`V1 · 1.0× · Fit`; clicking it opens that pane's menu), a row of action chips beside the pill while the pane is hovered -- audio, pause, solo, repeat, close, dropped from the right when the pane is narrow -- and a seek rail along its bottom edge, drawn 3 pixels thick and 6 while hovered or dragged, with the NAS cache's buffered range in a lighter tone ahead of the playhead and a time label above the rail while it is engaged. The rail lifts above the transport bar's row by the amount the row covers, scaled by the bar's reveal progress, so the two never overlap. A pane too short for the rail keeps only the pill; one too short for either shows neither. The pane being heard carries a fine accent border when more than one pane is active; a drag-to-swap dims the source pane and outlines the target. Chrome shows while the pointer moves inside the foreground player and fades 1.4 seconds after it stops (a dragged rail stays), on a 160 ms fade per pane; the cursor hides on the same timer.

Rails are dragged with capture on the video child: the position follows the pointer without seeking, and release commits one fast seek -- the master rail through `seekAbsolute`, a pane rail through `seekFromSourceBar`, which keeps its Linked/Independent policy. The volume rail applies live. A press on a button or chip activates on release only if the pointer is still on the same element. Every release reads what it is releasing before it lets the capture go: `ReleaseCapture` sends `WM_CAPTURECHANGED` to the video child synchronously, and that handler cancels whatever press or drag is under way, as it must when another window takes the pointer mid-drag. A drag-to-swap that released the capture first never swapped (2026-09-26 to 2026-10-07).

Settings changes, whether from the sheet, the popup menu or a key, apply directly to the same `App` state. The shader's active label derives only from the last successfully compiled preset or external file; failed compilation is transactional and leaves the previous pixel shader and status intact. The four Smart Vibrance Plus sliders map to the reference ranges and update a constant buffer immediately without recompiling the shader.

`activeLayoutCells` is the single geometry source for rendering, timeline placement, hit testing, overlay placement and drag-to-swap. It compacts only loaded panes into aspect-aware automatic, one-row, two-row, one-column, portrait-focus or landscape-focus layouts. Five-pane Auto uses a three-plus-two row split with clamped aspect-weighted widths and balanced row heights. A sole portrait source gets a narrow tall left cell with four side panes in a 2x2 grid; the inverse focus form puts a sole landscape source above a row of four portrait-friendly cells. With three or five active sources, an explicitly focused pane uses the existing left-focus geometry: two stacked side panes for three sources or a side 2x2 for five. The explicit choice survives batch additions/removals only while the active count remains three or five and the selected pane remains active. Solo remains an override. Adjustable mouse dividers are intentionally deferred because normalized weights, minimum sizes, pointer capture, reset behavior and persistence need one coherent layout-specific model.

V1-V5 are display-position names, derived from those cells top-to-bottom and then left-to-right; they are not the storage indices of `sources_`. The settings sheet, numeric audio shortcuts, per-pane repeat, volume/mute and Offset steppers all translate through the same order. Once all active sources have reported an aspect ratio, App locks Auto's selected tall/wide focus until the active pane set changes. Replacing media therefore preserves the chosen physical cell even when the replacement has a different aspect ratio. Solo does not renumber the selected pane.

Explorer media drops use append-first routing. Because compact dynamic cells cover the entire client area, a hit on an existing cell is ignored while any source slot is empty; `addFiles` fills empty slots in index order. Once all five slots are occupied, the hit pane becomes the replacement target. Explicit per-pane Open/Replace commands bypass this policy.

The main and video child HWNDs forward `Alt+Enter`, media commands and mouse-wheel input into one command path. Each complete wheel detent changes the XAudio2 master volume by five percentage points; high-resolution partial deltas are accumulated. A compiled Windows resource supplies the application icon to the executable and the main window class. The flat three-pane-and-play mark is authored in `assets/QuadDeck.svg`; `scripts/generate-icon.ps1` renders pixel-aligned 32-bit ICO frames at 16, 20, 24, 32, 40, 48, 64, 96, 128 and 256 pixels using Windows System.Drawing, and a 256-pixel PNG. The checked-in ICO is consumed directly by the existing resource build. The class leaves `hIconSm` null so Windows selects an appropriate small frame from the large icon's resource group instead of scaling the large handle into both slots.

The popup menu (its contents are described below) is drawn dark through uxtheme's unnamed `SetPreferredAppMode`/`FlushMenuThemes` exports (ordinals 135/136, guarded by a build-number check; `DarkMode.hpp`), so an older Windows keeps the light system menu. The bar's menu button opens the same menu without a pane section; a pane's pill opens it for that pane. Core tests exhaust the bar for every width from 1 through 1920 pixels for bounds, ordering, non-overlap and monotonic visibility, check that a 2x client at 2x scale doubles every coordinate, and cover the pane chrome's pill, chip prefix, rail lift and hit-test precedence.

Icons are Segoe Fluent Icons glyphs (MDL2 Assets on Windows 10) rendered by DirectWrite; if neither face is installed the buttons show short words. A control the pointer rests on for 600 ms shows a drawn tooltip naming it and its shortcut. The on-screen notice is a pill at the top centre: `showNotice` arms a 1.5-second deadline that `tick` enforces and the last 300 ms fade; user-initiated state changes that used to reach only the title bar -- audio selection, mute, volume, per-pane speed, view, zoom, pause, solo, offsets, loops, layout, decoder, shader, keyboard seeks and folder stepping -- call it. Session and settings loading do not, so nothing flashes at startup. An empty window draws its drop hint the same way.

`F5`, the bar's gear or the menu opens the settings sheet: a column of rows that slides in over the right 420 pixels and is drawn like everything else. `SettingsPanel.hpp` is its row model (header, note, toggle, choice, slider, buttons), its geometry and its hit test, pure and exhausted by the core tests. The rows are split over five tabs (`SettingsTab`) in a strip under the header that stays put while the content scrolls beneath it: Playback (seek bars, decoder, restart-all, keyframe seek, play order, and the timing rows -- per-video offset steppers in 0.1 s and 1 s, reset, per-video repeat), Audio (master and per-video volume with a mute toggle on each rail, per-video output), Subtitles, Picture (arrangement, all-video fit/fill/stretch and zoom; shader choice, file load, disable, the four Vibrance+ sliders, RTX Video) and General (pin the bar, NAS cache with its status, Explorer file types, visual preset and session load/save). `AppPanel.cpp` fills the chosen tab's rows from player state every frame and turns a hit into the same state change the keys make; each tab keeps its own scroll, the sheet reopens on the tab it was closed on, and the strip wraps onto a second line when the sheet is too narrow for its tabs at 56 pixels each. Emby is not a tab: the browser is the sheet's other face, and the header carries a switch (`sheetSwitch`, left of the close glyph) that reads `Emby ›` on the settings and `‹ Settings` on the Emby browser or the F6 list; `switchSheet` opens the browser as Ctrl+E does (the sign-in dialog when signed out) or returns to the settings, each side keeping its scroll. The sheet has no Emby account rows of its own: the browser's home page shows the account and signs out, and each library page carries its information-page toggle. Nor has the browser a Settings button of its own: its home page's row is Search, Sign out, Refresh (one part short of the other pages' Back, Search, Home, Refresh, which `embyNavigate` allows for), and its sign-in page is the status and the sign-in button. The Emby browser and the F6 list share the sheet without the strip. The sheet scrolls with the wheel while the pointer is on it, closes on `Esc`, its × or a click on the video beside it, and a slider drags with capture like the rails. The player has no Win32 controls left; App keeps no GDI resources.

The popup menu is actions on the current content, plus the two states that are changed while watching several videos. With a pane: open/replace, previous/next file, close; output this audio, mute this video, pause, solo, focus; speed, view/zoom and A-B loop submenus, repeat, reset offset. Always: play/pause, stop, mute, the master A-B loop, and with several videos loaded the Arrangement and Seek bars submenus -- the same choices as the sheet's Picture and Playback tabs, through `chooseLayout` and `chooseSeekMode` -- then open/save session, fullscreen, settings. The other states -- all-video view, decoder, shader, presets, restart-all, pinning -- live only in the sheet. The bar's menu button opens the menu without a pane section. With several videos loaded the bar also carries an arrangement button between the subtitles and the menu, which opens the Arrangement submenu on its own; with one video it is not there (the step buttons take that case), and when the bar is narrow it is the first control to leave after Settings.

A pane being heard shows a volume rail after its chips while hovered; dragging it trims that video alone and reports the value as a notice on release.

The process declares per-monitor v2 DPI awareness in `wWinMain` (falling back to system-DPI awareness on builds without `SetProcessDpiAwarenessContext`), so the video child's swap chain is the monitor's real pixels rather than a 96-DPI surface that DWM stretches -- on a 4K display at 200% the old behaviour rendered every source at 1920x1080. `App::uiScale_` is the window's DPI over 96, derived from the system DPI before `CreateWindowExW`, then from `GetDpiForWindow` once the HWND exists and again on `WM_DPICHANGED`, which also adopts the suggested rectangle. Every metric in `OverlayLayout.hpp` and `SettingsPanel.hpp` is a 96-DPI value multiplied by the factor once inside the layout functions, so the exhaustive 1x layout tests still hold and a 2x client twice as wide produces the 1x layout with every coordinate doubled; DirectWrite formats are rebuilt when the scale changes. A DPI change needs no relayout beyond the next frame, which is built from the new scale.

Keyboard pane targeting tracks the pointer independently of hover-button visibility.
A pinned or animated dock hides the buttons, but only its actual screen rectangle
excludes a pane from pointer hit testing. Loading a session clears the previous
deck's transient Solo and pointer selection before restoring its persisted layout.
The App regression target exercises these orchestration paths with hidden native
geometry and unopened decoders, without starting audio devices or settings I/O.

## Time model

`PlaybackClock` is the master time. A source's local time is:

```text
(master - startDelay) * playbackRate + independentSeekAdjustment
```

It may then be held by per-source pause or wrapped by that source's A-B loop. A loop with only an A point wraps at the source's end (`loopEnd`), so setting A alone arms it; a B set later narrows it, and until the duration is known such a loop neither wraps nor counts as repeating. If no A-B loop is active, an enabled per-pane auto-repeat wraps the complete local duration only in Independent mode. Linked seeks invert the mapping; independent seeks preserve master time and change the selected source's adjustment. Master A-B loop seeks the entire clock, while a source loop seeks only that decoder when its mapped local time wraps.

Clearing an offset writes the pane's adjustment without re-aligning, so resetting all five costs one synchronized seek rather than five. A paused pane needs more than the adjustment cleared: it shows the time it is holding, and resuming rebuilds the adjustment from that held value, so the held time moves with the offset or the change is undone the moment the pane resumes.

The UI exposes the combined signed value `adjustment - startDelay * rate` as Offset. Positive values start farther into source content at master zero; negative values keep the source inactive until its mapped time reaches zero. Dragging the slider of an inactive source always changes only its adjustment, even in Linked mode, so a waiting source can be positioned directly without advancing the master clock. Legacy session delay fields remain readable.

Each source has its own rail and duration. A drag on a pane rail follows the pointer as a fraction of that rail (`railFraction`, with the rail's own inset) and commits one seek on release through `seekFromSourceBar`, always against that pane's own duration, so clips of very different lengths cannot leak master or neighbour timing into one another. In Linked mode, an active unpaused pane uses the inverse master-time mapping only when the requested source time is representable. Paused panes, waiting panes and targets earlier than a positive adjustment move that pane's Offset directly, avoiding a jump back to the earliest representable source time. Total master duration accounts for start delay, rate and adjustment. By default, reaching the total duration performs one synchronized seek to zero while preserving the playing state. A valid enabled master A-B loop takes precedence; any active source A-B loop or Independent per-pane repeat keeps the master clock running so that local wrapping can continue.

Source opening is asynchronous. While any non-failed source is still probing, the master extent is the maximum of the ready-source calculation and the previous stable duration; replacing the longest pane therefore cannot transiently look like end-of-deck. The first seek requested by `openSource` is provisional because Independent whole-file repeat cannot wrap until duration is known. When metadata becomes ready, App compares that provisional local target with the settled mapping and performs at most one correction even while paused. If a synchronized barrier is active, it rebuilds the complete barrier and all recorded generations rather than replacing one pane's generation in place. Previous/next-file replacement also cancels a barrier that owns the old decoder and rebuilds it at the same resumable timeline target, except that the only video's replacement starts again from zero (see "One local video is the timeline"). Only actual video progress and video/full seeks update the wrap-discontinuity tracker; audio-only seek, selection and handoff cannot consume a pending video repeat wrap.

Audio selection is a five-bit mask. Every pane owns one XAudio2 SourceVoice and all selected voices mix through the common mastering voice. Each pane also carries its own volume and mute, applied to that pane's source voice, while the mastering voice keeps master volume and mute. The two multiply rather than compete: silencing one pane leaves the others where the user put them, and the master control still governs everything at once. A muted pane keeps its trim, so unmuting restores the level rather than jumping to full. Each voice has its own queue, flush operation and frequency ratio, so changing or seeking one pane does not discard audio queued by the others. When a selected non-repeating source reaches its end before the master timeline, the app replaces only that bit with the next active, unpaused, unselected and unfinished pane in cyclic visible V1-V5 order. A repeating pane's natural Ended state remains selected because its video wrap creates a fresh audio generation; a generation-scoped Error may still hand off. Other selected voices continue uninterrupted.

Manual audio enable and automatic audio handoff call `requestAudioSeek`, which updates only the private audio demux/decoder context. They never clear `videoQueue_`, `currentFrame_` or touch the video decoder's seek generation. Full timeline movement continues to use `requestSeek`, which atomically schedules both video and audio seeks.

Source voices are persistent and their Start/Stop state is tracked idempotently. Opening a replacement is asynchronous, so it can initially fail the `ready` audio predicate; the render tick starts the fixed voice when the pane later becomes eligible. Each `VideoSource` also holds an atomic destination-voice index. Drag-to-swap retargets that index before moving the existing decoder, so it keeps synchronization state without leaving the audio callback attached to the old voice. Audio chunks carry a process-wide monotonically increasing generation because a persistent voice outlives the `VideoSource` that previously fed it. Submit, flush, voice state and timeline accounting are serialized by one mutex per voice. A flush rejects every older generation as well as physically clearing buffers, closing both sides of the race in which a stale chunk could arrive immediately before or after `FlushSourceBuffers`. Submission reports Accepted, Backpressure, Stale or Error explicitly: only Accepted may publish Primed, Backpressure retries the intact chunk, Stale abandons the superseded generation and Error terminates the current generation.

Audio drift maintenance visits every selected voice so non-reference timelines discard consumed spans during long multi-audio playback. The first voice that is currently eligible, has a usable stream and still has an XAudio2 buffer queued controls the master-clock slew; a selected bit whose video/audio has ended cannot hide a later voice that is still audible.

## Session model

`Session.hpp` writes the platform-neutral `QDECK 6` text format. Version 6 adds the fifth pane; version 5 adds per-pane volume and mute; version 4 added the four Smart Vibrance Plus values; version 3 added the audio mask and per-pane auto-repeat. The reader remains backward compatible with `QDECK 1`–`QDECK 5`, promoting old single audio slots to a one-bit mask, supplying reference-default vibrance values, reading only four pane records from old files and leaving the fifth pane at safe defaults. It serializes global playback/render settings and the complete five-pane state. File parsing validates enum ranges, rates, zoom, shader parameters and loop boundaries before `App::applySession` recreates sources. The session stores paths rather than media content.

The separate `QSTYLE 3` format contains only visual state: dynamic layout, expanded pane, five per-pane view/zoom records, shader selection/path, Smart Vibrance Plus parameters and dock pinning. `QSTYLE 1` and `QSTYLE 2` remain readable; their fifth PaneView stays at the default. Loading a style never closes, reopens, seeks or otherwise mutates the current media sources.

`QCONFIG 14` is the automatic per-user application configuration stored under `%LOCALAPPDATA%\QuadDeck`. It composes `QSTYLE 3` with decode/seek/repeat, the five-bit audio mask, per-pane repeat flags, master volume/mute, per-pane volume/mute, normal window placement, NAS cache and RTX Video options, Emby browser preferences, playback order, keyframe seeking, subtitle settings and per-library information-page preferences. `QCONFIG 1` through `QCONFIG 13` remain readable; missing fields receive defaults. Old four-pane files retain a default fifth pane, and files written before per-pane volume existed load every pane at full volume. NAS caching defaults on for old files. Subtitle positions from formats before 14 reset to the current relative-lift default while the other valid subtitle settings remain.

Configuration is loaded after HWND/D3D/audio/control creation and before command-line media. Settings changes update state synchronously (the NAS option applies to subsequent opens) but coalesce persistence through a 300ms one-shot timer; a transient failure receives one bounded delayed retry, and normal `WM_DESTROY` writes the final state directly. It deliberately contains no media source paths or playback state. Because `QSTYLE 3` includes all five PaneViews and closing/replacing a source no longer resets its PaneView, Fit/Fill/Stretch, crop state and vibrance controls persist for later media.

`QDECK 6`, `QSTYLE 3` and `QCONFIG 14` saves share one transactional UTF-8 persistence path. The complete text is serialized in memory, written to a unique sibling with `CreateFileW`/`WriteFile`, flushed with `FlushFileBuffers`, and then committed with `ReplaceFileW` (or `MoveFileExW` for a new file). Existing files use a short-lived same-volume rollback name so every documented partial replacement state can restore the old destination. Failed serialization, sharing violations and failed writes remove the temporary file and do not truncate the last valid document. Reads use `CreateFileW` with delete sharing so an atomic replacement can proceed concurrently, reject malformed UTF-8 before parsing, accept an optional BOM and cap these small text documents at 16 MiB.

## Decode and render paths

`ReadAheadCache` provides custom AVIO for UNC paths and drive letters that
Windows reports as remote. Detection and file opening run on a per-file worker,
never the HWND thread. The sheet's NAS switch (interface section) defaults on and applies to
subsequent opens; QCONFIG 6 persists it, and QCONFIG 1-5 migrate to network-only
enabled. QDECK/QSTYLE formats and existing playback policy are unchanged.

Two independent AVIO cursors (video/audio) share immutable compressed 1 MiB
blocks and one overlapped Windows file handle. Foreground misses take priority
over speculative reads, with round-robin servicing of simultaneous audio/video
demands. The video cursor drives continuous read-ahead after stream discovery,
including while paused or blocked on the small decoded-frame queue. The target
is 30 source seconds from container bitrate (size/duration fallback), capped at
256 MiB per file; unknown bitrate uses 32 MiB. A global atomic reservation budget
caps resident plus in-flight block allocations at 1 GiB, and active files share
that capacity equally. Decoder surfaces, AVIO buffers and allocator overhead
are outside this payload limit. No claim of network-level bandwidth reservation
is made. First-packet preparation aims for 3 seconds and waits at most 5 seconds;
high bitrate, EOF and capacity shares can shorten that target. A seek cancels
old demand/overlapped reads, preserves reusable blocks, and prioritizes the new
byte position without repeating the startup delay after the first frame. A seek,
interrupt or miss on one cursor never cancels a block the other cursor is
waiting for; a seek or interrupt also keeps a read-ahead fetch that is still
inside the video window. Once the window is full the worker sleeps until a
cursor moves rather than polling, re-checking its share of the budget once a
second.

Metadata probing is demand-only to avoid prefetching at an index located at EOF.
Transient failed reads retry after 250 ms; demand waits fail after 15 seconds
instead of inventing EOF. A Windows read requests cancellation after 10 seconds.
Close marks the cache stopped before joining either decoder, wakes consumers,
and cancels synchronous open/metadata operations and overlapped reads. Buffers
and OVERLAPPED storage stay alive until kernel cancellation completes; a broken
network driver can still delay that completion. FILE_SHARE_READ excludes writers
and replacement while open, so existing cached blocks cannot silently mix with
new file contents. The same share mode refuses a file another process already
holds open for writing, such as a recording in progress; any failure of that
open therefore bypasses the cache, and FFmpeg's own share-everything open
either plays the file uncached or reports the real error. HTTP/other protocols
and local disks retain the original FFmpeg input path. Custom AVIO allocation/freeing is owned separately from
avformat_close_input, and cancelled AVIO error/EOF state is cleared for a new
seek. Existing generation gates still determine which decoded frames/PCM may
be submitted. Request publication and old-cache-read interruption share the
VideoSource state lock, so a late cancellation cannot hit the replacement seek.
A fatal video open/decode failure stops the background cache as well.

The settings page shows approximate media seconds buffered and total MiB; the
title shows first-open preparation. Performance diagnostics include each source's
resident MiB, ahead seconds, hits, misses and retry counts. Cache tests use a
controllable slow/offline byte source to check content through outages, shared
cursor isolation, cancellation, retry/EOF semantics and five-file allocation;
native media probes use the same Windows/AVIO path via the optional `cache`
argument even for local fixtures. This is separate from physical NAS testing.

`DecodeMode::Automatic` prefers D3D11VA but accepts software frames. `Hardware` strictly requires D3D11 surfaces; `Software` bypasses hardware setup.

Hardware `AVFrame` clones pin the decoder texture and array slice. A D3D11 Video Processor converts each surface directly to a per-slot BGRA texture. Most software frames are converted to BGRA at the size the pane actually displays: never magnified past 1:1, snapped to eighths so that dragging a window edge cannot recreate the converter on every pixel of movement, and capped at 3840 in either dimension. 1920×1080 survives only as the seed used before the first layout hint reaches the decoder. They are uploaded with `UpdateSubresource`. One eligible software path preserves 8-bit YUV420 SDR frames with BT.601/BT.709 metadata, uploads them as NV12 and sends them through the video processor, so RTX Video HDR can also work when the decoder returned CPU frames. Other software formats retain the BGRA path.

All paths converge on shader-resource views. A vertex-shader UV transform implements Fill cropping and 1×–4× centered zoom; viewport geometry implements Fit and Stretch. A replaceable `ps_4_0` shader performs final pixel processing. `ShaderCompat.hpp` translates common PotPlayer/MPC Direct3D 9 `.txt` forms (named `s0` sampler variants, `tex2D` family, `p0/p1/pPrev`, `COLOR`) into D3D11 HLSL. It parses balanced sampling-call arguments while masking comments and quoted text. Plain `tex2D` becomes `Texture2D.Sample`; lod/bias/project calls use small helpers that evaluate their coordinate argument once, preserving enclosing and nested expressions. Bare `TEXCOORD` becomes `TEXCOORD0`. Native regression tests compile each supported legacy input as `ps_3_0` and its translation as `ps_4_0`. A 48-byte pixel constant buffer at b0 maps c0 to dimensions/counter/clock, c1 to inverse dimensions, and c2 to a safe temporal-state default. The complete stateless Smart Vibrance Plus preset uses a separate 16-byte b1 buffer for intensity, saturation pivot, gray pivot and gray sharpness, preserving the legacy b0 contract. The vertex shader mirrors the transformed UV over `TEXCOORD0`–`TEXCOORD3` for legacy scripts using different input indices. No hardware frame is read back to CPU.

Two NVIDIA RTX Video features ride on that video processor, both off by default and stored as device settings in `QCONFIG 7` (`rtxvideo`), not in a visual preset. They are private stream extensions the driver accepts through `VideoProcessorSetStreamExtension`; the GUIDs and version/method words in `D3DRenderer.cpp` are the ones mpv and MPC Video Renderer ship, cross-checked, since NVIDIA publishes no header. At device creation the renderer records the adapter (a null-adapter `D3D11CreateDevice` takes whichever GPU drives the primary display), and on an NVIDIA adapter builds a throwaway 1080p processor to ask the driver once whether it honours Super Resolution (the enable call succeeds) and RTX Video HDR (`VideoProcessorGetStreamExtension` answers non-zero). Every few seconds and on resize it also reads, for the output the window sits on, whether Windows has it in HDR mode (`IDXGIOutput6::GetDesc1` colour space) and its SDR white level (`DisplayConfigGetDeviceInfo`). `videoEnhancementNote` in `Core.hpp` turns wanted-versus-available into the one line under the two toggles, pure and exhausted by the core tests.

Super Resolution needs the processor, not the sampler, to reach the pane's pixels. When the pane actually upscales, `presentationSize` may magnify the processor's intermediate (in sixteenths, still bounded at 3840); the extension is applied to each slot's processor when it is built. This also covers the eligible software NV12 path above. The request and code path do not establish visual Super Resolution quality or software-frame throughput. RTX Video HDR needs somewhere to write HDR: when it is wanted, the driver supports it and the display is in HDR mode, `applyOutputMode` moves the swap chain to `R10G10B10A2_UNORM` with `SetColorSpace1(RGB_FULL_G2084_NONE_P2020)` through `ResizeBuffers`, without touching the device the decoders share, and drops every slot so the processors are rebuilt with 10-bit PQ output views. Hardware PQ sources use their declared DXGI stream colour space; eligible software NV12 uses its retained SDR metadata and the TrueHDR extension. When TrueHDR is declined for an SDR frame, or an SDR software format bypasses the NV12 bridge, the BGRA frame is converted by a separate shader from G22/BT.709 to BT.2020/PQ at the display's SDR white level after selected effects. Native CPU HDR retains its previous software colour and tone-mapping limitations; this does not add native software HDR support. Direct2D cannot draw sRGB into a PQ back buffer -- white would be 10,000 nits -- so in PQ mode the painter is handed a transparent premultiplied BGRA layer instead and a shader pass linearises it, scales it to the display's SDR white level, PQ-encodes it and blends it over the video. Whatever the swap chain's mode, the replaceable pixel shader still runs; in PQ mode it sees PQ-encoded values. The built-in Vibrance preset handles those explicitly as described below; other presets and external shaders retain their existing behavior.

The built-in Smart Vibrance Plus uses a per-draw 16-byte b2 buffer, separate
from the unchanged legacy b0 and four-slider b1. `SlotTexture::rtxHdrApplied`
records whether that processor accepted TrueHDR; the global enhancement status
only describes the last processor and cannot choose a pane's shader route.
`bindVibranceColorSpace` selects the old SDR algorithm for non-PQ textures,
HDR-aware enhancement for hardware PQ from an accepted SDR-to-HDR conversion,
and exact passthrough for native/other PQ. Failed shader compilation leaves the
previous selection intact. b2 is unbound for all other presets/custom shaders.
Slot invalidation, output changes and device recovery also reset this state.

HDR Vibrance runs in the existing composition pass: ST 2084 decode to nits,
BT.2020 linear luminance Y (0.2627/0.6780/0.0593), and colour scaled along the
constant-Y gray axis before PQ encoding. Exposure-normalized colourfulness
controls saturation rolloff; Gray pivot/sharpness suppress near-gray noise
(unlike the original SDR response, which is retained verbatim). A heuristic
exposure mask ramps up from 0.01 to 1 nit and down from 203 to 1000 nits, and a
warm red-yellow mask halves the added gain at its centre; this is not skin
recognition or display calibration. Gain smoothly approaches the legal RGB
boundary along the same axis, avoiding independent channel clipping. Linear Y
is retained within shader floating-point accuracy, but channel peaks can rise;
the boundary is the BT.2020/PQ encoding range, not the monitor's actual gamut.
Intensity 1 returns the input exactly. No extra AI pass, CPU readback or saved
format change is introduced. The UI and subtitles are composited afterwards.
Software frames outside the eligible NV12 bridge keep the BGRA path; SDR-to-PQ
presentation uses the explicit conversion above rather than the processor's
TrueHDR conversion. Native CPU HDR retains its prior software colour and
tone-mapping limitations; HLG and Dolby Vision support are not added. Native PQ
passthrough here applies to built-in Vibrance, not a promise about arbitrary
external shaders.

`QuadDeckShaderProbe` reads back the actual compiled shader into a float target,
checking luminance, enhancement, protected ranges, unchanged SDR, per-pane
selection, native PQ passthrough and resource reconstruction. Those synthetic
PQ inputs do not establish that the driver performed RTX HDR inference, that
the display looks correct, or that full playback meets 4K60.

## Emby, subtitles and audio tracks

### Independent Emby panes (Plan A)

`embyRequestPlay` distinguishes ordinary Play/Resume from Add. Ordinary play
uses the last explicitly selected storage pane (`embyTargetPane_`), displayed
as its current V number and media title. Pointer hover and browser focus do
not change that target or the audio mask. Add reserves the first empty pane,
counting paths and resolving requests as occupied. It clears only the new
pane's audio bit, and leaves the other selected audio voices alone. Successful
Add exits Solo so the additional pane is visible. The browser stays open and
can be closed or navigated without stopping any source.

Only a full five-pane deck opens `EmbyPendingPlay`: a copied item, queue,
account generation and every destination's path/media generation. The sheet
shows V number, title and cached Emby/local thumbnail for each replacement,
plus Cancel. Confirmation rechecks the account, duplicate playback and the
chosen destination identity before any playback mutation; cancellation only
discards the pending request and restores browser scroll. Movies and episodes
offer explicit Add-and-resume and Add-from-beginning actions when applicable.
List and tile Add regions have their own `PanelHitKind::Add` geometry.

Each `EmbyPane` owns `emby::PlaybackQueue` (items, cursor and originating page)
and server/user/account identity. Queue helpers in `EmbyPlayback.hpp` preserve
duplicate playlist-entry identity. A reply may extend the same page without
dropping or reordering existing entries; looking at another page cannot replace
that queue. Explicit sorting reorders only the selected playback target's queue.
The old shared `embyPlaylist_` no longer exists. Detail-season pagination uses
the same per-pane extension rule.

While an Emby video or a local browser-managed pane participates,
`activeSeekMode()` is Independent; the saved local `seekMode_` is unchanged
for mapping, repeat, audio and EOF. A separate, session-only
`browserSeekMode_` starts Independent and lets F5 link user-initiated seeks
without changing the per-pane timeline. A linked pane rail, bottom rail or
arrow key rebases every ready video's adjustment against one chosen source
time, then performs one fast seek. Before the first alignment the bottom rail
shows the selected pane's source time; afterward it shows the linked target,
so arrows continue past the end of a shorter selected video. The rail spans
the longest video duration. A resolving pane blocks alignment until ready;
a failed pane must be closed and a video with unknown duration cannot be
aligned. Videos shorter than the chosen time follow their own end or repeat
rule; an A-B loop can remap the clicked time. Multiple local files opened by hand
without a browser-managed queue retain the user's saved Linked/Independent mode.
Resume is a source-local offset under the unchanged master clock. Opening,
replacement, seeking or pausing one pane retires only its seek-barrier ticket
and audio generation, without re-seeking other panes. A new source inherits
global pause; a request finishing later cannot resume the deck. The first
source may start the otherwise empty clock, and Add is still muted. Because
the master clock runs on through every replacement, it says nothing about the
only F6 or Emby video: `barTimelinePane` (`SyncState.hpp`) names that pane,
and `App::barTime` gives the bottom rail and the window title its
source-local time and length, zero while it is still opening or resolving.
A drag on that rail moves the pane (`seekBarTimelinePane`): an F6 file through
the master clock when the target is reachable, so it still lands on a
keyframe, otherwise and for Emby by `seekPaneTo`. Multiple independent browser
panes omit the bottom time and seek rail because the master extent includes
elapsed time before those panes were added. The remaining controls are still
labelled All. Browser Linked restores the bottom rail with source seconds and
the longest video duration. Existing explicit global synchronization and
multi-audio controls remain available, as does automatic audio handoff.

`finishEmbyPanes` observes each pane's presentation time against container or
server duration, then `endHandled` latches before stopping/reporting that pane
and applying the shared PlayOrder to its own queue. Other panes keep playing.
A next item already open elsewhere is selected without opening another session;
the ended pane remains held, so a timer cannot repeatedly advance it. Unknown
duration without server runtime does not trigger speculative early advancement.
Reports are built from each pane's playback/media-source IDs and local time,
including muted playback. Browser and direct-locator opens deduplicate the same
server/user/item, with a second uniqueness check at report generation. Account
changes enqueue each old Emby pane's Stopped request under the old session before
closing it and invalidating callbacks; local panes are retained. No new account,
device identity or persisted format is introduced.

Movie and Series information pages are an optional layer over the browser,
implemented in `AppEmbyDetails.cpp`. An item opens that layer only when its
library is identified by the current server's Views as `movies` or `tvshows`.
Other item/library types and ambiguous provenance retain their original
behavior. The library page's `Information pages` toggle and F5's Emby
library switches edit the same preference. Search records each responding
library and waits for all replies before enabling results; Continue watching
uses a known parent or a read-only Ancestors request matched uniquely to Views.

`EmbyLibraryPrefs.hpp` keys overrides by the opaque server ID and library ID,
not a display name, address or item type. Eligible libraries default on;
unknown types default off even if a stale override says on. `QCONFIG 13`
adds `embylibrarydetails <count>` and quoted
`embylibrarydetail "serverId" "libraryId" <0|1>` lines after `subtitles` and
before QSTYLE. Versions 1–12 load with no overrides. At most 256 pairs of
512-byte UTF-8 IDs are accepted; duplicate keys, invalid text or malformed
values reject the configuration transactionally. Serialization sorts the
pairs for stable output. No token, media position or server address is added.

`EmbyDetails` owns its detail item, season/episode lists, scroll, metadata
generation and child-request generation separately from the source browser.
Back restores the unchanged list and its scroll, including Random/search
order. Hidden detail cancellation restores only the Emby scroll slot. Account
changes retire details, image replies and provenance; callbacks verify their
generation before applying metadata or children. Refresh reloads seasons and
retains the selected season if it still exists. A failed request leaves a
Refresh/Retry route. TV children page in batches of 300, automatically up to
20000, with explicit Show more thereafter.

Viewing details never adopts a playback queue or reports progress. Ordinary
Play/Resume targets the explicitly selected Emby pane; `Add & resume` and
`Add from beginning` follow the placement rules above. Each destination pane keeps its own
captured source list and queue; a detail-season page may extend that pane's
queue as later pages arrive, without replacing another pane's queue. Resume
uses the fresh item reply's latest position, while From Beginning remains
explicit through that reply.

`PanelRowKind::MediaDetail` uses one shared geometry for painting and hit
testing. Overlay measures wrapped text with DirectWrite before layout,
paints a cover-cropped backdrop with a dark scrim, contains the full poster
and optional logo, and stacks the poster above text below 680 DIP. Synopsis
expansion changes the row height and scroll range. Missing optional values
are omitted; image failure falls back to text/artwork placeholders. Image
keys distinguish Primary, Backdrop and Logo, image index, tag and width,
while accepting the previous Primary keys. Detail metadata and technical
track descriptions come from read-only item data; browsing never requests
PlaybackInfo. The new [single-item request](https://dev.emby.media/reference/RestAPI/UserLibraryService/getUsersByUseridItemsById.html)
uses the documented path without a Fields query; the existing playback
request remains unchanged. Library provenance uses the documented
[Ancestors array](https://dev.emby.media/reference/RestAPI/LibraryService/getItemsByIdAncestors.html),
and artwork uses the [typed image endpoint](https://dev.emby.media/reference/RestAPI/ImageService/getItemsByIdImagesByTypeByIndex.html).
Trailer, predicted end time and editing/download actions are
outside this information-page implementation.

The full-deck replacement explanation is an opt-in wrapped Note. DirectWrite
measures its complete text at the current inner width and DPI scale using
the same Small style and line spacing as painting. That measured height,
plus padding, participates in the shared row layout, so Cancel, thumbnails
and their hit regions move together. Before DirectWrite is ready the pure
layout reserves conservative text space. Other Note rows retain their fixed
height; this is not a general settings-sheet layout change.

Movie/TV identity uses `emby::displayTitle` for playback titles and most
browser labels: movies and series carry a valid `ProductionYear`, episodes
retain their series and season/episode code (including `IndexNumberEnd` for a
combined episode), and an unnamed season falls back to Specials / Season N.
Browser episode labels place the code before the series name and title so
long show names cannot hide adjacent episode numbers in a narrow dock.
With no episode number, the episode name comes first instead.
Missing optional metadata does not manufacture a year or an episode number.
Season rows show supplied episode/unplayed counts and a Season badge rather
than Folder; watched series/seasons can show the server's completion mark.
Search includes Episode alongside the existing Series, Movie, Video and Photo.

`embyListPath` now covers the TV hierarchy as well as ordinary lists.
Both `/Shows/{id}/Seasons` and `/Shows/{id}/Episodes` request the shared
`kBrowserItemFields` and explicit `StartIndex`/`Limit`, using the existing
300-item paging, in-place refresh and stale-reply guards. A season continues
fetching automatically (up to 20000 episodes, further entries via Show more)
so its playback sequence is not cut off at the first page. Their order remains
the server's; pagination does not enable the library's sort/filter/flatten
controls on these pages. A season's SeriesId wins, then its ParentId, then the
current Series page; with none available, its own id still scopes an
`/Users/{user}/Items?ParentId=...&IncludeItemTypes=Episode` request, ordered by
the core [IndexNumber, then SortName](https://dev.emby.media/reference/pluginapi/MediaBrowser.Model.Querying.ItemSortBy.html)
sort keys rather than alphabetizing episode titles. The season
therefore remains browsable without inventing an unrelated parent. The
official [Seasons](https://dev.emby.media/reference/RestAPI/TvShowsService/getShowsByIdSeasons.html)
and [Episodes](https://dev.emby.media/reference/RestAPI/TvShowsService/getShowsByIdEpisodes.html)
contracts document Fields and paging. Hierarchy requests do not automatically
expand a playing sequence across seasons.

The latest item reply remains authoritative for resume, including a nonzero
position accompanying `Played=true` during a rewatch. The existing five-second
start/end thresholds and Home-to-restart interaction are retained; this work
does not infer a new restart policy from the completed flag. Reports and server
writes during normal user playback remain on the existing path.


Emby is a source of items, not a second player. `EmbyApi.hpp` is the protocol, pure: request paths and queries, the reply shapes (`Item`, `MediaSource`, `MediaStream`, `PlaybackInfo`), report bodies, the `emby://<serverId>/<itemId>` locator a pane path holds in place of a file, and the `QEMBY 1` sign-in file; `tests/emby_tests.cpp` runs it against replies a 4.11 server gave. `EmbyClient` does HTTP with WinHTTP on one worker thread and queues each reply with its handler; `pump()`, called from `tick()` every frame, runs the handlers on the window thread, so nothing App owns is touched from the worker. Every handler carries a serial -- `embyRequestSerial_` for the browser, `EmbyPane::serial` for a pane -- and does nothing when the browser has moved on or the pane was reused since the request was queued. The token is stored under DPAPI in `%LOCALAPPDATA%\QuadDeck\emby.qauth`, separate from `settings.qconfig`, and travels to the server only as the `X-Emby-Token` request header, also for the stream FFmpeg opens: `VideoSource::open` takes `SourceOptions` whose headers become FFmpeg's `headers` option, so the URL that reaches `QuadDeck.log` and a `.qdeck` file carries no secret.

The browser is the settings sheet in another mode (`embyBrowserOpen_`), laid out `wide` -- the whole client, not the 420 px sheet at the right. `buildEmbyRows(clientWidth)` in `AppEmbyBrowser.cpp` (tiles per line by `panelTilesPerLine`, then stretched or shrunk with `panelTileWidthFor` so every line is filled; the sort row's segments per line by `panelSegmentsPerLine`) turns the page stack (`EmbyPage`: home, library, series, season, folder, search, playlist) and the current list into rows: the crumb header, the navigation buttons, a `View` choice (list, posters, thumbnails), a `Sort by` choice with its segments on one line where they fit (`PanelRow::segmentsPerLine`; eight orders, and on a playlist's page the playlist's own before them), the `Show` choice (`Folders`, `All videos`) and the `Unplayed only` toggle where the page takes them, then the list itself -- `PanelRowKind::Item` rows in the list view, or `PanelRowKind::Tiles` rows in the two grid views, one row per line of as many tiles as `panelTilesPerLine` says fit, each tile carrying its item (`tileParams`), its picture key (`tileKeys`) and its corner badge. The main tile action is `PanelHitKind::Tile` with the part index; its separate `+Add` region is `PanelHitKind::Add`, and `panelRowParam(row, part)` is the item it names; the press-and-release identity check compares that, so a reply landing between press and release cannot move a click to another item. Signed out, the same sheet is the sign-in page: the status line and a `Sign in…` button that opens the native dialog. The arrangement lives in `EmbyBrowserPrefs` (`Core.hpp`), written to `settings.qconfig` as the `QCONFIG 8` line `embybrowser`; older files read as thumbnails by name.

Ordering is the server's: `emby::applySort` writes `SortBy`/`SortOrder` from a `SortKey` (`SortName`, `DateCreated`, `PremiereDate`, `Runtime`, `Random`, `DatePlayed`, `Size`, `DateLastSaved` -- the only names sent, since an unknown one is a 500), with `IsFolder,<key>` and a per-field `Ascending,<order>` where folders come first, and `Filters=IsUnplayed` when asked. `SortKey::Updated` is `DateLastSaved`, when the server last wrote the item: a server told to keep a file's own date as its `DateCreated` lists a video it has only just found among the files of that date, so `DateAdded` does not bring what is new to the front, and neither does the server's own `/Items/Latest`. The date is not in the replies, so a list ordered here -- a search's merged results, the list being played under the sort keys -- goes by `emby::itemNumber`, the number the server gave the item, which counts up. What a library, folder or playlist page asks for is `emby::listQuery`, pure, from an `emby::ListPage` (kind, id, a library's collection type, the type of the item that was opened, a playlist's `ownOrder`) and an `emby::ListArrangement` (sort, direction, unplayed, flat); `App::embyListPath` is the request. Libraries of type tvshows list series recursively, movies list movies; everything else is walked as folders, folders first, or with `flat` -- the sheet's `Show` choice, `EmbyBrowserPrefs::flat` as before -- as every `Video,Photo,Movie,Episode` under the page recursively, 300 items a page. `emby::listTakesFlat` says where that choice is offered and honoured: not in the libraries of box sets and of playlists, which list those, nor inside a box set or a playlist, which list what they hold. A playlist (`EmbyPage::Kind::Playlist`, opened from an item of type `Playlist`) is asked for by `ParentId` with no `SortBy` at all, which a 4.11 server answers in the order the playlist was put together, honouring `Filters`; `/Playlists/{id}/Items` gives the same order but ignores `Filters`, and `SortOrder` without `SortBy` does not reverse it. `EmbyPage::ownOrder` is that state and the first segment of the page's Sort row; one of the browser's orders chosen there, or with the sort keys, gives it up for the page (`embyChooseSort`, `embySetSort`: the first choice sorts by the order, only the next one reverses it). A playlist comes whole, because it is a list to play through: `embyListArrived` asks for the next thousand entries until the total is held, 20000 at most. Any other long list grows as the sheet is scrolled to within its own height of the list's end (`embyLoadMoreNearEnd`, from `refreshPanelGeometry`); while a further page is on its way the `Show more` row says `Loading more` and is disabled -- the same row under the list, never a row above it, so nothing being read moves -- and a page that failed or came back empty is not asked for again by the scroll (`embyAutoMoreBlocked_`), only by the button. Search fans one `SearchTerm` request out per library (the server matches word starts and skips libraries marked excluded only when asked without a `ParentId`), merges the replies under one serial and orders the merged list with `emby::sortItems`, the same rules in the client. Choosing a playable item seeds the target pane's `emby::PlaybackQueue` from
the playable items in the shown order, retaining duplicate playlist-entry
identity. `Page Up` / `Page Down` walks that pane's queue first (including
search results); an item opened outside a list falls back to its Series episodes
or parent folder's children.

Docked: with any pane taken (a source, or a path still resolving), the browser is laid out at the right of the client, `embyDockWidth` wide (a quarter, never under 320 px), over the video: the panes keep the whole window and play on under it, as under PotPlayer's playlist, and only the transport bar keeps to `videoAreaWidth` -- the uncovered part -- so its buttons stay reachable. A click on the uncovered video is `PanelHitKind::Outside` and, docked, is not a click outside the sheet but the player's own. Choosing or adding an item from either the full-window or docked browser leaves it open in its docked state. F6 toggles the browser (`toggleEmbyBrowser`), Ctrl+E still opens it. While an Emby item plays, `embyEdgeHover` (every hover update) opens the docked browser when the pointer reaches the window's right edge and closes it again 500 ms after the pointer has left the sheet -- or, while the transport bar is up, the bar's band, because the bar is laid out beside the docked browser and closing it under a pointer that went from the list to the bar would stretch the rail under that pointer; a browser F6 opened is not the edge's to close, and the edge re-arms only once the pointer has been away from it. The sheet scrolls by dragging: a press on a row or on the empty sheet is armed (`panelPressArmed_`) and becomes a `ControlDrag::PanelScroll` once it moves past the system drag threshold, dropping the pressed row so the release is no scroll-turned-click; `PanelLayout::scrollbar` is the strip at the content's right, `panelScrollThumb` its thumb, and `ControlDrag::PanelScrollbar` drags it (`panelScrollForThumbTop`), a click on the track jumping the thumb under the pointer. The playing item is marked (`PanelRow::current`, a Tiles row's `selected`), and a video's watched state and length are shown side by side, never one for the other: the list row's value, and a tile's left pill, right pill and a progress bar along the picture's bottom (`tileMarks`, `tileBadges`, `tileProgress`).

Pictures: a tile's key is `emby::imageKey` (the item, or the child a folder borrows its `PrimaryImageItemId` from, the tag, the width asked for -- 440 for thumbnails, 300 for posters, twice the tile so a 200 % display gets a sharp one), empty for an item without a Primary image, which is never asked for because the server answers 500. `embyRequestVisibleImages`, run from `refreshPanelGeometry` every frame the browser is up, asks for the keys of the tiles within a sheet's height of the visible content that are neither cached nor in flight, at most 24 queued, through a second `EmbyClient` (`embyImages_`, `Accept: image/*`) so a page of pictures never queues ahead of a click; leaving a page or closing the sheet calls `clearPending`, which drops the calls not started and delivers their handlers a `Cancelled` response so the in-flight set stays true. Replies land in `ThumbnailCache` (`Thumbnails.hpp`): encoded bytes keyed by image key, 600 entries or 32 MiB, oldest out first, a failure kept as an empty entry so it is tried once. The Overlay holds the `ThumbnailCache*` through the scene, decodes what it draws with WIC into Direct2D bitmaps (`bitmapFor`, at most six decodes a frame, 220 bitmaps kept, least recently drawn dropped, all released with the device) and draws each picture letterboxed inside its tile, so a portrait phone clip and a poster both show whole.

Subtitles in the list: a list's items do not say whether a video has subtitles (a 4.11 server sends no `HasSubtitles` in them, and `Fields=MediaStreams` costs about 4.7 KB a video, megabytes for one with a hundred subtitle files beside it). So once a list has come -- `embyListArrived`, a merged search, the resume row -- `embyAskSubtitles` asks about its playable videos by id: `emby::subtitleQuestions` picks those not answered or out, `emby::subtitleBatches` cuts them into requests of at most 100 ids and 1500 characters, and `emby::subtitledItemsPath` asks `Ids=...&SubtitleCodecs=<kTextSubtitleCodecs>`, to which the server answers with those that have a stream of a text codec (`HasSubtitles=true` would also count a video with only PGS). `embySubtitled_` keeps the answers per account (`embyConfigure` clears it; a reply for an older `embyAccountSerial_` is dropped). A list asked for anew, or by `Refresh` (`embySubtitlesAgain_`), asks again about every video; one kept fresh unasked only about videos it had not shown, so the half-minute refresh adds nothing for a list already answered. The rows tag such a video `Sub` as the folder list does. On the human's server (15393 videos, 2026-10-07) the probe's `--subtitle-flags` found every video with a text stream marked and none other, in 154 requests and 243 KB.

Playing an item is `openSource(pane, locator)`: the prologue reports the
previous item stopped and clears that pane's Emby state; a locator then goes to
`embyBeginResolve`, which fetches the item and its `PlaybackInfo`, and
`embyStartResolved` opens the server-provided file source through
`startSourceStream`. Photos skip `PlaybackInfo` and open the server-rendered
primary image. `embyPlayItem` opens only the requested destination pane; target
selection, Add placement and full-deck replacement follow the rules above. Only
that destination's delay and rate are reset. Resume uses the fresh item reply's
position; an explicit From Beginning request remains in force through that
reply. A pane is `resolving` until its stream opens or fails. Sign out or a 401
sends Stopped under the old session for each Emby pane, closes those Emby
sources and retires their serials; local panes are retained. `swapPanes` moves
Emby state with the media, and replies find their pane by serial. `embyTick`
sends Playing when ready and per-pane Progress every ten seconds and on pause or
seek changes; closing, replacing, EOF and `WM_DESTROY` send Stopped.
`openAdjacentFile` on a locator follows that pane's queue, with the Series or
parent-folder fallback described above.

### Subtitle rendering and selection

Subtitles are drawn by libass (`AssSubtitles.*`, vcpkg `libass` with FreeType, HarfBuzz and FriBidi, DirectWrite as its font provider): a script as it is written -- positions and movement, the faces it names and those the video brought, colours, outlines, shadows, blur, rotation, fades, clips, drawings, karaoke. One `AssSubtitles` per pane and per video's media (`App::assPanes_`, made again when `subtitleSerial_` changes, moved by `swapPanes`, dropped by `resetPaneMediaState` and when the pane has no source) holds a libass library with the video's fonts, a renderer and the script shown; it is used on the window thread only. `App::syncAssSubtitles` brings it up to what the pane shows each frame: a file's or a server's stream's text (`SubtitleTrack::text`, kept by `parseSubtitles` beside the cues) loaded whole -- an ASS or SSA script as it is, an SRT or WebVTT file through the pure `plainSubtitleScript`, which turns its cues into events of one style (`plainSubtitleHeader`: 384x288 like FFmpeg's stand-in, so the sizes a stream's decoder writes for `<font size>` hold; white, a black outline, the face of the most wanted language from `plainSubtitleFont`, libass's box style 4 when `SubtitleSettings::background` asks for one) and its HTML-like tags into overrides (`assTextFromSrt`: only the tags SRT and WebVTT have count, so "a<b and c>d" stays text, and a backslash before n or h stays a backslash) -- or, for a stream inside the file, the stream's header and then its events as the video worker reads them, each handed over once. A plain script is made again when its look changes. The fonts a video brought (`VideoSource::fonts`) are added once the source is ready; libass's font lookup is begun again then (`addFonts`), because it keeps the stand-in it already chose for a name. `App::renderAssSubtitles` takes its frame from the pure `subtitlePlacement` (AssSubtitles.hpp). A script's frame is the picture as it is on screen (`visiblePictureRect`: Fit's fitted rectangle, the cell for Fill and Stretch, which a zoom does not move), with the whole picture (`pictureRect`: Fit, Fill, Stretch and the zoom as the renderer draws them) given against it as margins -- negative where Fill or a zoom crops -- so a script's coordinates are the picture's and nothing it draws lands on Fit's bars. Where the picture is cropped, its unplaced speech is laid out in the frame instead (`ass_set_use_margins`), or it would fall off the pane; what the script places still goes with the picture. A plain script's frame is the whole cell with no picture in it and no storage size: its lines are sized and placed against the pane, bars included, never stretched, as the plain lines always were and as mpv sizes converted subtitles by its window. `SubtitleSettings::size` is libass's font scale, applied only to lines the script does not place (`ASS_OVERRIDE_BIT_SELECTIVE_FONT_SCALE`); `SubtitleSettings::position` is libass's line position, how far the bottom lines are raised above their style's place, and the transport bar adds to it while it is up (the bar's settled height, from half its slide, and the line position rounded to a tenth of a percent: every change empties libass's caches). Colours are corrected from the script's `YCbCr Matrix` to the video's (`subtitleColourForVideo`, `VideoSource::colourMatrix`), as xy-VSFilter's successors do; a script that names none is taken as BT.601. What libass hands back -- coverage masks in one colour each -- is composited into premultiplied BGRA pieces, one per place on the pane that has lines (overlapping masks merged), so that a sign in a corner and the speech at the bottom are not one picture the size of the pane to clear and upload; the same picture comes back while libass reports no change. The scene carries it (`OverlayPaneScene::subtitleImage`) and `Overlay::drawSubtitleImage` draws each piece pixel for pixel before the chrome, keeping a bitmap per piece and refilling it in place while its size holds. Measured on the probe below: about 0.1 ms a frame for static lines, 4 ms for a fansub opening's fades and 8-9 ms for a frame full of moving effects, at 3840x2160; a style's first appearance loads its face, 20-50 ms. Where libass cannot be set up or reads no event from a script, the cues are drawn as before: `Subtitles.hpp` parses SRT, WebVTT and ASS/SSA into cues with tags and overrides stripped and of a line's placement only whether it belongs at the top (`SubtitleCue::top`, from `\an`, SSA's `\a`, `\pos`/`\move` against `PlayResY`, a style's alignment, an SRT's `{\an8}`), drawings dropped, a layered line once (`subtitleLinesAt`); `Overlay::drawSubtitle` paints them as white letters with an eight-way black outline, a box where `background` asks, the size a share of the pane's height, the bottom lines 6 % plus `position` above the edge or clear of the bar, in the face DirectWrite substitutes for the most wanted language (`App::subtitleLocale`).

A pane's subtitles come from one of three places, `App::SubtitleKind` (`AppSubtitles.cpp`). `EmbyStream`: a text stream of an Emby item that is not read with the video -- a file beside the video on the server's disk, or one inside it that the source turned out not to read -- fetched whole from `/Videos/{id}/{source}/Subtitles/{index}/Stream.{ass|srt}` -- as ASS when that is the stream's own format (`emby::isAssSubtitleCodec`), which keeps the placement the conversion loses, and as SRT otherwise or when the ASS request fails or parses to nothing; the request goes through a client of its own (`embySubtitles_`), as pictures do, because the server took up to 25 s to pull a stream out of a file the first time it was asked, and neither a click in the browser nor a progress report should wait behind that. `File`: a file beside a local video (`name.srt`, `name.xx.ass`, ssa, vtt) or one loaded by hand (the menu, Alt+O, a drop on the pane), read as UTF-8, UTF-16 with a BOM or the system code page. `Embedded`: a text stream inside the container, a local file's or an Emby item's. An Emby item's stream inside its file was once fetched from the server like the rest; the server pulls one out by reading the file from its start, and a 6.6 GB film did not answer within the 30 s the client waits, for the ASS request nor for the SRT one after it, while the client's one worker held every later subtitle request -- a file beside the video included -- behind them. The stream URL hands the file over unchanged (`Static=true`), so its streams are read as a local file's are: `App::subtitleOptions` offers a server stream that is not external and is text as `Embedded` (`embyStreamReadWithVideo`), trusting the server's index -- the server numbers a file's streams as FFmpeg does -- until the source is open, and then as far as the source has a reader for it; `adoptEmbeddedSubtitles` turns a shown stream the source does not read after all into an `EmbyStream` request for the same index. The first two are parsed whole into an immutable `SubtitleTrack` held in `subtitles_`. Nothing is read on the window thread: `loadSidecarSubtitles` and `loadSubtitleFile` list, read (bounded at 16 MiB before the read) and parse on a detached thread and post the result through `MainThreadQueue`, where it finds its pane by `subtitleSerial_` -- a number unique across panes, moved by `swapPanes` and retired by `resetPaneMediaState` -- and is dropped when the pane has moved on or another choice was made meanwhile.

Embedded streams are read by the video worker, not a second time. `SourceOptions::readSubtitles` (set for local files by `App::localSourceOptions` and for an Emby item's stream by `embyStartResolved`; not for a photo) has `VideoSource::openSubtitleDecoders` open a decoder for every text subtitle stream (`AV_CODEC_PROP_TEXT_SUB`) and leave those streams undiscarded on the video demuxer; `decodeSubtitlePacket` turns each packet's ASS event (`cueFromDecodedAss`, under the header `parseAssHeader` read from the decoder) into a cue and `insertSubtitleCue` adds it to that stream's track, in order and once -- a seek back passes the same packets again. The tracks live under `subtitleMutex_`; `subtitleLinesAt` answers for the stream `setSubtitleTrack` named, so a change of stream is a change of which track is asked and shows at once. A Matroska file keeps a line in the cluster of its first moment, so a seek that starts reading at a keyframe would never see the line being spoken there: `handleVideoSeek`, when there are subtitle readers and the container is Matroska, looks the landing keyframe up in the stream's index and seeks again to a little before it (`kSubtitlePrerollSeconds`, fewer in a high-bitrate file so that at most about 24 MiB is read, never under two), and the read loop passes video packets over until that keyframe (`videoSkipToKeyframe_`). A scan of the whole file for its subtitles was considered and not done: Matroska's demuxer reads every block to skip it, which on a share is the whole file. Streams of pictures (PGS, VobSub) are listed by `subtitleTracks` with `text` false and offered greyed. For libass the same packets are kept as events as well (`SubtitleEvent`, `subtitleEventsSince`), with the decoder's header (`subtitleStreamHeader`): an ASS or SSA stream's own, which `syncAssSubtitles` passes on, or FFmpeg's stand-in for any other text stream, which is replaced by the plain header so that a SubRip stream inside a file looks like an SRT beside it; so is an ASS stream that came without its header, whose events libass would otherwise drop for want of a format line. An event is kept once, by its times and its text and, for an ASS or SSA stream, its read order (a script may write a line twice on purpose); a plain stream's events get read orders of the source's own, because its decoder counts from 0 again after every seek and libass drops an event whose read order it has seen. Lines arrive as the demuxer reaches them, so a file whose subtitle packets were not interleaved with the video (all of them late in the file) shows them only when playback gets there. `openVideoInput` also collects the container's font attachments (by codec, MIME type or name; at most 256 MiB) for every source, a server's stream included, and the matrix the video's colours are decoded with.

What is shown when the viewer has not chosen is `App::autoChooseSubtitle` and the pure `chooseSubtitle`/`rankSubtitles`: the stream the Emby server names in `DefaultSubtitleStreamIndex` (whether the server or the video hands it over), where it names one -- an account whose subtitle mode is `Smart` with no language names none for anything, which is why this exists -- and otherwise the candidate that suits the wanted languages best. `subtitleLanguageTag` reads a language from a container's or the server's code and, failing that, from a title or a file name's suffix (`chs`, `SC`, `unibig5`, a name in its own script); `subtitleLanguagePreference` is the viewer's choice (`SubtitleSettings::language`) before Windows' own language list, the languages other than English first; `subtitleLanguageRank` orders exact script, same language, a file naming no language, the next wanted language, a stream naming none, and anything else. A local file's sidecars are ranked on the scan's thread (`scanSidecars`), its embedded streams by `SourceOptions::chooseSubtitle` on the decoder's thread the moment the container's streams are known, and `adoptEmbeddedSubtitles` (from `tick()`) takes the source's choice up once it is ready unless a file beside the video is already shown. An Emby item's streams are ranked as one list, those the video carries with those the server hands over, as they were when the server handed them all over. A choice at the menu, the keys or the sheet sets `subtitleChosenByViewer_`, after which nothing found later chooses for that pane. The keys are PotPlayer's (read from its own language file): Alt+H, Alt+L, Alt+O, `.` `,` `/`, Alt+PgUp/PgDn, Alt+Up/Down/Home -- Alt keys arrive as `WM_SYSKEYDOWN` with bit 29 set and are routed by `handleAltKey`, their `WM_SYSCHAR` swallowed. The Subtitles menu (`appendSubtitleMenu`) is shared by the pane menu and `BarItem::Subtitles`. `SubtitleSettings` (show, language, size, position, background; `Core.hpp`) is the `subtitles` line, five words since `QCONFIG 12`; a `QCONFIG 11`, which drew the box behind every line and had no word for it, is read as without the box, and older files as shown, automatic, default size and place. Before `QCONFIG 14` the position was every bottom line's height above the pane's edge; such a value is dropped to 0, the script's own place, when it is read.

Parsed subtitle tracks keep a prefix-maximum end-time index alongside their start-sorted cues. A query first finds the cues that have started, then skips only a prefix whose cues have all ended and filters the rest with start-inclusive/end-exclusive boundaries. This preserves long-lived signs beneath arbitrarily many short dialogue cues and works equally after forward or backward seeks. The index is built once before the track becomes shared and immutable for playback; manually constructed tracks without an index use a complete scan of the started cues.

Audio tracks: `VideoSource` lists the container's audio streams when the video input opens, `setAudioTrack` names one by stream index, and the audio worker honours it when it opens and, for a running source, on its next seek -- the demuxer is repositioned anyway, so the decoder is swapped first (`openAudioCodec`), discards updated, and the timeline reset as for any seek. `App::selectAudioTrack` follows the request with the same audio-only relocation an output change makes, or rebuilds the seek barrier when one is active.

## The local folder as the list, and Explorer's file types

One local video brings its folder with it. `openSource`, when the pane it opened is the only one taken, calls `refreshLocalList` (`AppLocal.cpp`): the folder is listed on a detached thread (`scanLocalFolder`: media files by `kVideoExtensions`, with size and write time, in Explorer's order by `StrCmpLogicalW`; `markSubtitleFiles` notes each video that a subtitle file of the folder fits by `subtitleNameFitsVideo`, every video it fits rather than `videoForSubtitle`'s longest, because that is what `scanSidecars` loads when the video plays) and arrives through `MainThreadQueue` under a serial, as sidecar subtitles do, because a share that has gone away must not freeze the window thread; the same folder is not read twice unless `Refresh` forces it. `localEntries_` is the listing as read, `localList_` the listing ordered by the browser's sort key with the pure `orderLocalEntries` (`LocalPlaylist.hpp`: size or date on top of the name order, reversed on request, one shuffle per folder and session for random; length and the other keys a folder cannot answer stay by name). `siblingFiles` supplies the visible listing as a fallback only for manually opened local sources without a browser-managed pane queue; queued browser sources follow their pane snapshot. The same thread then reads each file's length and whether a text subtitle stream is inside (`probeLocalFile`: FFmpeg opens the container, and looks two megabytes further only when opening did not say a length; a stream counts by `VideoSource`'s rule, text with a decoder, so pictures such as PGS do not; an interrupt callback ends it when the scan is superseded or the player is gone, through `ScanToken`) and posts them a few at a time to `applyLocalProbes`; what was read is kept by path in `localProbes_` so a refresh does not read it again. `buildLocalRows` tags a video with either kind of subtitles (`localHasSubtitles`) `Sub`: `PanelRow::tag` on a list row, drawn before the value, `tileTags` on a tile, at the picture's bottom-left. By length a file whose length is not read yet sorts last, whichever way the rest run, and takes its place when it arrives.

The sheet keeps its place. Settings, the Emby browser and the folder list each have a slot (`sheetScrollSlot`); `rememberSheetScroll` stores the scroll when the sheet closes or changes what it shows and `restoreSheetScroll` brings the slot of what it shows next, so F6 reopens a list where it was left. Reopening the Emby browser keeps the page shown and asks for it again in place (`embyRequestPage(0, true)`: nothing emptied, no "Loading" row, the whole list shown asked for rather than its first page; populated Random pages and search results are left as they are, using the same keepsOrder rule as automatic refresh; an outstanding initial, further-page or refresh request is not superseded by reopening). Choosing Random again explicitly requests another shuffle. A sort chosen while the Emby sheet is hidden retires the old browser serial and clears its cached list and request flags, so the next open fetches that choice once; it neither requests a hidden playlist nor resets another sheet's scroll. A browser that stays up keeps itself fresh the same way, because what was watched, added or renamed on the server -- in another client, or by this player's own reports -- shows only when the page is asked for again: `emby::listRefreshDue` (pure) says when, from an `emby::ListRefreshState` that `embyRefreshState` fills and `embyTick` asks every frame (`embyRefreshWhenDue`). It is due when there is news (`embyWantRefresh`: `WM_ACTIVATE` bringing the window back to the front, a Playing or Stopped report sent -- the request goes out behind the report on the same queue) and every thirty seconds the browser is up; never while a press or a drag is under way or a request for the page is out (`embyRefreshing_`, which also keeps `embyLoadMoreNearEnd` from overtaking it with a newer serial), not within two seconds of the last asking, not for a page never asked for, and not where asking again would not keep the order -- a random order, a merged search. A list grown past 2000 items is asked for again on news only. Such a request is quiet (`embyRefreshQuiet_`): its failure is logged and the list left alone, and the log says once for a page that it is kept fresh and afterwards only when `emby::sameListing` finds the reply differs. The `Refresh` button of the navigation row (`embyRefreshShown`) asks at the viewer's word, runs a search again, and has `ThumbnailCache::forgetFailures` drop the pictures that failed so they are tried once more. A reply that replaces the list under a press gives the press up when the row pressed has come to name another item (`embyPressedItem`, `embyKeepPressOn`), since a row's param is its place in the list. And a page asked for again in place becomes the list being played only when `emby::sameRelativeOrder` holds: its new entries join, but it does not re-order the walk -- by last played the video just started comes first, and the step after it would go back to the one before. `EmbyPage::scroll` notes where a page was left when another was opened from it, and Back returns there once the page has arrived (`embyPendingScroll_`). Another folder, or a page newly opened, starts from its top.

The F6 sheet has two sources, `BrowserSource::Emby` and `BrowserSource::Local`. F6 follows the explicitly selected `Vn` target to its local folder or Emby browser; `Ctrl+E` always opens Emby. A click on a video pane selects the target, while hover does not. `buildLocalRows` shares the browser's List, Posters and Thumbnails layouts, marks every pane playing the same file, and provides a separate Add hit target on every row and tile. Local tile pictures use `file|<width>|<path>` keys. `LocalThumbnailer` (`LocalThumbnails.cpp`) asks `IShellItemImageFactory` on a worker thread in the multithreaded apartment, for a thumbnail or nothing, scaled by the shell to the size asked for, packs the bitmap as BMP bytes and delivers them from `pump()` on the window thread into the same `ThumbnailCache` the Overlay decodes with WIC; `clearPending` answers what was not started with `cancelled`.

### Browser-managed local queues

`localEntries_` and `localList_` are the single visible folder listing, not a shared playback queue. Clicking an item or Add copies that displayed, sorted order into the destination's `LocalPanePlayback::queue`. The local file identity may appear in more than one pane. `Page Up`/`Page Down`, the step controls and EOF handling use that pane's snapshot; `PlayOrder` advances only its owner. Refresh, sorting or browsing another directory changes the visible listing for later opens and Adds, not queues already captured by other panes. When an Explorer-opened local video is joined by F6 Add, its pane adopts the folder queue as well.

Local Add uses the first logically empty pane; a path or opening source reserves a pane. It clears only the new pane's audio bit, preserving the selected voices of existing panes. The new pane follows the deck's global play/pause state, and Add leaves Solo so the pane remains visible. The same local file can be opened in multiple panes. If all five panes are occupied, `LocalPendingPlay` holds the request and its queue while the user sees each pane's thumbnail and title plus Cancel. The pending sheet makes no playback, queue or audio change; confirmation checks the destination path and media serial again and rejects a stale choice before opening only that pane. Hiding the browser, Back or changing to Emby leaves local playback running.

File types are registered only at the user's request (the `FileTypes` buttons of the settings sheet, or `--register-file-types` / `--unregister-file-types` on the command line, which leave without a window). `fileAssociationPlan` (`FileAssociations.hpp`, pure and tested) is every value written, all under `HKEY_CURRENT_USER`: the ProgIDs `QuadDeck.Video` (`kVideoExtensions`), `QuadDeck.Subtitle` (`kSubtitleExtensions`, the `.ass .ssa .srt .vtt` the player reads) and `QuadDeck.Session` with their open command, the `Applications\QuadDeck.exe` entry, an `OpenWithProgids` value per extension, and the `Capabilities` key named by `RegisteredApplications`. It never writes a type's own default: Windows keeps that choice to the user, so registering ends by opening the Default apps page. Removing deletes the keys that are ours and, from keys that are not, only the values we added. The settings sheet's status line is `associationStatusFrom` (pure): not registered without the video command or the `RegisteredApplications` value, registered elsewhere when the command names another executable, and `Incomplete` when any value of the plan is missing -- a registration from a build before the subtitle types -- which Register completes. No test applies the plan.

## Seeking one video: keyframe landing, and the GPU wait outside the lock

A drag on the bar or an arrow key with one video loaded is a keyframe seek (`VideoSource::requestKeyframeSeek`, `keyframeSeek_` on): the worker seeks with FFmpeg to the keyframe at or before the target -- at or after it for a forward step, `direction` > 0 -- and, instead of gating frames until the exact target and prerolling through the GOP, takes the first decoded frame as the frame: it is published as the exact seek frame, the audio is re-aimed at its time under a new generation, and `takeSeekLanding` hands the owner where it landed; `App::tick` moves the clock to that time (`timelineForSourceTime`) and flushes the audio sink for the new generation. Several videos still seek exactly, or they would not stay in step. Measured on the human's 4K60 HEVC stream with a 10 s GOP through the player's own `VideoSource` (`QuadDeckSeekProbe ... keyframe`): 80 ms to the keyframe against 372 ms for the exact frame with the GPU free.

The seconds the human saw were not the seek: with RTX Video HDR on, Present cost 13-15 ms of GPU time per 60 Hz frame, and the renderer held the device lock across it, so the D3D11VA decoders (which take the same lock through FFmpeg's `AVD3D11VADeviceContext` callbacks) got under 2 ms of every 16.7 and a seek's preroll crawled. `D3DRenderer::render` now ends the frame with an event query, flushes, releases the lock and polls the query (taking the lock only per poll, 0.5 ms apart, 250 ms at most), then re-locks for Present, which then has only the blank to absorb. `RenderStats::gpuWaitMs` reports the wait (`gpuwait=` on the `Perf` line); `held=` is the lock time without it. The Super Resolution extension is set on a stream's processor only when it magnifies (`applyStreamExtensions`): asked for on a video shown at or below its own size the driver still runs its network, which at 4K60 can take most of a frame for nothing; the log says `idle` once and `videoEnhancementNote` says so on the status line.

## One local video is the timeline

The rule lives in `SyncState.hpp`, not in App. `App::paneOrigins` reports
each pane as Empty, Manual (dropped, Open files, command line, session),
LocalBrowser (F6 Play/Add with its own queue), EmbyVideo (including a locator
still resolving) or EmbyPhoto. `deckTimeline` is PerPane as soon as any
LocalBrowser or EmbyVideo pane is present (`App::perPaneTimelines`), Shared
otherwise. `effectiveSeekMode`, used by `App::activeSeekMode`, is the seek
mode in force. It is Independent on a per-pane deck, Linked for exactly one
pane on a shared deck, and the saved `seekMode_` otherwise.
Runtime consumers use this effective mode, while the persisted setting remains
the local preference. Browser seek-bar linkage is separate from this effective
mode, so it never changes a browser pane's repeat or end-of-list policy. In
local-only Independent mode a repeating video wraps its
source time under the running master clock. With one local video, Linked mode
lets the clock reach the end and `finishSingleVideo` applies the playback order;
the pane's repeat chip then toggles `PlayOrder::RepeatOne`. A pane's own A-B
loop still wraps its source time under the clock. A file that replaces the
only manual video -- Page Up/Down, the bar's Previous/Next, one file dropped
on it, the pane's Open -- starts the timeline from zero through
`restartReplacedOnlyVideo`, playing or paused as before, as `advanceAtEnd`
does; among several manual panes the replacement joins the deck's time.

`paneRepeatFlagInForce` decides whether a pane's saved whole-source repeat
flag reaches `PaneTiming::autoRepeat`. The only video never uses it, whatever
its origin: its chip shows the only-video rule, and a lone F6 or Emby pane
also ends through `localFinishPane` / `embyFinishPane` with the playback
order. Manual panes on a per-pane deck keep their shared-deck rule: the flag
counts only among several manual panes under a saved Independent mode.
`LocalPanePlayback::suppressInheritedRepeat` additionally holds an adopted
Explorer pane's flag off until the viewer chooses again.

## The playback order of one local video

`PlayOrder` (`Core.hpp`) defines end behavior: stop, repeat one, advance and
stop, advance with wrap, or random. For one local video,
`App::finishSingleVideo` pauses at the end and `advanceAtEnd` uses
`playOrderNextIndex` on the local siblings in Explorer order. A local
browser-managed pane uses `localFinishPane` and its captured queue, applying
that order only to its own source. `masterTimelineEndAction` continues to govern
multiple local panes opened without browser-managed queues. Emby EOF is also
per-pane: `finishEmbyPanes` applies the same PlayOrder to that pane's queue and
stops/reports only that source. The selected order and popup submenu are stored
in the `QCONFIG 9` line `playorder`. PotPlayer's Ctrl+4/6/7/8/9 are list-sort
keys here: name, size, date, length and random (`App::sortListBy`). Local
sorting changes the visible folder listing and the order copied into future
pane queues; it does not reorder existing local queues. Emby sorting changes
only the selected target pane's queue, with source-page extensions preserving
its existing order. One local video also puts Previous and Next on the transport bar
(`transportBarLayout(..., singleVideo)`), which are Page Up/Down. The A-B keys
are PotPlayer's too: `[`, `]` and `\` (with `L`) for the switch.

The bar's volume is a popup, not a row item: `transportBarLayout` never gives `BarItem::Volume` row width, so the seek rail keeps its length whether the popup is open or not, and places it standing on the speaker (`volumePopupWidth` x `volumePopupHeight`, reaching down to the button so the pointer climbs from one into the other without a gap); `Overlay::verticalRail` draws it and `verticalRailFraction` reads a drag from the bottom up.

## Threads and pause correctness

Both conversion paths select BT.601/709 and full/limited range from each decoded
AVFrame. Software uses swscale colour details before each conversion; hardware
reads the retained owner frame and updates video-processor input/output colour
space before each blit, with automatic driver enhancements disabled. BGRA output
is full range. Unknown or unsupported matrices use BT.709 when original width
exceeds 720 or height exceeds 576, otherwise BT.601. Untagged YUV defaults to
limited range; RGB and legacy JPEG YUV default to full. BT.2020 also takes this
approximation: there is no gamut conversion, transfer conversion or HDR tone
mapping. Pixel tests cover swscale, NV12 and P010 and metadata changes while
reusing the same conversion resources.

Audio seek trimming uses integer 48 kHz output-sample positions. The decoder
seeks back by at least 50 ms (or the codec's larger seek preroll), then the gate
discards only samples preceding the requested position. Frame timestamps are
adjusted for swresample delay; timestamp quantization does not introduce gaps
between adjacent output chunks, and EOF drains delayed output before Ended.
Initial missing PTS falls back to the requested origin and cannot establish
sample-exact source positioning. Synthetic FLAC/WAV tests exercise the actual
audio worker, PCM contents, resampling, EOF, backpressure and superseded seeks.

In Matroska the audio worker seeks by the video stream instead
(`audioSeekPoint`). The index (Cues) of nearly every Matroska file, as
mkvmerge and FFmpeg write it, lists only the video's keyframes, and FFmpeg
seeks a stream the index leaves out by reading the file from its start to the
target: 43 s for 50 minutes into a 6.6 GB film on the human's share, 60-80 s
for 25 minutes into others, silence the whole time and the video's reads
queued behind it. By the video's entries the demuxer lands in the cluster of
a keyframe before the target in milliseconds. It drops the audio block that
began before that keyframe, so this seek aims at least 1 s early
(`kAudioSeekByVideoLeadSeconds`), past any audio block's length, and the gate
above discards the rest. A target within that lead of the video's first frame
seeks by the audio stream, which reads only the start of the file. A
synthetic Matroska film with video-only Cues checks the samples and that a
seek near its end fetches under half the file through the cache.

Each source owns separate video and audio demux/decode threads. Both FFmpeg format contexts are allocated explicitly and carry an interrupt callback tied to the source's running flag, allowing close, replacement, decode-mode changes and device recovery to cancel blocking open, stream-probe and read calls before joining the workers. FFmpeg D3D11VA, rendering and DXGI presentation share one D3D11 device/immediate context and one recursive mutex. The renderer keeps `Present(1, 0)` inside that critical section so presentation cannot race a hardware decoder's immediate-context work. The App waits on the frame-latency object outside the mutex when available; fallback presentation can wait for vertical blank while holding it. Each pane's XAudio2 SourceVoice frequency ratio follows that pane's playback rate; the common mastering voice owns master volume and mute. Device reconstruction keeps the renderer's lost-device latch raised until device, pipeline and back buffer all succeed. App tears decoder sources down only on the first bounded attempt and retains that attempt's master position and resume intent across transient failures, so a later successful retry does not return as a permanent black paused deck. Paths remain logical occupied slots during the retry interval: open/drop/close/session operations update that logical state, while D3D11VA source construction and shader creation are deferred until a valid device exists. Play, pause, stop and seek update the saved recovery intent rather than appearing to work and then being overwritten. Renderer shader creation also checks the device under the shared recursive mutex, so an unexpected caller cannot dereference a released device.

Seeking uses FFmpeg's backward keyframe seek. The decoder must process dependency frames between that keyframe and the requested timestamp, but the video target gate prevents them from entering the render queue. User-visible seconds are converted to the nearest integer stream tick with finite/range checks and saturating origin addition; truncation is deliberately avoided because binary rounding can otherwise turn an exact boundary such as 2.001 seconds in a millisecond time base into the preceding tick. Video, audio and the runtime seek probe use the same conversion. The gate compares integer `best_effort_timestamp` values in stream time-base units and treats a frame as preroll only when its half-open PTS interval ends at or before the target. `AVFrame::duration` defines that interval; when it is absent, `av_guess_frame_rate` supplies a nominal duration rounded upward to whole stream ticks, and a stream with neither falls back to the first PTS at or after the target. This avoids the old fixed 50ms gate selecting multiple expired frames at high frame rates. The worker retains only the most recent raw preroll frame so an EOF seek can promote the final frame once when the requested endpoint lies outside every half-open interval; this defers software conversion and pins at most one extra D3D11VA surface. The audio worker uses its own target-gated strategy after flushing its decoder and resampler. A newer pending seek supersedes frames from an older request.

Timeline moves take one of two styles. A synchronized seek freezes the clock until every participating pane has decoded its own exact destination frame, so playback resumes with all participating panes aligned; its cost is bounded by the keyframe interval. One historical run with four unnamed 4K HEVC sources reached 1.6 to 3.8 seconds, but the corpus and log were not retained, so the range is context rather than a reproducible performance baseline. A fast seek keeps the clock running and lets each pane show its nearest keyframe and converge on its own. Seek-bar drags and relative keyboard seeks are fast, because both are coarse positioning where waiting for the slowest decoder dominates the interaction. Starting playback, master A-B loop wraps, repeat-all restarts and every re-alignment after a structural change (offset edits, pane swaps, decoder changes) stay synchronized.

Fast timeline and relative keyboard seeks enable a two-stage presentation path. The first decoded keyframe may be published once as a static preview only when it is no more than 15 seconds before the exact target; if that keyframe is older, the seek has no preview. Every subsequent dependency frame remains suppressed until the target frame's presentation interval reaches the requested PTS. Software decode sets `skip_frame`/`skip_idct` to `AVDISCARD_NONREF` and skips loop filtering during invisible preroll, restoring full quality 750 ms before the video-stream-clamped decode target. Using the video stream's own final tick here prevents longer audio or container padding from keeping non-reference-frame discard active through EOF. Both demux contexts mark unrelated streams `AVDISCARD_ALL`. Synchronized structural seeks do not request a preview.

`SyncState.hpp` holds the rules themselves, free of Win32, D3D11 and FFmpeg: `PaneTiming` describes one pane, `mappedPaneTime`/`paneBeforeEnd`/`shouldOutputPaneAudio` derive its local time and audio eligibility, `audioHandoff` picks the successor when a selected source's video or audio finishes, and `SeekBarrier` owns the per-pane wait, video/audio generation and frame-ready state that `App` previously kept in parallel member arrays. Audio has four generation-scoped states: Unknown still waits, while Primed, Ended and Error are terminal for the audio half of the barrier. Ended/Error never waive the exact-video requirement, but a short, missing or broken audio stream no longer parks otherwise-ready video until the 30-second fail-safe. `App` supplies the decoder- and device-dependent facts through `PaneTiming` and `SeekBarrier::PaneStatus`, and the barrier reports elapsed time rather than reading a clock, so the core test binary exercises the whole state machine without a window or a decoder.

Every video seek has a monotonically increasing generation. Frames carry that generation plus an exact-target marker, so a frame decoded for a superseded request cannot satisfy a newer synchronization operation. Audio seek state carries its own process-wide generation, and the barrier accepts audio readiness only when that generation matches the request it recorded. A global seek issued while playing pauses the master clock, all SourceVoices and all source audio producers. `App` continues calling `frameForTime(..., advance=false)` to show an allowed static keyframe preview, but does not resume until every active, non-paused, non-finished source has returned an exact frame for its recorded generation and that frame is included in the same render call. The barrier then seeks the frozen clock to the requested master time and starts all selected audio voices and the clock together. Pressing Pause while the barrier is active disables and flushes its audio prefetch immediately; pressing Play again creates replacement audio generations at the unchanged target. A 30-second fail-safe prevents a broken video source from deadlocking playback. First-frame, exact-frame, generation-scoped audio state and barrier timings are appended to `QuadDeck.log`. `Diagnostics.hpp` is the single writer: one process-wide mutex serializes the decoder, renderer and application records, every line includes a local timestamp to milliseconds and the Windows thread ID, the path is resolved once from the executable's own directory (falling back to `%LOCALAPPDATA%\QuadDeck` when that is not writable) rather than from the working directory, and the file is truncated once it passes 4 MB. Packaging starts from a fresh allow-list staging directory, copies only the app-local Release DLLs and never puts diagnostic logs or the PDB in the archive; the unpacked `dist\QuadDeck` gets `QuadDeck.pdb` and keeps the replaced build's `QuadDeck.log`.

A frozen window thread is what Windows reports as "stopped interacting with Windows"; it keeps no dump of it. `HangWatchdog` (in `App::run`) is a thread of its own that `renderFrame` beats once per frame. `renderFrame` also runs from the modal-loop timer, so menus, dialogs and window drags keep beating, and only a window thread that is waiting on something goes silent. `StallTracker`, pure and tested, decides when to look: after 4 s of silence and again after 10 s (stuck or slow), never a third time in one stall, a recovery line when beats return, and nothing when the watchdog's own polls were seconds apart (the machine slept). To look, `describeAllThreadStacks` suspends every other thread in turn and unwinds its stack with `RtlLookupFunctionEntry`/`RtlVirtualUnwind` into a preallocated array, touching no heap or lock while a thread is suspended, since that thread may hold either; dbghelp names the frames after all threads run again, QuadDeck's own from `QuadDeck.pdb` beside the executable (the Release build is linked with `/DEBUG /OPT:REF /OPT:ICF`), other DLLs by their exports. The window thread comes first, and threads with identical stacks are folded. The report goes through `appendDiagnostic`, so a window thread stuck inside the log's own mutex would stop it too.

Video enqueue rechecks its pending-seek state after expensive software conversion. Audio resampling rechecks its generation, and the destination voice independently rejects chunks at or below its flush watermark while holding the same per-voice lock used by submission. The App-side order is therefore disable producer, allocate the replacement seek generation, flush/rebase the voice while preserving that generation, then re-enable it. Both the popup menu and the settings sheet route per-pane repeat changes through that same App operation. Repeat and per-pane A-B edits sample the running master clock once for both old and new mappings; if the current mapping is unchanged, they change future policy without flushing either decoder, while a genuine remap follows the generation-safe reset order or rebuilds an active global barrier. Resuming a pane recomputes its mapped target after clearing the paused flag, because loop/repeat policy deliberately does not affect the held frame and may have changed while paused. Together these checks close the race where work from a superseded request could otherwise refill a queue after `requestSeek` and the XAudio2 flush, briefly exposing an old frame or audio chunk.

When globally or individually paused, `frameForTime(..., advance=false)` can prime a frame after open/seek but does not drain the decoded-frame queue. This prevents malformed or sparse timestamps from letting the picture continue for several seconds after pause.
