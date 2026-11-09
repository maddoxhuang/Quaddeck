<p align="center">
  <img src="assets/QuadDeck.png" alt="QuadDeck icon" width="112" height="112">
</p>

<h1 align="center">QuadDeck</h1>

<p align="center">
  <strong>Five videos. One window. Your timeline.</strong><br>
  A native Windows player with synchronised or independent playback,<br>
  hardware acceleration and styled subtitles.<br>
  Also an Emby player that feels like a local one: the original file plays directly,<br>
  without server transcoding, with the same decoding, ASS subtitles, shortcuts and sound settings.
</p>

<p align="center">
  <a href="https://github.com/maddoxhuang/Quaddeck/actions/workflows/windows-build.yml"><img src="https://github.com/maddoxhuang/Quaddeck/actions/workflows/windows-build.yml/badge.svg?branch=main" alt="Windows Build status"></a>
  <a href="CHANGELOG.md#150-2026-11-10"><img src="https://img.shields.io/badge/version-1.5.0-0891b2" alt="Version 1.5.0"></a>
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
| **Audio per pane** | Mix audio from several videos, with separate volume, mute, track and delay controls, an output device of your choice and an equalizer. |
| **Local files and Emby** | Browse folders or your Emby library and give each pane its own playback queue. Emby videos play the original file directly, without server transcoding, and report playback progress back to the server, so resume points and Continue watching stay in step. |
| **Styled subtitles** | Display ASS / SSA subtitles with their styling and attached fonts, or load SRT / WebVTT files. |
| **GPU decoding and shaders** | Use hardware decoding, adjust Vibrance+ and enable RTX Video features on supported systems. |

