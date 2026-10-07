# QuadDeck changelog

Public releases start at 1.0.0. See [README.md](README.md) for current usage and [ARCHITECTURE.md](ARCHITECTURE.md) for implementation and file-format details.

## Unreleased

No changes for a subsequent release yet.

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
- `scripts/build-windows.ps1` builds, runs CTest and creates the package. The archive includes the EXE, required DLLs, documentation, example shaders and dependency licences, excluding logs, PDB files and personal configuration.
- The project uses GPL-3.0. See [README.md](README.md#licence) for the Smart Vibrance Plus algorithm's attribution and third-party dependency licences.
- Logs contain playback diagnostics and thread information when the window thread stops responding for an extended period. A PDB beside a local build supplies function names and line numbers. Logs may contain media paths; review them before sharing.
- The full Windows build and all 14 CTests passed for this public source. Automated tests do not establish real-media, NAS, Emby, audio-device, DPI or NVIDIA display acceptance. Outstanding scenarios are recorded in the [runtime validation register](docs/RUNTIME-VALIDATION.md).
