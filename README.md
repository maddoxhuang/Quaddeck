<p align="center">
  <img src="https://raw.githubusercontent.com/maddoxhuang/Quaddeck/main/assets/QuadDeck.png" alt="QuadDeck icon" width="112" height="112">
</p>

<h1 align="center">QuadDeck</h1>

<p align="center">
  <strong>Five videos. One window. Your timeline.</strong><br>
  A native Windows player with synchronised or independent playback,<br>
  hardware acceleration, Emby integration and styled subtitles.
</p>

<p align="center">
  <a href="https://github.com/maddoxhuang/Quaddeck/actions/workflows/windows-build.yml"><img src="https://github.com/maddoxhuang/Quaddeck/actions/workflows/windows-build.yml/badge.svg?branch=main" alt="Windows Build status"></a>
  <a href="CHANGELOG.md"><img src="https://img.shields.io/badge/version-1.0.0-0891b2" alt="Version 1.0.0"></a>
  <a href="#upgrading-and-building"><img src="https://img.shields.io/badge/Windows-10%20%2F%2011-0078d4" alt="Windows 10 / 11"></a>
  <a href="CMakeLists.txt"><img src="https://img.shields.io/badge/C%2B%2B-20-00599c" alt="C++20"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--3.0-16a34a" alt="GPL-3.0 licence"></a>
</p>

<p align="center">
  <a href="#get-started">Get started</a> ·
  <a href="#feature-overview">Features</a> ·
  <a href="#keyboard-shortcuts">Shortcuts</a> ·
  <a href="CHANGELOG.md">Changelog</a> ·
  <a href="https://github.com/maddoxhuang/Quaddeck/issues">Report an issue</a>
</p>

---

## Feature overview

| | What you can do |
| :--- | :--- |
| **Five video panes** | Combine up to five videos, choose a layout, focus one pane or drag to swap. |
| **Your choice of timeline** | Link playback, seek independently or offset each video against the master timeline. |
| **Audio per pane** | Mix several sources and control each pane's volume, mute and audio track. |
| **Local files + Emby** | Browse folders or your Emby library and build a separate queue for each pane. |
| **Styled subtitles** | Render ASS / SSA with libass, including attached fonts, or load SRT / WebVTT. |
| **GPU decoding + shaders** | Use D3D11VA, live Vibrance+ controls and RTX Video features on supported setups. |

<details>
<summary><strong>Explore all features</strong></summary>