**New in v1.5.0:** a list of video sets in place of sessions, deleting a video from the Emby server (off by default), Emby lists that load in full, a list that follows the video you step to and marks the folders it is in, one control capsule per video in Linked mode, and a right-click menu in text boxes. See the [changelog](CHANGELOG.md#150-2026-11-10) for all changes.

<details>
<summary><strong>More features</strong></summary>

- **Playback controls:** shared and independent timelines, per-video offsets, playback speed, repeat and A-B loops. Exact synchronised seeking is used when starting or restarting a group.
- **Layouts:** six arrangements adapt to the videos you open. Focus on one pane with Solo, choose a larger main pane, and adjust each video's zoom or Fit / Fill / Stretch mode.
- **Audio:** play audio from several panes at once, switch tracks and adjust each pane's level and delay. Choose the output device, stereo or every speaker, a volume limit up to 200%, a preferred audio language and effects: a 10-band equalizer, loudness normalisation, night mode, channel options, voice emphasis, crossfeed and widening. Sound keeps its pitch at other speeds. When the selected audio source ends, QuadDeck can switch to the next pane automatically.
- **Folder browsing:** press `F6` for thumbnails, duration and file size. Open subfolders and go back with **↑ Up**, or choose **All videos** to list every video under the folder and its subfolders in one list. Sort by name, size, date or duration, or shuffle the list. Click an item to replace the current pane; use `+Add` to open it muted in an empty pane. Each pane keeps its own queue. The folders a playing video is in are marked in its colour, and the list stays on the video you step to.
- **Emby:** press `Ctrl+E` to browse libraries, folders, series, playlists, favorites and search results. Search across libraries, or within the folder you are in, with `Ctrl+F`, choose list, poster or thumbnail views, and open movie details or season and episode lists. Lists load in full without scrolling. Local and Emby videos can play side by side, with separate queues and progress reporting. Optionally, delete a video from the server after a confirmation; favorites are never deleted.
- **Subtitles:** choose embedded or external text subtitles, with automatic language selection, and show or hide them for each video separately. ASS / SSA rendering uses libass and supports styling, animation, karaoke and fonts attached to the video. A blue `Sub` badge identifies videos with available text subtitles.
- **Network playback:** a background buffer reads ahead on Windows shares and mapped network drives to help absorb short interruptions. Local files do not use this cache.
- **Video output:** choose the renderer, graphics card, full-screen display, 10-bit output, deinterlacing, the scaler for enlarged pictures, output levels and colour controls in `F5 → Video`.
- **NVIDIA RTX Video:** enable Super Resolution or RTX Video HDR in `F5 → Picture` on supported GPUs and drivers. Super Resolution applies when upscaling; RTX HDR also requires Windows HDR to be enabled for the display. Eligible software-decoded SDR video can use these features too.
- **Shaders:** choose from Normal, Sharpen, Grayscale, Invert and Smart Vibrance Plus, or load a compatible custom shader. Vibrance+ has separate processing for SDR and RTX HDR output.
- **Video sets and presets:** `Ctrl+S` adds the videos playing together to a list of video sets; `Ctrl+O` opens the list, where **Play** plays a set whole and previous / next step from set to set. Export and import sets as `.qdeck` files, and save a visual preset as `.qstyle`. Player settings and the list are saved automatically.
- **File associations:** register QuadDeck in `F5 → General → Register…`, then select it in Windows' Default apps. Opening a subtitle file attaches it to the matching video when possible, or to the current pane.
- **Single instance:** files opened from Explorer go to the existing QuadDeck window. New videos use empty panes; if all five panes are occupied, you choose which one to replace.

</details>

## Get started

1. **Download QuadDeck.** Get `QuadDeck-<version>-win-x64.zip` from the [latest release](https://github.com/maddoxhuang/Quaddeck/releases/latest); for this version it is [QuadDeck-1.5.0-win-x64.zip](https://github.com/maddoxhuang/Quaddeck/releases/download/v1.5.0/QuadDeck-1.5.0-win-x64.zip). You can also [build from source](#build-from-source).
2. **Extract and run.** Extract the ZIP, then open `QuadDeck\QuadDeck.exe`. Keep the accompanying files together. QuadDeck requires Windows 10/11 x64 and the [Microsoft Visual C++ v14 x64 runtime](https://aka.ms/vc14/vc_redist.x64.exe).
3. **Open some videos.** Press `O` or drop files into the window. Use `F6` to browse local files, `Ctrl+E` for Emby and `F5` for settings.

See the [1.5.0 release notes](CHANGELOG.md#150-2026-11-10) and [settings and compatibility](#settings-and-compatibility) before replacing an existing installation. The latest published download may be older than this source version; check its release number.

For a development build, sign in to GitHub and download **QuadDeck-win-x64** from a successful [Windows Build](https://github.com/maddoxhuang/Quaddeck/actions/workflows/windows-build.yml) run for `main`. Extract the downloaded artifact, then the ZIP inside it.

## The bottom bar and settings

Move the pointer near the bottom of the window to reveal the playback controls. The bar includes play / pause, stop, seeking, volume, the video sets button and fullscreen, plus the subtitle button with one video and arrangement controls with several. Open settings with `F5` or **Settings...** in the right-click menu; the bar has no menu or settings button. With one video open, or while a video set plays, it also offers previous and next buttons. Press `U` to keep the controls visible or let them hide automatically.

When several videos have no shared timeline (Independent seeking, or several videos opened from F6 or Emby), the bar shrinks to an **All** capsule at the bottom centre: play / pause for all videos, stop and master volume, the **All videos** and **V1**…**V5** buttons, then sync, arrangement and fullscreen. It appears when the pointer reaches it or the top or bottom edge of the window, floats above the videos' own control strips, and stays centred when a browser is docked on the right unless the window is too narrow. In a narrow window it drops the label, arrangement, stop, volume and fullscreen in that order; play always remains.

Left-click a subtitle button to show or hide that video's subtitles; it lights while subtitles are shown and a track is selected. Right-click it to select a track, load a file or adjust timing. Turning subtitles back on keeps the selected track; if none is selected, QuadDeck tries the preferred language. Selecting a track also shows it. With several videos, each video's subtitle button is on its own control strip, and `F5 → Subtitles` has a switch in each video's rows.

The right-click menu, the subtitle and arrangement menus and the browser's drop-down menus are drawn inside the window. Hover over or click a submenu to open it, use the arrow keys, `Enter` and `Esc` from the keyboard, and click outside a menu to close it. Videos keep playing while a menu is open.

Press `F5` to open settings in a window of their own. It stays above the player and closes with it; drag its caption to move it and its edges to resize it, and press `F5` or `Esc` again, or click its close button, to put it away. Playback keys still reach the player while the settings window has the keyboard. Per-pane controls list only loaded videos, and Picture shows **Mixed** when their view or zoom differs:

| Tab | Settings |
| :--- | :--- |
| **Playback** | Linked or independent seeking, playback order, offsets and repeat |
| **Audio** | Master volume and its limit, output device and speakers, per-video volume, track and delay, preferred language, effects and equalizer |
| **Subtitles** | Subtitle appearance, timing and position |
| **Picture** | Layout, zoom, shaders, Vibrance+ and RTX Video |
| **Video** | Renderer, graphics card, decoder, full-screen display, output depth, deinterlacing, scaler, levels and colour |
| **General** | Control bar, network cache, file associations, presets, video sets and deleting from the Emby server |
| **Keys** | The keyboard shortcuts, up to two keys for each action |

On the **Keys** tab, click a key box and press the new key, with `Ctrl`, `Alt` or `Shift` if wanted. A key does one thing: taking it from another action says which one lost it. **Clear this key**, **Default keys** and **Reset all keys to the defaults** undo changes. Windows' own combinations such as `Alt+F4`, lone modifiers, lock keys, input-method keys and media keys cannot be assigned.

The settings window remembers the last tab and returns where it was left for the rest of the run. The Emby browser and the F6 list keep the sheet on the right of the picture.

Move the pointer to the top edge to reveal the title and window buttons. Drag the title area to move the window, or double-click it to maximise or restore. In a window the title strip stays while the pointer rests on it. Dragging a single picture, or a video in Solo, does not move the window; with several videos it swaps panes.

The title strip is also available in full screen: move the pointer to the top edge, then use Restore to leave full screen or Minimise to send the window to the taskbar. Dragging the title strip in full screen does not move the window.

With only one video, there is no control strip on the picture: use the right-click menu for Repeat and Close, and the bottom bar for sound, pause, volume and subtitles. Repeat follows the single video's playback order. Separate audio and pause buttons appear on the picture's lower edge when needed to undo that pane's mute or pause. With several videos, the selected pane's outline and audio borders fade with the hover controls.

With no video loaded, **Open file** in the centre opens the file dialog, like `O`, and the chosen videos start playing together from the beginning; **Open file...** in the right-click menu does the same. While signed in to Emby, **Open Emby** beside it opens the browser.

In a narrow window, some controls are hidden to leave room for the seek bar. They remain available through shortcuts, settings or the right-click menu.

## Emby sign-in, browsing and favorites

When signed out, `Ctrl+E` opens **Server**, **User name** and **Password** fields inside the sheet. Use `Tab` / `Shift+Tab` to move between them and `Enter` or **Sign in** to connect. The password is masked, cannot be copied or cut, and uses direct keyboard input. A failed sign-in keeps the form open and highlights the field to correct; success clears the password.

Movie and series information pages show **Cast & Crew** portraits. Select a person to see their biography and titles across your libraries, then use **Back** to return to the information page. Play and Add use compact split buttons; expand **More details** for technical information.

Browse using the View and Sort menus and the filter buttons on the toolbar: **Unplayed**, **♥ Favorites** (the current page's favorites, including those in subfolders), **All videos** and, in movie and series libraries, **Info pages**. A lit button is on. Choose the checked sort order again to reverse it. F6 local folders switch between list and thumbnails with the **Thumbnails** button; Emby also offers Posters, except in home video and photo libraries. Emby lists load in full in the background after their first 300 items, so shuffle and repeat reach the whole list. Failed lists have a separate error and **Retry** action. Adding a sixth video opens a compact replacement list with thumbnails; Cancel preserves the current videos.

In the F6 list, subfolders appear before videos; click one to open it and use **↑ Up** to return, up to the top of a drive or network share. Change **Folders** to **All videos** to list the videos in the folder and all its subfolders together (up to 5,000), each marked with the subfolder it is in. This choice is shared with the Emby browser's **All videos** button.

The list marks every playing video, and the folders it is in at every level, in its pane's colour, with a mark on the scrollbar; click a mark to scroll that row to the middle of the list. While the list is docked beside the picture, stepping to the previous or next video scrolls the new one to the middle; scrolling or clicking the list stops this until the next step.

Press `Ctrl+E` to open Emby. Choose **Search**, or press `Ctrl+F` from any browser page, to type in the search box. Searching starts after a **450 ms** pause or when you press `Enter`; clearing the box clears the results. Searching from a library, folder, series or season searches only what is inside it; choose **Every library** under the box to search everything, or **In** *folder* to return. Results are grouped into Movies, Series, Episodes, other Videos and Photos, with counts. A failed library is reported alongside any results that were retrieved; use **Refresh** to retry.

The box supports Chinese and other input methods, text selection, double-click to select all, `Shift` + arrows, `Ctrl` + arrows or `Backspace` for words, `Ctrl+A/C/X/V`, and a right-click menu with **Cut**, **Copy**, **Paste**, **Delete** and **Select all**, as do the sign-in boxes and the settings window's number boxes. Line breaks at either end of pasted text are dropped. Search waits for an input-method composition to finish. While the box has focus, typing and arrow keys edit text; `F5` and `F6` remain available. `Esc` leaves the box, and a second `Esc` closes the panel. Opening a series from the results and choosing **Back** restores the search text and results.

Choose **Favorites** to see the current account's favorites across libraries. Pink hearts mark favorite items. Use the **Favorite** button on an information page, right-click an item, or use **Add to Emby favorites** / **Remove from Emby favorites** in a playing pane's menu. The heart changes after the server confirms the update; removing an item from the favorites page removes it from that list on the next refresh. These actions change the favorites stored on your Emby account. `Page Up` / `Page Down` follow the favorites list when you start playback from it.

To delete videos from the server, turn on **Delete videos from the Emby server** in `F5 → General`; it is off by default. Then right-click a movie, episode or video in the browser and choose **Delete from server…**, or choose **Delete from Emby server...** in a playing Emby video's menu. A window lists every file the server will delete; only clicking **Delete** goes ahead. Favorites, and episodes of a favorite series or season, are never deleted, and series, seasons, folders, collections, playlists and libraries cannot be. Deleting removes the files from the Emby server's disk and cannot be undone in QuadDeck.

## Video sets

Press `Ctrl+S` to add the videos playing together to the video sets, with their places, arrangement and enlarged pane; local files and Emby items can be mixed. `Ctrl+O`, the video sets button on the bar or **Video sets** in the right-click menu opens the list beside the picture. **Play** opens a set's videos from their beginning, keeping your other settings. While a set plays, previous and next step to the set before or after it, and the drop-down above the list chooses what follows its end: stop, repeat the set, the next set, repeat the list or shuffle. A set with a video that cannot play is skipped whole, with the reason in the list. Each set's **⋯** menu moves, renames or deletes it, and **Export…** / **Import…** exchange sets as `.qdeck` files.

The browser's navigation and compact toolbar stay at the top while the list scrolls. In very short windows, the whole page scrolls to leave enough room for the content.

## Independent seek bars and synchronisation

With several videos, each pane has a control strip along its lower edge: in Independent mode its seek bar across the pane, and below it the pane's coloured tag (`V1`, `V2`, …; click it for that pane's menu), pause and elapsed / total time on the left, with volume, sound, subtitles, repeat, Solo and close on the right. Pointing at a pane's sound button unfolds its volume slider to the left. In Linked mode, or when a pane is too short for its seek bar, the strip is one capsule from the left with the same buttons in the same order, and the volume slider stands upright above the sound button; the rest of that row is picture. The tags show on every pane while the pointer moves; the rest of the strip appears on the pane under the pointer, hides after about 1.4 seconds of inactivity, and stays visible while you drag, hold a button or have its menu open. In a narrow pane, Solo, repeat, total time, elapsed time, close, subtitles, sound and pause are hidden in that order and remain in the pane's menu; the tag always stays. That pane's subtitles move up while its strip is shown.

Videos can have different lengths. Click or drag a pane's bar to choose a position; dragging applies the seek when you release the mouse. The hint above the bar shows the target time and whether the seek affects all videos (`All linked`) or only that one (`V2 only`). A single video uses the bottom bar. In Linked mode the strips have no seek bar of their own and stand above the bottom bar, whose shared seek bar marks where each shorter video ends in its colour; all multi-video Independent groups hide the shared time and seek rail and use the centred **All** capsule instead, with the strips along the panes' own lower edges.

With two or more videos, the sync button beside the arrangement button opens the sync panel: **Linked** / **Independent** seek bars, each video's offset in steps of 0.1 s and 1 s (or the mouse wheel over its row), its repeat switch, and **Reset all offsets**. The same choices are in `F5 → Playback`:

- **Linked:** seek through the group together, keeping each video's timing offset.
- **Independent:** seek through one video without moving the others. The arrow keys affect the Solo pane, or the pane most recently under the pointer. Each pane can repeat independently.

Seeking normally shows a nearby keyframe first, then finishes moving to the exact position in the background. For a single video, seeking stops at the keyframe by default; you can change this in Playback settings.

The previous picture stays visible until the new position's first frame is ready. A pane held at the end of an F6 or Emby video resumes when you seek back into it; a pane you paused yourself stays paused. A manual video's Repeat control is dimmed when the current mix of manual and browser videos prevents that repeat setting from taking effect.

### Videos added from F6 or Emby

These videos start in Independent mode and keep separate queues, resume positions and end-of-file settings. With several independent panes open, the shared seek bar and total time are hidden. Each pane's seek bar remains available, and play, stop and volume for all videos are in the centred **All** capsule.

Selecting Linked does not move the videos immediately. Your next seek aligns them to the same time in each file, plus each video's offset. For example, seeking to **10:00** in a 20-minute video also moves a 40-minute video to **10:00**, or to **10:05** if its offset is +5 s. Repeat in Linked mode restarts them together: a video that ends first holds its last frame until the last one ends. The shared seek bar returns in Linked mode and uses the longest video's duration; before the first linked seek, it shows the selected pane's position.

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
- With one video, double-click it to pause or resume. With several, double-click a video to enter Solo, and double-click again or press `Esc` to return. `Shift+1`–`Shift+5` show V1–V5 alone, switching straight from one to another, and `Shift+0` shows every video again; the **All videos** and **V1**…**V5** buttons on the bar do the same.
- With several videos, each has its own colour by position: V1 blue, V2 orange, V3 green, V4 purple and V5 pink. Its tag, seek and volume bars, lit buttons, outlines, menus, settings rows and its marks in the F6 and Emby lists use that colour. Closing a video or changing the layout renumbers the videos and their colours.
- Adjust each pane's zoom and choose Fit, Fill or Stretch to control how the picture fills its space.

## Decoding and pixel shaders

Choose a decoder in `F5 → Video`:

- **Auto:** use D3D11VA hardware decoding when available, with software decoding as a fallback.
- **Hardware:** require D3D11VA. If decoding fails, the error appears in the window title and log.
- **Software:** use the CPU to decode video. This can help diagnose compatibility problems.

Changing the decoder reopens the loaded videos at their current positions. On the WARP renderer, or when QuadDeck has fallen back to WARP, the decoder is dimmed and every video decodes in software.

The built-in shaders are Normal, Sharpen, Grayscale, Invert and Smart Vibrance Plus. Adjust Vibrance+ in `F5 → Picture`; changes take effect immediately and are saved automatically. Its defaults are Intensity **1.5**, Saturation pivot **0.5**, Gray pivot **0.003** and Gray sharpness **45**. ReShade does not need to be installed.

The four Vibrance+ sliders appear only while Vibrance+ is active and keep their values when another shader is selected. If a saved custom shader is temporarily unavailable, QuadDeck uses the preset and shows **Remembered but not loaded** in Picture settings. Select the file again once it is available; its saved path is retained until you choose another shader.

Use **Shader...** to load a compatible PotPlayer-style `.txt` shader or a `ps_4_0` `.hlsl` shader with a `main` entry point. For example:

```hlsl
Texture2D videoTexture : register(t0);
SamplerState videoSampler : register(s0);

float4 main(float4 position : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
    return videoTexture.Sample(videoSampler, uv);
}
```

Try [example-warm.hlsl](shaders/example-warm.hlsl) for a simple colour adjustment. Custom shaders process the displayed image and can be used with hardware decoding.

## Video output

`F5 → Video` and the **Video** submenu of the right-click menu choose how the picture reaches the screen:

- **Renderer:** Direct3D 11 (the default), Direct3D 11 with bitblt presentation for capture tools that need it, or WARP, which renders on the CPU and decodes in software. When no graphics card can create a device, QuadDeck opens on WARP and says so.
- **Graphics card and full-screen display:** Automatic or any card Windows lists, and the display full screen uses. A card that is not connected falls back to the default one.
- **Output:** 8-bit or 10-bit, deinterlacing (Automatic or Off), the scaler used when a picture is enlarged (Bilinear, Bicubic, Lanczos or Nearest neighbour), and PC or TV levels.
- **Colour:** brightness, contrast, saturation, hue and gamma, each with a slider; double-click a slider to reset it.

## Keyboard shortcuts

| Action | Shortcut |
| :--- | :--- |
| Add a video (in an empty window the videos start from the beginning) | `O` |
| Search Emby from any browser page | `Ctrl+F` |
| Previous / next file in the current pane, or set while a video set plays | `Page Up` / `Page Down`, or the media previous / next track keys |
| Show / hide the local folder or Emby list | `F6` |
| Open the Emby library | `Ctrl+E` |
| Sort by name / size / date / duration / random | `Ctrl+4` / `Ctrl+6` / `Ctrl+7` / `Ctrl+8` / `Ctrl+9` |
| Sort by release date / last played / last updated | `Ctrl+1` / `Ctrl+2` / `Ctrl+3` |
| Show the video sets / add the videos playing as a set | `Ctrl+O` / `Ctrl+S` |
| Play / pause | `Space` or the media `Play/Pause` key |
| Adjust volume | `↑` / `↓` or the mouse wheel, 5% per step |
| Reset all offsets | `0` |
| Seek 5 seconds back / forward | `←` / `→` |
| Seek 30 seconds back / forward | `Ctrl+←` / `Ctrl+→` |
| Return to the start | `Home` or `R` |
| Toggle a pane's audio output | `1` / `2` / `3` / `4` / `5` |
| Mute | `M` |
| Next audio track | `Alt+A` |
| Sound 50 ms earlier / later; back in step | `Shift+,` / `Shift+.`; `Shift+/` |
| Equalizer / loudness normalisation / night mode | `Shift+E` / `Shift+N` / `Shift+D` |
| Swap channels / voice removal / voice emphasis | `Shift+T` / `Shift+V` / `Shift+A` |
| Next audio output | `Ctrl+Shift+A` |
| Set the master loop's A / B points | `[` / `]` |
| Toggle the master A-B loop | `\` or `L` |
| Show / hide subtitles of the video last under the pointer | `Alt+H` |
| Next subtitle track; turn off after the last track | `Alt+L` |
| Load a subtitle file | `Alt+O`, or drop it onto the video |
| Show subtitles 0.5 seconds earlier / later | `.` / `,` |
| Reset subtitle timing | `/` |
| Increase / decrease subtitle size | `Alt+PgUp` / `Alt+PgDn` |
| Raise / lower bottom subtitles | `Alt+↑` / `Alt+↓` |
| Reset subtitle position | `Alt+Home` |
| Choose an arrangement: Automatic, One row, Two rows, One column, Portrait focus, Landscape focus | `Alt+1` – `Alt+6` |
| Show V1–V5 alone; all videos again | `Shift+1` – `Shift+5`; `Shift+0` |
| Brightness / contrast / saturation / hue down and up | `W` `E` / `Shift+R` `T` / `Y` `Shift+U` / `I` `Shift+O` |
| Reset the colour controls | `Q` |
| Auto-hide / pin the controls | `U` |
| Open settings | `F5` |
| Full screen | `Alt+Enter` |
| Leave Solo / full screen | `Esc` |

These are the default keys; change them on the `F5 → Keys` tab. Holding a toggle key, such as Space or M, acts once. Seek and volume arrows and subtitle timing, size and position adjustment keys repeat while held. In Emby search and sign-in fields, text-editing shortcuts take priority as described above.

## Upgrading and building

### Portable package

Extract `QuadDeck-<version>-win-x64.zip` and run `QuadDeck\QuadDeck.exe`. Keep the EXE, DLLs, `shaders` and `licenses` folders together. To update, close QuadDeck normally and extract the new package into a separate folder so files from different builds do not get mixed together.

### Build from source

You need Windows 10/11, Git, CMake and Visual Studio 2022 Build Tools with the **Desktop development with C++** workload, MSVC v143 and a Windows SDK.

Open PowerShell in the source directory and run:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1
.\dist\QuadDeck\QuadDeck.exe
```

The script sets up or reuses `%USERPROFILE%\vcpkg`, builds the application and creates `dist\QuadDeck-<version>-win-x64.zip`.

Keep the `build` directory when updating the source to reuse installed dependencies. If you need a clean build, run:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1 -Clean
```

Local builds keep `QuadDeck.pdb` beside the EXE for diagnostic logs and preserve the previous `QuadDeck.log` and `QuadDeck.1.log` when refreshing `dist\QuadDeck`. Logs and PDB files are excluded from the ZIP. Packaging stages a new directory first and stops if the existing package cannot be moved aside, preserving the previous build.

### Settings and compatibility

- Close QuadDeck before replacing a build or switching to another copy.
- Settings are stored in `%LOCALAPPDATA%\QuadDeck\settings.qconfig`, and the video sets in `videosets.qsets` beside it. Copies running under the same Windows account share these files. Back them up, along with any `.qstyle` presets, before upgrading.
- Version 1.5.0 uses `QCONFIG 23`, `QSETS 1` and `QSTYLE 3`; version 1.4.0 uses `QCONFIG 22` and `QDECK 6` sessions, version 1.3.0 `QCONFIG 15`, and versions 1.2.0, 1.1.0 and 1.0.0 `QCONFIG 14`. Each version reads earlier settings, but older applications cannot read settings saved by a newer one, so back up `settings.qconfig` before trying 1.5.0. Settings from 1.4.0 keep their values. Version 1.5.0 no longer opens `.qdeck` sessions saved by earlier versions; its `.qdeck` files are exported video sets. Settings from 1.3.0 keep their values in 1.4.0 and later; the audio, video and key settings added in 1.4.0 start from their defaults. When importing older settings, the single subtitle switch applies to every video and, from files older than `QCONFIG 14`, subtitle height returns to the default position; other valid settings are retained.
- Versions 1.1.0 and later protect unreadable settings: a newer `QCONFIG` version is left untouched and changes are not saved during that run. Other invalid files are copied to `settings.qconfig.unreadable` before defaults are saved; if the copy fails, nothing is saved. Version 1.0.0 predates this protection, so keep a backup before switching builds.
- Video sets refer to video paths and Emby items, and visual presets refer to custom shader paths. They do not include the files themselves, so keep those files in place or select them again after moving them.
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

When [reporting a problem](https://github.com/maddoxhuang/Quaddeck/issues), include the window title, steps to reproduce it and any relevant log entries. `QuadDeck.log` is normally beside the EXE, or in `%LOCALAPPDATA%\QuadDeck` if that folder is not writable. Its startup record identifies the build and matching PDB. Once the log passes 4 MiB it moves to `QuadDeck.1.log`, replacing the previous rotated log; if moving it fails, the current log is cleared instead. For hangs, QuadDeck attempts to record thread stacks after 4 and 10 seconds, writes module offsets before resolving function names, and records the duration when it recovers. Logs may contain private media paths, so review both files before sharing.

## Contributing

QuadDeck is a native Windows C++20 application built with MSVC, CMake and vcpkg. See [Build from source](#build-from-source) for setup and packaging instructions.

[ARCHITECTURE.md](ARCHITECTURE.md) describes the implementation. Please keep project documentation in English.

## Credits

QuadDeck was inspired by [GridPlayer](https://github.com/vzhd1701/gridplayer)'s multi-video layout.

## Licence

QuadDeck is licensed under the [GNU GPLv3](LICENSE).

The SDR algorithm in Smart Vibrance Plus is adapted from aston89's [Smart Vibrance for ReShade](https://github.com/aston89/Smart-vibrance-for-reshade), also licensed under GPLv3. The shader source retains its attribution.

Bundled libraries retain their own licences, including FFmpeg (LGPL-2.1+), libass (ISC), FreeType (FreeType License), HarfBuzz (MIT) and FriBidi (LGPL-2.1). Their licence notices are included in the package's `licenses/` folder.
