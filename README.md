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
| **Five video panes** | Watch up to five videos in one window, choose a layout and drag panes to rearrange them. |
| **Flexible playback** | Keep videos in sync, adjust their timing with offsets or seek through each one independently. |
| **Audio per pane** | Mix audio from several videos, with separate volume, mute and track controls. |
| **Local files and Emby** | Browse folders or your Emby library and give each pane its own playback queue. |
| **Styled subtitles** | Display ASS / SSA subtitles with their styling and attached fonts, or load SRT / WebVTT files. |
| **GPU decoding and shaders** | Use hardware decoding, adjust Vibrance+ and enable RTX Video features on supported systems. |

<details>
<summary><strong>More features</strong></summary>

- **Playback controls:** shared and independent timelines, per-video offsets, playback speed, repeat and A-B loops. Exact synchronised seeking is used when starting or restarting a group.
- **Layouts:** six arrangements adapt to the videos you open. Focus on one pane with Solo, choose a larger main pane, and adjust each video's zoom or Fit / Fill / Stretch mode.
- **Audio:** play audio from several panes at once, switch tracks and adjust each pane's level. When the selected audio source ends, QuadDeck can switch to the next pane automatically.
- **Folder browsing:** press `F6` for thumbnails, duration and file size. Sort by name, size, date or duration, or shuffle the list. Click an item to replace the current pane; use `+Add` to open it muted in an empty pane. Each pane keeps its own queue.
- **Emby:** press `Ctrl+E` to browse libraries, folders, series, playlists and search results. Choose list, poster or thumbnail views, and open movie details or season and episode lists. Local and Emby videos can play side by side, with separate queues and progress reporting.
- **Subtitles:** choose embedded or external text subtitles, with automatic language selection. ASS / SSA rendering uses libass and supports styling, animation, karaoke and fonts attached to the video. A blue `Sub` badge identifies videos with available text subtitles.
- **Network playback:** a background buffer reads ahead on Windows shares and mapped network drives to help absorb short interruptions. Local files do not use this cache.
- **NVIDIA RTX Video:** enable Super Resolution or RTX Video HDR in `F5 → Picture` on supported GPUs and drivers. Super Resolution applies when upscaling; RTX HDR also requires Windows HDR to be enabled for the display. Eligible software-decoded SDR video can use these features too.
- **Shaders:** choose from Normal, Sharpen, Grayscale, Invert and Smart Vibrance Plus, or load a compatible custom shader. Vibrance+ has separate processing for SDR and RTX HDR output.
- **Sessions and presets:** save a viewing session as `.qdeck` or a visual preset as `.qstyle`. Player settings are saved automatically.
- **File associations:** register QuadDeck in `F5 → General → Register…`, then select it in Windows' Default apps. Opening a subtitle file attaches it to the matching video when possible, or to the current pane.
- **Single instance:** files opened from Explorer go to the existing QuadDeck window. New videos use empty panes; if all five panes are occupied, you choose which one to replace.

</details>

## Get started

