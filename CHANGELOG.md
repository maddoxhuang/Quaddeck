# QuadDeck changelog

Public releases start at 1.0.0. See [README.md](README.md) for current usage and [ARCHITECTURE.md](ARCHITECTURE.md) for implementation and file-format details.

## 1.2.0 (2026-10-15)

This release simplifies playback controls and expands Emby browsing. Existing
settings, sessions and styles remain compatible with 1.1.0: the formats are
still `QCONFIG 14`, `QDECK 6` and `QSTYLE 3`.

### Emby and browsing

- Move Emby sign-in into the sheet with shared text editing, keyboard navigation
  and input-method handling for the server and user-name fields. Keep failures
  on the form, mask the password and clear it after successful sign-in.
- Add cast portraits and person pages with biography and grouped titles
  across libraries, preserving navigation back to the original information page.
- Use compact browsing menus, Play/Add split buttons, expandable technical
  details and thumbnail replacement rows. Distinguish failed lists from empty
  lists and offer Retry. Local folders offer List and Thumbnails.
- Add an empty-window Open Emby button while signed in.

### Playback controls

- Reveal each pane's seek bar near its lower edge. Hide shared time and seeking
  for all multi-video Independent groups; preserve Linked alignment and offsets.
- Remove the bottom menu/settings buttons and the lone video's corner pill,
  Repeat and Close chips. Keep right-click actions, F5 and mute/pause recovery.
  Show only V1-V5 on multi-pane pills and align single-video Repeat with its end policy.

### Subtitles and settings

- Turn the subtitle button into a left-click visibility switch with a right-click
  track menu. Retain track selection while hidden and show a newly selected track.
- List loaded panes in settings, group per-pane audio controls and show mixed
  view/zoom values.

Real-media playback, live Emby, input methods, NAS interruption, physical audio, HDR and
display-transition acceptance remain unverified.

## 1.1.0 (2026-10-14)

This release adds Emby favorites and in-browser search, improves playback and
audio coordination, and hardens settings, subtitle handling and packaging.
Existing settings, sessions and style formats remain compatible with 1.0.0.

### Emby and browsing

- Add a Favorites page across libraries, heart markers, and server-confirmed favorite / unfavorite actions on information pages, browser items and playing panes. Playback from Favorites follows that list's queue.
- Replace the search dialog with a search box inside the browser. `Ctrl+F` opens it from any browser page; a 450 ms pause or `Enter` starts a search across libraries. Support input methods, selection, clipboard and word editing, clear, focus-aware `Esc`, and grouped results with counts. New searches discard superseded queued requests.
- Report failed and partially failed searches instead of treating failures as empty results. Parse large list replies off the window thread, limit them to 32 MiB, and avoid rebuilding unchanged lists. Limit subtitle-marker requests to two batches at once and prioritise Stopped reports during shutdown.
- Keep browser and F6 navigation at the top while scrolling. Reuse row layout and text measurements, fill only nearby rows, and separate local-thumbnail requests from Emby pictures. Reject oversized images before decoding, downscale to the requested width and limit retained decoded pictures.
- Read sibling folders in the background for manual-file previous / next and end-of-file actions. Expire pending manual steps after 1.5 seconds. Refresh duration and subtitle markers when files change or a previous metadata read failed.

### Playback, audio and interface

- Retain the last picture during seeking until the new frame arrives, without allowing it to satisfy the exact-seek barrier. Align single F6 arrow-key seeking with its bottom bar.
- Correct lost wakeups when stopping decode or cache workers. Retire blocked sources in the background, with at most four closes at once; device recovery and application shutdown still wait for remaining closes. Bound local-thumbnail shutdown and let queued external video opens proceed after a subtitle-folder lookup has held them for ten seconds.
- Wait for GPU frame completion with a fence, falling back to a high-resolution timer. Reuse software conversion buffers, reject incomplete pixel buffers, and avoid needless decoder wakeups. Minimising skips rendering without changing the last useful layout or stopping playback policy.
- Align newly started audio with the running video clock and output latency, use the video's origin for audio timestamps, and re-aim a pane whose audio drift exceeds the correction range. Log start error and drift for every selected audio pane. A lone remaining video muted only by `+Add` receives audio when the heard pane closes; explicit mutes remain in force.
- Keep a manually paused lone video held past the end of its running clock. Seeking away from a browser video's end hold resumes it, while explicit pane pauses remain paused. Preserve a negative-offset pane's scheduled start when changing speed, and explain when a saved Repeat choice cannot apply.
- Show the title strip in full screen, simplify hover controls for a lone video, and fade selection and audio borders with pane controls. Keep Vibrance+ sliders visible only while that shader is selected. Quick repeated clicks work on steppers and rails; toggle keys act once while held. A closing sheet stops intercepting input immediately, and a covered window edge no longer opens the browser.
- Retry failed device recovery with backoff instead of giving up, refresh display information after Windows HDR changes, and continue at the old size while retrying a failed swap-chain resize. Keep an unavailable custom shader's saved path and show its fallback status; preserve native shader ternary expressions and switch labels during compatibility translation.