- **Synchronised playback:** up to five videos composed in one window and one D3D11 swap chain, following one master timeline. Pressing play, a master A-B loop wrapping around and a joint restart use an exact synchronised seek: every pane presents its target frame, then all start together.
- **Per-pane Offset:** each pane has a signed offset (seconds, fractions allowed) from the master time. The `0` key zeroes every Offset at once, paying for one synchronised seek.
- **Independent seek bars:** each pane has its own bar, scaled to its own duration. `Linked` drags everyone; `Independent` moves one pane and can loop it on its own. With a single video, that video is the timeline.
- **Browser videos aligned on request:** videos added pane by pane from F6 or Emby default to independent positioning. While several of them play independently, the bar shows no total time or total seek bar; only the per-pane bars are used. After choosing `Linked` in `F5 → Playback → Seek bars`, the bottom bar's seek bar returns and clicking any pane's bar or the bottom bar jumps every reachable pane to the same point in time; queues and end-of-file rules keep running per pane.
- **Fast seeking:** dragging and the arrow keys first show the nearest keyframe, then decode silently to the exact position without showing the dependency-frame chase. A single video lands on the keyframe by default (`F5 → Playback` turns that off).
- **Multi-audio:** several panes can be output and mixed at once; each has its own volume and mute, multiplied by the master volume. When the current source finishes first, the audio hands over to the next pane automatically. Right-click `Audio track` switches tracks.
- **Layout:** cells are divided dynamically by the number of loaded videos and their aspect ratios (Auto aspect, one row, two rows, one column, portrait main pane, landscape main pane). Solo, drag to swap, choose the focus pane, and Fit / Fill / Stretch plus zoom per pane. With two or more videos loaded, the bottom bar has an arrangement button and the context menu has `Arrangement` and `Seek bars`; with one video, neither appears.
- **Decoding and shaders:** Auto, Hardware (D3D11VA zero copy) and Software decoding; built-in Normal, Sharpen, Grayscale, Invert and Smart Vibrance Plus with four live parameters; PotPlayer-style `.txt` or `ps_4_0` `.hlsl` shaders can be loaded too.
- **NAS read-ahead cache:** Windows shares and mapped network drives read about 30 seconds ahead in the background by default to smooth short slowdowns; local files are not cached.
- **NVIDIA RTX Video:** `F5 → Picture` offers Super Resolution (only when the picture is actually upscaled) and RTX Video HDR (10-bit PQ output when Windows has HDR on for the display); 8-bit SDR frames from software decoding can go through it as well. Vibrance+ switches to an HDR-specific algorithm under RTX HDR.
- **F6 local folder list:** lists the folder of the current video with duration, size and Shell thumbnails, sorted by name, size, date, duration or randomly (`Ctrl+4/6/7/8/9`). A video with subtitles (a subtitle file of the same name beside it, or a text subtitle track inside it) carries a blue `Sub` mark. A plain click replaces the target pane, `+Add` adds muted into an empty pane, and each pane steps to its previous / next file along its own queue.
- **Emby browser:** `Ctrl+E` browses libraries, folders, series, playlists and search results as a list, posters or thumbnails, with sorting and filtering done by the server and automatic refresh while open. Videos with text subtitles carry the same blue `Sub` mark. The sign-in token is stored DPAPI-encrypted and only ever sent in an HTTP header.
- **Emby information pages:** movies and series can open an information page with backdrop, poster, metadata, overview and season / episode selection, switchable per library.
- **Emby multi-pane playback:** Emby items can be added to any of `V1`–`V5` next to local videos; each pane has its own queue, pause, position, resume and progress reporting.
- **Subtitles:** when nothing is chosen, an Emby subtitle stream, a sidecar file of the same name or a text subtitle track inside a local file is picked by language. ASS/SSA is drawn by libass exactly as the script says (position, fonts, colours, outline and shadow, rotation, movement, fades, blur, clipping, vector drawings, karaoke, and fonts attached to an MKV); SRT and other plain text is drawn white with a black outline. The shortcuts are PotPlayer's (`Alt+H`, `Alt+L`, `Alt+O`, `.` `,` `/` and so on). Graphic subtitles are not shown.
- **Explorer file types:** `Register…` under `F5 → General` registers QuadDeck for the current user as a program that opens video and subtitle files (`.ass .ssa .srt .vtt`), writing only `HKEY_CURRENT_USER`; then pick it in Windows' Default apps. Double-clicking a subtitle file adds it to the pane playing the same-named video from the same folder, otherwise to the current pane; with no video in the window, the same-named video from that folder is opened with that subtitle.
- **Single instance:** one QuadDeck per user and session. Files handed over by Explorer go to the existing window: an empty window forms a synchronised group, a window with videos adds them muted into empty panes, and with all five panes full you choose which pane to replace.
- **Sessions and settings:** `.qdeck` saves video paths, master time, layout, audio and per-pane settings; `.qstyle` visual presets contain no video path; `settings.qconfig` saves player settings automatically. After a format upgrade an older build cannot read the new file.
- **Interface:** the bottom bar, per-pane labels and buttons, notices and the settings sheet are all drawn with Direct2D over the video and hide themselves; the process declares Per-Monitor V2 DPI awareness.
- **No title bar:** like PotPlayer's "auto-hide under video", the window has no Windows title bar and the picture fills the whole window; the title and the minimise, maximise and close buttons are drawn along the top edge and appear and hide with the bottom bar.

</details>

## Get started