1. **Download a build.** Sign in to GitHub, open [Windows Build](https://github.com/maddoxhuang/Quaddeck/actions/workflows/windows-build.yml) and select a successful run for `main`. Download **QuadDeck-win-x64** from **Artifacts**. You can also [build from source](#build-from-source).
2. **Extract and run.** Extract the downloaded artifact, then extract the `QuadDeck-win-x64.zip` inside it. Open `QuadDeck\QuadDeck.exe` and keep the accompanying files together.
3. **Open some videos.** Press `O` or drop files into the window. Use `F6` to browse local files, `Ctrl+E` for Emby and `F5` for settings.

See the [1.0.0 release notes](CHANGELOG.md) for this release and [settings and compatibility](#settings-and-compatibility) before replacing an existing installation.

## The bottom bar and settings

Move the pointer near the bottom of the window to reveal the playback controls. The bar includes play / pause, stop, seeking, volume, subtitles and settings. With one video open, it also offers previous and next buttons. Press `U` to keep the controls visible or let them hide automatically.

Press `F5` to open settings:

| Tab | Settings |
| :--- | :--- |
| **Playback** | Linked or independent seeking, decoder, playback order, offsets and repeat |
| **Audio** | Master volume, per-pane volume and audio output |
| **Subtitles** | Subtitle appearance, timing and position |
| **Picture** | Layout, zoom, shaders, Vibrance+ and RTX Video |
| **General** | Control bar, network cache, file associations, presets and sessions |

Use **Emby ›** to open the Emby browser and **‹ Settings** to return. The settings window remembers the last tab and each tab's scroll position.

Move the pointer to the top edge to reveal the title and window buttons. Drag the title area to move the window, or double-click it to maximise or restore. When playing a single video or using Solo, you can also drag the picture to move the window.

In a narrow window, some controls are hidden to leave room for the seek bar. They remain available through shortcuts, settings or the right-click menu.

## Independent seek bars and synchronisation

Each video has its own seek bar and elapsed / total time. The videos can have different lengths. Click or drag a pane's bar to choose a position; dragging applies the seek when you release the mouse.

Choose **Seek bars** in `F5 → Playback`:

- **Linked:** seek through the group together, keeping the timing offsets between videos in a manually opened group.
- **Independent:** seek through one video without moving the others. The arrow keys affect the Solo pane, or the pane most recently under the pointer. Each pane can repeat independently.

Seeking normally shows a nearby keyframe first, then finishes moving to the exact position in the background. For a single video, seeking stops at the keyframe by default; you can change this in Playback settings.

### Videos added from F6 or Emby

These videos start in Independent mode and keep separate queues, resume positions and end-of-file settings. With several independent panes open, the shared seek bar and total time are hidden. Each pane's seek bar and the other playback controls remain available.

Selecting Linked does not move the videos immediately. Your next seek aligns them to the same time in each file. For example, seeking to **10:00** in a 20-minute video also moves a 40-minute video to **10:00**. The shared seek bar returns in Linked mode and uses the longest video's duration; before the first linked seek, it shows the selected pane's position.

Wait for the videos to finish opening before a linked seek, and close any that failed to open. Videos with unknown duration cannot be aligned this way. A target beyond a video's end follows that pane's repeat or end-of-file setting, and any A-B loop still applies. Moving to the next item in a pane's queue starts that item at its beginning; seek again to align it with the others.

The browser group's seek mode applies only to the current run. It does not replace the saved preference for manually opened groups.

### Timing offsets

`Offset V1`–`Offset V5` set each video's position relative to the shared timeline, in seconds:

- **+12.5:** the video is at 0:12.5 when the shared timeline is at 0:00.
- **−12.5:** the video holds its first frame until the shared timeline reaches 0:12.5.

Press `0` to reset all offsets at once. In a manually opened group, seeking a pane marked `wait` moves that pane without jumping the shared timeline to its scheduled start.

## Layout and mouse

- Choose from six layouts in `F5 → Picture`. **Auto aspect** arranges a mix of portrait and landscape videos to suit their proportions.
- With two or more videos, use the arrangement button on the bottom bar or **Arrangement** in the right-click menu.
- With three or five videos, right-click to choose the larger main pane. Select it again to return to the automatic choice.
- Drag a video to another pane to swap their positions.
- Double-click a video to enter Solo. Double-click again or press `Esc` to return.
- Adjust each pane's zoom and choose Fit, Fill or Stretch to control how the picture fills its space.

## Decoding and pixel shaders

Choose a decoder in `F5 → Playback`:

- **Auto:** use D3D11VA hardware decoding when available, with software decoding as a fallback.
- **Hardware:** require D3D11VA. If decoding fails, the error appears in the window title and log.
- **Software:** use the CPU to decode video. This can help diagnose compatibility problems.

Changing the decoder reopens the loaded videos at their current positions.

The built-in shaders are Normal, Sharpen, Grayscale, Invert and Smart Vibrance Plus. Adjust Vibrance+ in `F5 → Picture`; changes take effect immediately and are saved automatically. Its defaults are Intensity **1.5**, Saturation pivot **0.5**, Gray pivot **0.003** and Gray sharpness **45**. ReShade does not need to be installed.

Use **Shader...** to load a compatible PotPlayer-style `.txt` shader or a `ps_4_0` `.hlsl` shader with a `main` entry point. For example:

```hlsl
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return videoTexture.Sample(videoSampler, uv);
}
```

Try [example-warm.hlsl](shaders/example-warm.hlsl) for a simple colour adjustment. Custom shaders process the displayed image and can be used with hardware decoding.

## Keyboard shortcuts

| Action | Shortcut |
| :--- | :--- |
| Add a video | `O` |
| Previous / next file in the current pane | `Page Up` / `Page Down` |
| Show / hide the local folder or Emby list | `F6` |
| Open the Emby library | `Ctrl+E` |
| Sort by name / size / date / duration / random | `Ctrl+4` / `Ctrl+6` / `Ctrl+7` / `Ctrl+8` / `Ctrl+9` |
| Open / save a session | `Ctrl+O` / `Ctrl+S` |
| Play / pause | `Space` or the media `Play/Pause` key |
| Adjust volume | Mouse wheel, 5% per step |
| Reset all offsets | `0` |
| Seek 5 seconds back / forward | `←` / `→` |
| Seek 30 seconds back / forward | `Ctrl+←` / `Ctrl+→` |
| Return to the start | `Home` or `R` |
| Toggle a pane's audio output | `1` / `2` / `3` / `4` / `5` |
| Mute | `M` |
| Set the master loop's A / B points | `[` / `]` |
| Toggle the master A-B loop | `\` or `L` |
| Show / hide subtitles | `Alt+H` |
| Next subtitle track; turn off after the last track | `Alt+L` |
| Load a subtitle file | `Alt+O`, or drop it onto the video |
| Show subtitles 0.5 seconds earlier / later | `.` / `,` |
| Reset subtitle timing | `/` |
| Increase / decrease subtitle size | `Alt+PgUp` / `Alt+PgDn` |
| Raise / lower bottom subtitles | `Alt+↑` / `Alt+↓` |
| Reset subtitle position | `Alt+Home` |
| Auto-hide / pin the controls | `U` |
| Open settings | `F5` |
| Full screen | `Alt+Enter` |
| Leave Solo / full screen | `Esc` |

## Upgrading and building

### Portable package

Extract `QuadDeck-win-x64.zip` and run `QuadDeck\QuadDeck.exe`. Keep the EXE, DLLs, `shaders` and `licenses` folders together. To update, close QuadDeck normally and extract the new package into a separate folder so files from different builds do not get mixed together.

### Build from source

You need Windows 10/11, Git, CMake and Visual Studio 2022 Build Tools with the **Desktop development with C++** workload, MSVC v143 and a Windows SDK.

Open PowerShell in the source directory and run:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1
.\dist\QuadDeck\QuadDeck.exe
```

The script sets up or reuses `%USERPROFILE%\vcpkg`, builds the application, runs the tests and creates `dist\QuadDeck-win-x64.zip`.

Keep the `build` directory when updating the source to reuse installed dependencies. If you need a clean build, run:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1 -Clean
```

Local builds keep `QuadDeck.pdb` beside the EXE for diagnostic logs and preserve the previous `QuadDeck.log` when refreshing `dist\QuadDeck`. Neither file is included in the ZIP.

### Settings and compatibility

- Close QuadDeck before replacing a build or switching to another copy.
- Settings are stored in `%LOCALAPPDATA%\QuadDeck\settings.qconfig`. Copies running under the same Windows account share this file. Back it up, along with any `.qdeck` sessions and `.qstyle` presets, before upgrading.
- This release uses `QCONFIG 14`, `QDECK 6` and `QSTYLE 3`. It can read earlier formats, but older applications may not read files saved by this version. When importing older settings, subtitle height returns to the default position; other valid settings are retained.
- Sessions refer to video paths, and visual presets refer to custom shader paths. They do not include the files themselves, so keep those files in place or select them again after moving them.
- If Explorer opens an older copy, use `F5 → General → Register…` in the new copy, then choose QuadDeck in Windows' Default apps. Registration also supports `.ass`, `.ssa`, `.srt` and `.vtt` files.
- If the window appears too small after changing display scaling, resize it once to save the new size. Run QuadDeck normally to allow drag-and-drop from Explorer.

## Video data path

QuadDeck uses FFmpeg for decoding, Direct3D 11 for rendering and XAudio2 for audio. Hardware decoding uses D3D11VA; software decoding converts or uploads CPU-decoded frames for display. Each pane has its own audio decoder, and enabled audio sources are mixed together.

See [ARCHITECTURE.md](ARCHITECTURE.md#decode-and-render-paths) for details of the rendering, caching and HDR paths.

## Current limitations

- Only text subtitles are supported; PGS and VobSub image subtitles are not displayed. ASS subtitles may use a substitute font if the requested font is neither installed nor attached to the video. In files with poorly interleaved subtitle tracks, some subtitles may appear late.
- Video rotation, mirroring and recording are not supported.
- Hardware decoding support depends on the video format, GPU and driver.
- Synchronisation uses media timestamps and manual offsets; professional genlock is not supported.
- Windows may block drag-and-drop from Explorer when QuadDeck runs as administrator. Use **Open** in that case.
- Moving the window between monitors with different display scaling has not yet been fully tested on real hardware.

When [reporting a problem](https://github.com/maddoxhuang/Quaddeck/issues), include the window title, steps to reproduce it and any relevant log entries. `QuadDeck.log` is normally beside the EXE, or in `%LOCALAPPDATA%\QuadDeck` if that folder is not writable. For hangs, QuadDeck attempts to record thread information after 4 and 10 seconds and the duration when it recovers. Logs may contain private media paths, so review them before sharing.

## Contributing

QuadDeck is a native Windows C++20 application. Read [the contributor workflow](docs/AI-WORKFLOW.md) and [handoff board](docs/handoffs/BOARD.md) before making changes. They cover worktree isolation, module ownership and the required MSVC / CMake / vcpkg verification steps.

[ARCHITECTURE.md](ARCHITECTURE.md) describes the implementation. The [runtime validation register](docs/RUNTIME-VALIDATION.md) tracks completed and outstanding checks. Please keep project documentation in English.

## Credits

QuadDeck was inspired by [GridPlayer](https://github.com/vzhd1701/gridplayer)'s multi-video layout.

## Licence

QuadDeck is licensed under the [GNU GPLv3](LICENSE).

The SDR algorithm in Smart Vibrance Plus is adapted from aston89's [Smart Vibrance for ReShade](https://github.com/aston89/Smart-vibrance-for-reshade), also licensed under GPLv3. The shader source retains its attribution.

Bundled libraries retain their own licences, including FFmpeg (LGPL-2.1+), libass (ISC), FreeType (FreeType License), HarfBuzz (MIT) and FriBidi (LGPL-2.1). Their licence notices are included in the package's `licenses/` folder.