### Subtitles, security and privacy

- Keep mostly valid BOM-marked UTF-8 subtitles readable after a stray invalid byte; accept CR-only SRT line endings and missing blank separators without stray cue numbers. Recognise drawing-only ASS events and case-insensitive event fields. Use the video's actual conversion matrix for subtitle colours.
- Decode and parse downloaded subtitles off the window thread with the same 16 MiB limit as local files. Show unreadable / oversized files distinctly and skip failed files during automatic selection. Bind subtitle and audio-track menu actions to the media that opened the menu, including after pane swaps.
- Reject redirects and secondary demuxer resources on authenticated Emby media inputs, including seek and reconnect requests. A silent HTTP connection times out after 15 seconds and uses the bounded reconnect path.
- Bound embedded-subtitle retention across a source and use linear scanning for malformed subtitle markup, avoiding unbounded deduplication and repeated suffix scans.
- Sanitize identified build-path strings only in staged binaries, then check the final archive. Original build outputs and dependency installations are retained. Review the final source and package before distribution; packaging checks do not establish an exhaustive privacy audit.

### Settings, diagnostics and packaging

- Preserve newer-format settings unchanged and disable saving for that run. Back up other unreadable settings as `settings.qconfig.unreadable` before saving defaults; do not save if the backup fails. Formats remain `QCONFIG 14`, `QDECK 6` and `QSTYLE 3`.
- Rotate a full log to `QuadDeck.1.log`. Hang reports write raw module-offset stacks before symbol resolution and avoid loader-lock-dependent unwinding. Startup logs identify the version, commit, configuration, link timestamp and matching PDB; network clients send only the application version.
- Reset performance accounting across pauses and source replacements. Record unavailable audio output and audio-engine failures; automatic audio-device recovery is not included.
- Stage and validate a package before replacing it, preserve the previous package when it cannot be moved, and retain both local logs outside the ZIP.

Build and package checks do not establish manual playback, service, input,
audio or display acceptance.

## 1.0.0 (2026-10-08, initial public release)

### Playback and synchronisation

- Native Windows C++20 player using FFmpeg, Direct3D 11 / D3D11VA and XAudio2, with up to five videos in one window.
- Each pane has its own seek bar, signed Offset, speed, pause, volume, mute, whole-file repeat and A-B loop. Multiple audio outputs can be mixed, with automatic handoff when the current audio source ends.
- For ordinary video groups, `Linked` seeks along the master timeline while preserving each pane's Offset and speed mapping; `Independent` moves only the selected pane. Synchronised playback, master-loop wrapping and joint restart use an exact-seek barrier, resuming together after all target frames are ready.
- Timeline drags and arrow keys can show a nearby keyframe first, then decode silently to the exact target while hiding intermediate dependency frames. A single video defaults to keyframe seeking, which can be disabled under `F5 → Playback`.
- Video targets are selected by timestamp intervals. Audio seeking retains samples after the target and accounts for resampling delay. MKV audio uses the video index to avoid reading sequentially from the beginning to the target.
- Videos added pane by pane from F6 / Emby retain their own queues, resume positions and end-of-file rules. Selecting `Linked` does not seek immediately; the next seek aligns them to the selected media time. Multiple `Independent` panes do not show a misleading total duration or total seek bar.
- Windows shares and mapped network drives use in-memory read-ahead by default to absorb short read slowdowns. Local files bypass this cache, and no cache files are written to the NAS.

### Interface and picture