- **Build locally:** follow the [requirements and build instructions](#build-from-source) to create the Windows application and portable ZIP.
- **Try a CI build:** sign in to GitHub, open [Windows Build](https://github.com/maddoxhuang/Quaddeck/actions/workflows/windows-build.yml), choose a successful run for `main` and download **QuadDeck-win-x64** under **Artifacts**. Extract the downloaded artifact, then the `QuadDeck-win-x64.zip` inside it; run `QuadDeck\QuadDeck.exe` with its DLLs beside it. A run must finish successfully before its package is available.
- **Start watching:** press `O` or drop videos into the window, use `F6` for the local list, and `Ctrl+E` for Emby. Open `F5` to adjust playback, audio, subtitles and picture settings.

The [1.0.0 release notes](CHANGELOG.md) summarise this release. See [compatibility guidance](#settings-and-compatibility) before replacing an existing installation.

## The bottom bar and settings

The bottom bar is a row of controls drawn over the video: play / pause, stop, time, the **master seek bar**, mute, volume, subtitles, arrangement (only with two or more videos loaded), menu, full screen and settings; with a single video the play button also has previous / next beside it. In an ordinary multi-video group the master seek bar positions every video on the master timeline and drags with the fast seek; with a single video the time and the bar are that video's own. While several F6 / Emby panes are `Independent` there is no common time, so neither the bar nor the window title shows a total time and the bar has no total seek bar; the per-pane bars and the other buttons keep working. After `Linked`, the bar's seek bar returns and aligns the videos by their own seconds. When the window is too narrow for the whole row, controls fold away in a fixed order and hand their space to the seek bar; a folded control is still reachable by shortcut, the `F5` sheet, the context menu or the wheel.

Layout, audio output, decoder, shader, Offset and seek mode used to be on the bar too, duplicated three times with the settings window and the context menu, and all truncated when the window was narrow. Items that describe state (decoder, shader, Fit / Fill of all videos, presets) live only in the `F5` sheet; arrangement and seek mode change often while watching several videos, so they are also in the context menu (the `Arrangement` and `Seek bars` submenus, shown only with two or more videos loaded), and the arrangement has a bar button as well; audio output and Offset reset are actions in the context menu too.

The settings sheet has five tabs; the tabs stay below the title and the content scrolls beneath them: `Playback` (seek bars, decoder, end-of-file rule, keyframe seek, and the `Timing` group with per-pane Offset steps and looping), `Audio` (master volume, per-pane output and volume), `Subtitles`, `Picture` (arrangement, Fit / Fill / Stretch and zoom for all videos, shader, Vibrance+, RTX), `General` (pinned bar, NAS cache, Explorer file types, visual presets and sessions). Each tab remembers its own scroll position and the sheet reopens on the last tab; when the sheet is too narrow for the tabs they wrap onto two rows. Emby is not a tab: a permanent switch sits to the right of the sheet's title, left of the close button; it reads `Emby ›` on the settings tabs and opens the Emby browser (with the sign-in dialog first when signed out, as `Ctrl+E` does), and `‹ Settings` in the browser and the F6 list to return. Sign-in state and `Sign out` are on the browser's home page, and the `Information pages` switch of each movie / series library is on that library's own page. The home page's navigation row is only `Search…`, `Sign out`, `Refresh`; the title bar's switch goes back to the settings, and subpages still start with `← Back`.

The window has no Windows title bar and the picture fills the whole window, like PotPlayer with "Skin → auto-hide under video". The title (playback state, time and current file name) and the minimise, maximise / restore and close buttons are drawn on a strip along the top edge that appears and hides with the bottom bar: it shows when the pointer touches the window's top or bottom edge and hides after about 1.1 seconds of rest; `U` pins it together with the bar, it stays while the settings sheet is open or no video is loaded, and it is absent in full screen. Drag its empty part to move the window (snapping at screen edges works), double-click to maximise / restore, right-click for the system menu; the top few pixels of the window resize its height, and the left, right and bottom edges are still Windows' own frame. With a single video (or in Solo) dragging the picture itself also moves the window; with several, dragging the picture still swaps panes. The taskbar and Alt+Tab show the same title; per-pane decode statistics go to `QuadDeck.log`. With nothing loaded, the middle of the window explains drag-and-drop and the shortcuts.

## Independent seek bars and synchronisation

Every loaded video has its own `current / total` time and seek bar; the videos need not be the same length. A shorter video holds its last frame when it ends while the others go on.

The bars are auto-hiding layers over the bottom edge of each video and take no layout height; they show on pointer movement and hide after about 1.4 seconds of rest. A press or drag maps the track pixel straight to 0–100% of that video and the seek is performed once, on release; the track's empty part no longer triggers the native trackbar's large Page Up / Page Down step.

Dragging a bar is a fast seek: the picture appears as soon as you let go, the master clock keeps running and every pane converges on the exact position in the background without waiting for the slowest one.

`Seek` has two behaviours:

- `Linked`: dragging any pane derives the master time from that pane's target and every video jumps together.
- `Independent`: only the dragged pane jumps; the master time and the other videos stay, and that pane's sync correction is saved. The settings tab can let each pane go back to its own start at its end and keep playing. In this mode `←` / `→` also move one pane: the Solo pane when in Solo, otherwise the pane the pointer last touched, by the same rule as `Page Up` / `Page Down`. In `Linked` the arrow keys still move the whole master timeline.

Videos played from F6 or Emby or added with `+Add` keep their own lists, resume positions and end-of-file rules. Their `Seek bars` start as `Independent`; choosing `Linked` **does not move anything by itself**. The next click or drag on any pane's bar, on the bottom bar, or `←` / `→` aligns every video that can reach the target time to the same second: clicking 10:00 of a 20-minute video takes a 40-minute one to 10:00 as well. Until the first alignment the bar shows the chosen pane's time, afterwards the linked target time; the scale covers the longest duration present. A video that has not finished opening makes that alignment wait for a later click rather than moving only part of the group; a failed video must be closed first, and one whose duration is unknown cannot take part. A video shorter than the target follows its own end-of-file or loop rule, and an existing A-B loop still applies. Stepping a pane to the next item of its list starts from that item's own beginning; click again to align when needed. The browser's Linked choice lasts for this run only; once the browser videos are closed, an ordinary manual group still uses the Linked / Independent choice it saved.

While several F6 / Emby panes are `Independent`, the master clock may be far ahead of a newly added video and cannot stand for their common duration, so the bar shows no total seek bar or total time and the window title no total time either. Each pane's own bar is still clickable, and the bar's play, stop, volume and other buttons keep working.

`Offset V1`–`Offset V5` state directly where each video sits relative to the master time (seconds, fractions allowed): `+12.5` means the video is at 0:12.5 when the master time is 0:00; `-12.5` means it holds its first frame and starts when the master time reaches 0:12.5. Changing an Offset merges an old session's `Delay + independent correction` into one plain signed value.

In an ordinary manual group a pane still in `wait` can have its own bar dragged. Whether Linked or Independent, QuadDeck then moves only that pane's timeline and does not force the master time to its planned start; the video shows the chosen position at once and takes part in the sync from there. After choosing Linked for an F6 / Emby group, dragging a `wait` pane's bar aligns every ready video by the rule above.

## Layout and mouse

- Six layouts divide the window dynamically by the loaded videos and show no empty cells; `Auto aspect` handles mixes of portrait and landscape by their aspect ratios.
- With three or five videos, the context menu chooses which pane is the large one; the other two of three stack on the right, the other four of five form a 2x2 on the right. Choosing that pane again returns to the automatic main pane.
- With two or more videos loaded, an arrangement button sits right of the subtitle button on the bar and opens the six layouts; the `Arrangement` and `Seek bars` submenus of the context menu and the choices on the `Picture` / `Playback` tabs of the sheet are the same state. With one video the bar has no such button.
- Double-click a picture for Solo; double-click again or press `Esc` to return.
- The dock hides itself and comes back when the pointer nears the window's bottom edge; moving back onto its visible part while the slide-out animation is still running reverses it at once. `U` toggles Auto / Pinned. Neither the dock nor its hidden state takes video layout space.

## Decoding and pixel shaders

- `Auto`: prefers shared D3D11VA zero-copy hardware decoding and allows software decoding where unsupported.
- `Hardware`: requires D3D11VA strictly; a failure is reported in the title and in `QuadDeck.log`.
- `Software`: forces CPU decoding and swscale, mainly for compatibility diagnosis.

Switching the decoder reopens the loaded videos and returns to the current time.

Built in: Normal, Sharpen, Grayscale, Invert, Smart Vibrance Plus. Smart Vibrance Plus defaults to Intensity 1.5, Saturation pivot 0.5, Gray pivot 0.003, Gray sharpness 45, adjustable live on the Picture tab of the settings sheet (`F5`) and saved automatically. The SDR algorithm is a rewrite of the [Smart Vibrance for ReShade](https://github.com/aston89/Smart-vibrance-for-reshade) effect (GPL-3.0), running inside QuadDeck's D3D11 composition stage with no ReShade installed.

`Shader...` still loads a PotPlayer-style `.txt`, or a modern `ps_4_0` `.hlsl` whose entry point is `main`:

```hlsl
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return videoTexture.Sample(videoSampler, uv);
}
```

`shaders/example-warm.hlsl` can be tried as is. Shaders run in the final D3D11 composition stage and leave the hardware decoding path intact.

## Keyboard shortcuts

| Action | Shortcut |
|---|---|
| Add a video | `O` |
| Previous / next file in this pane | `Page Up` / `Page Down`; with a single video the bar has buttons too |
| Toggle the list (the folder of a local video, the library of an Emby item; docked on the right while playing) | `F6` |
| Open the Emby library | `Ctrl+E` |
| Sort the list: name / size / date / duration / random (again for reverse) | `Ctrl+4` / `Ctrl+6` / `Ctrl+7` / `Ctrl+8` / `Ctrl+9` |
| Open / save a session | `Ctrl+O` / `Ctrl+S` |
| Play / pause | `Space` |
| Media play / pause | the keyboard's `Play/Pause` key |
| Volume | mouse wheel, 5% per notch |
| Zero every Offset | `0` |
| Jump 5 seconds back / forward | `←` / `→` |
| Jump 30 seconds back / forward | `Ctrl+←` / `Ctrl+→` |
| Back to the start | `Home` or `R` |
| Toggle a pane's audio output | `1` / `2` / `3` / `4` / `5` |
| Mute | `M` |
| Set the master loop's A / B | `[` / `]` |
| Toggle the master A-B loop | `\` or `L` |
| Show / hide subtitles | `Alt+H` |
| Next subtitle (off after the last one) | `Alt+L` |
| Load a subtitle file | `Alt+O`, or drop a subtitle file onto the picture |
| Subtitle 0.5 s earlier / later, reset | `.` / `,`, `/` |
| Subtitle size up / down | `Alt+PgUp` / `Alt+PgDn` |
| Raise / lower bottom subtitles (relative to their own position), back to normal | `Alt+↑` / `Alt+↓`, `Alt+Home` |
| Auto-hide / pin the bar | `U` |
| Open the settings sheet | `F5` |
| Full screen | `Alt+Enter` |
| Leave Solo / full screen | `Esc` |

## Upgrading and building

### Portable package

Extract the complete `QuadDeck-win-x64.zip` archive and run `QuadDeck\QuadDeck.exe`. Keep the EXE, DLLs, `shaders` and `licenses` folders together. When replacing an existing installation, close QuadDeck normally first and extract into a new folder so that old DLLs cannot remain beside the new EXE.

### Build from source

Open PowerShell in a checkout or extracted source directory, then run:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1
.\dist\QuadDeck\QuadDeck.exe
```

When updating an existing source checkout, keep its `build` directory to reuse installed dependencies. If the CMake cache is incompatible, rebuild it with:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1 -Clean
```

A first build needs Windows 10/11, the **Desktop development with C++** workload of Visual Studio 2022 Build Tools, MSVC v143, a Windows SDK, Git and CMake. The script reuses or installs `%USERPROFILE%\vcpkg`, runs the tests and produces `dist\QuadDeck\QuadDeck.exe` and `dist\QuadDeck-win-x64.zip`. Every packaging run rebuilds a clean release directory; the previous `QuadDeck.log` stays in the new one, and `dist\QuadDeck` also holds `QuadDeck.pdb` (so the log of a hang names functions and lines). Neither the log nor the PDB enters the ZIP.

### Settings and compatibility

- Close an existing QuadDeck process before starting a replacement. A build without the current single-instance protocol cannot receive files from the new one; QuadDeck does not terminate it for you.
- Settings live in `%LOCALAPPDATA%\QuadDeck\settings.qconfig`, outside the application folder. Back up that file and any `.qdeck` sessions or `.qstyle` presets before upgrading or switching builds. Copies of QuadDeck run under the same Windows account share the automatic settings file.
- This release writes `QCONFIG 14`, `QDECK 6` and `QSTYLE 3`, and reads the earlier versions of each format. These are file-format versions, independent of the application version. Older applications may not read files saved by this release. When importing older settings, the subtitle position resets to the new relative-lift default; other valid subtitle settings are retained.
- Sessions store video paths, and visual presets store external shader paths; they do not embed those files. Keep media and custom shaders at their original paths, or select them again after moving them.
- If Explorer still launches a previous EXE, use `F5 → General → Register…` in the new copy and choose QuadDeck in Windows' Default apps. Registering again also adds subtitle file types if an earlier registration did not include them.
- If an imported window size looks too small after a DPI change, resize it once; the new placement is saved automatically. Running normally, rather than as administrator, allows drag-and-drop from a normal Explorer window.

## Video data path

```text
Hardware: FFmpeg → D3D11VA texture → Video Processor → BGRA texture → UV/pixel shader → swap chain
Software: FFmpeg → CPU frame → swscale BGRA → D3D11 texture → UV/pixel shader → swap chain
```

The hardware path reads nothing back from video memory. Software frames are converted to BGRA at the pane's displayed size: never upscaled past 1:1, the scale rounded to steps of 1/8 (so dragging a window edge does not rebuild the converter per pixel), either side capped at 3840. 1920×1080 is now only the initial value before the first layout hint reaches the decoder. Each pane's audio uses its own FFmpeg demux / decode thread and its own XAudio2 SourceVoice, and the selected SourceVoices are mixed in a common mastering voice.

## Current limitations

- Text subtitles only: graphic subtitles (PGS, VobSub) are not shown. ASS is drawn by libass; a font the script asks for that is neither installed nor attached to the video is substituted. Subtitles inside a file are read while playing, so in a file that was not interleaved by time (subtitles bunched somewhere in the file) they appear only once playback reaches them.
- No rotation / mirroring and no four-pane recording yet.
- Whether Auto can hardware-decode a particular 4K H.264 / HEVC / AV1 file depends on the GPU and driver.
- The five panes align by media time and user correction, not broadcast genlock.
- When run as administrator, Windows may block drag-and-drop from a normal-privilege Explorer; use Open then.
- Per-Monitor V2 DPI awareness is declared, but re-layout while moving the window between monitors of different scaling has not been verified on real hardware.

For a problem report, send the complete window title and the `QuadDeck.log` beside the EXE. When the program has hung (the window says "Not responding") for 4 seconds, the log records where every thread is stopped, again at 10 seconds, and how long the hang lasted once it recovers; all of that is written before you close the window, so killing it loses nothing. When the EXE's directory is not writable, the log is in `%LOCALAPPDATA%\QuadDeck`. The log contains the media paths that were opened; check it for private information before sharing.

## Contributing

Keep QuadDeck a native Windows C++20 application. Before changing the project, read
[`docs/AI-WORKFLOW.md`](docs/AI-WORKFLOW.md) and the [handoff board](docs/handoffs/BOARD.md).
They define worktree isolation, module ownership and the shared MSVC / CMake / vcpkg
verification command for human contributors, Codex and Claude Code.

[ARCHITECTURE.md](ARCHITECTURE.md) explains the implementation; the
[runtime validation register](docs/RUNTIME-VALIDATION.md) records what has and has
not been tested. Please keep documentation in English and include reproduction
steps when [reporting a problem](https://github.com/maddoxhuang/Quaddeck/issues).

## Licence

QuadDeck is released under the [GNU GPL-3.0](LICENSE). The SDR algorithm of the built-in Smart Vibrance Plus shader (`src/D3DRenderer.cpp` and `shaders/Smart-Vibrance-Plus-compatible.txt`) is a rewrite of aston89's [Smart-vibrance-for-reshade](https://github.com/aston89/Smart-vibrance-for-reshade) (GPL-3.0), which is why the whole project is GPL; no GridPlayer source was copied. The bundled FFmpeg (vcpkg's default features, without `gpl`, LGPL-2.1+, dynamically linked DLLs), libass (ISC), FreeType (FreeType License), HarfBuzz (MIT), FriBidi (LGPL-2.1) and the rest keep their own licences; their texts are in the `licenses/` folder of the release package.