- Six dynamic layouts divide space by video count and aspect ratio, with Solo, drag-to-swap, a focus pane in three- or five-video layouts, and per-pane Fit / Fill / Stretch and zoom.
- The bottom bar, pane seek bars, notices, title area and settings sheet are drawn over the video and hide automatically. `U` pins the bar; `Alt+Enter` toggles full screen.
- The `F5` settings sheet has five tabs: `Playback`, `Audio`, `Subtitles`, `Picture` and `General`. The title area's `Emby ›` / `‹ Settings` switch moves between the browser and settings.
- With two or more videos, the bottom bar offers an arrangement button and the context menu offers `Arrangement` and `Seek bars`. Narrow windows fold controls away while preserving access to essential actions.
- Auto, Hardware and Software decoding are available. Built-in shaders include Normal, Sharpen, Grayscale, Invert and Smart Vibrance Plus, with four live parameters for the latter. PotPlayer-style `.txt` and modern `.hlsl` pixel shaders can also be loaded.
- NVIDIA RTX Video Super Resolution and RTX Video HDR are available when the hardware, driver and display settings allow them. Vibrance+ uses its HDR algorithm when RTX HDR is active.
- The application declares Per-Monitor V2 DPI awareness. Layout behaviour when moving between displays with different scaling still needs hardware validation.

### Local lists and Emby

- `F6` browses the current local video's folder, showing duration, size, thumbnails and text-subtitle markers, with name, size, date, duration and random sorting.
- A normal click replaces the explicitly selected pane; `+Add` adds a muted video to an empty pane. When all five panes are occupied, a replacement chooser appears. Each pane follows its own list for previous, next and end-of-file actions.
- `Ctrl+E` opens Emby, supporting libraries, folders, series, playlists, search and Continue Watching, with list, poster and thumbnail views, sorting, filtering and refresh.
- Movies and series can show information pages, backdrops, metadata and season / episode selection, enabled per library. Reopening a random list preserves its order; an explicit refresh or random-sort selection can reshuffle it.
- Emby and local videos can play together. Each Emby pane independently manages pause, position, resume, queue and playback progress reporting. Windows DPAPI encrypts the stored sign-in token.
- Emby uses direct playback without transcoding. Items that cannot play directly produce a notice.

### Subtitles and Explorer

- Subtitle sources include same-named sidecar files, manual file selection, text tracks inside containers and Emby subtitles. Automatic selection follows language and server preferences; manual selection takes priority.
- ASS / SSA uses libass for script styles, positioning, fonts, animation and effects, including fonts attached to MKV files. libass also renders SRT / WebVTT and other plain-text subtitles.
- Embedded subtitles in directly played Emby containers are read with the video; external subtitles are fetched separately, so extracting embedded subtitles does not block sidecar requests. The `Sub` marker in F6 and Emby lists indicates displayable text subtitles.
- Subtitle switching, delay, size and bottom-position adjustment are supported. Content explicitly positioned by the script retains its position. Graphic subtitles such as PGS and VobSub are not displayed.
- `F5 → General → Register…` registers video and subtitle file types for the current Windows user. The user chooses the default application in Windows.
- Explorer file opens are forwarded to the existing window in the same user session. An empty window forms a video group; an occupied window adds muted videos to empty panes; a full window offers a replacement choice.
- Opening a subtitle from Explorer first tries to match a same-named video in the same folder. An empty window can open that neighbouring video with the subtitle.

### Persistence and compatibility

- `.qdeck` stores sessions and video paths; `.qstyle` stores visual presets without video paths; `%LOCALAPPDATA%\QuadDeck\settings.qconfig` automatically stores application settings.
- Current formats are `QDECK 6`, `QSTYLE 3` and `QCONFIG 14`. Readers accept the earlier versions of each format. These file-format versions are maintained separately from application version 1.0.0.
- Importing older settings resets the subtitle position to the current relative-lift default while retaining other valid subtitle settings. Older applications may not read newer formats; back up settings and sessions before switching application versions.
- Saves write and flush a complete temporary file in the same directory before replacing the destination, preserving the original file on failure. Reads validate UTF-8, accept a BOM and limit document size; repeated settings adjustments are coalesced into one write.
- Close the running application normally before upgrading. File associations can be registered again after changing the EXE location. See [README.md](README.md#upgrading-and-building) for installation, build and compatibility instructions.

### Build, licensing and validation scope

- Builds use MSVC, CMake and vcpkg. `vcpkg.json` pins the dependency baseline, and a Windows GitHub Actions build workflow is provided.
- `scripts/build-windows.ps1` builds the application and creates the package. The archive includes the EXE, required DLLs, documentation, example shaders and dependency licences, excluding logs, PDB files and personal configuration.
- The project uses GPL-3.0. See [README.md](README.md#licence) for the Smart Vibrance Plus algorithm's attribution and third-party dependency licences.
- Logs contain playback diagnostics and thread information when the window thread stops responding for an extended period. A PDB beside a local build supplies function names and line numbers. Logs may contain media paths; review them before sharing.
