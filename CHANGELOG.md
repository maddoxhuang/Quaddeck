# QuadDeck changelog

Public releases start at 1.0.0. See [README.md](README.md) for current usage and [ARCHITECTURE.md](ARCHITECTURE.md) for implementation and file-format details.

## 1.7.0 (2026-11-26)

This release shows HDR10 and HLG video as HDR on an HDR display and
tone-maps it on an SDR one, shows the picture subtitles of Blu-ray and DVD
rips, plays interlaced video field by field, and resumes after a pause
without decoding every video again. Emby videos open again on Emby
4.11.0.18, and an HTTPS server's certificate is now checked for the video
stream too. Settings remain `QCONFIG 26`, styles `QSTYLE 4` and video sets
`QSETS 1`, so 1.6.0 and 1.7.0 read each other's files.

### HDR video

- On a display with Windows HDR turned on, switch the output to 10-bit PQ
  while an HDR10 or HLG video is on screen, on any graphics card and
  without RTX Video HDR. HDR10 reaches the display unchanged; SDR videos
  beside it are converted at the SDR white level Windows sets for that
  display. The output returns to SDR about 2 seconds after the last HDR
  picture leaves, so playing the next episode does not switch back and
  forth.
- On an SDR display, with Windows HDR off or with the bitblt renderer,
  tone-map HDR10 and HLG to SDR. Brightness follows the BT.2390 curve from
  the video's peak down to SDR white (203 nits): shadows and midtones keep
  their level and highlights roll off instead of clipping. Colours are
  converted from BT.2020 to BT.709. The peak is the video's MaxCLL, else its
  mastering display's, else 1000 nits. Tone mapping comes before the
  shaders, Vibrance+ and the colour controls, so they work on an ordinary
  SDR picture.
- Treat HLG (ARIB STD-B67) as HDR instead of a grey SDR picture. On an HDR
  display it is shown as on a 1000-nit display (BT.2100, system gamma 1.2);
  on an SDR display it is tone-mapped with a 1000-nit peak. RTX Video HDR no
  longer applies to HLG video.
- Decode software-decoded HDR with the BT.2020 matrix (it used BT.709, which
  shifted hues) and keep its 10 bits: 10-bit and deeper HDR in 4:2:0, 4:2:2
  or 4:4:4 now reaches the graphics card's video processor as P010, like
  hardware decoding, instead of banding in dark gradients. 4:2:2 and 4:4:4
  chroma is averaged to 4:2:0. The WARP renderer has no video processor and
  stays 8-bit.
- Hand the video's HDR10 mastering display, MaxCLL and MaxFALL to Windows
  while the output is PQ (the brightest video's when several are open), and
  withdraw them after the last HDR picture. Whether Windows passes them on to
  the display for a window cannot be checked from QuadDeck.
- Decode full-range PQ and HLG video and top-left chroma (common on UHD
  Blu-ray) correctly with hardware decoding, and recognise HDR that is
  marked only in the MKV or MP4 container.
- Find an HDR display attached to another graphics card than the one
  rendering, as on a laptop whose screen is on the integrated graphics.
- Make Grayscale and Invert work on linear brightness under PQ output.
  Invert used to turn black into 10,000-nit white; it now inverts within
  the display's SDR white. Sharpen and custom shaders still work on PQ
  values.
- Warn `Dolby Vision profile 5 is not supported: the colours will be wrong`
  when a Dolby Vision video has no HDR10, HLG or SDR base layer. Its colours
  are still wrong. Profiles 7 and 8 play their base layer as before.
- Under PQ output the colour controls and output levels do not apply, as with
  RTX Video HDR before. The status line on `F5 → Video` says
  `Output: 10-bit PQ (HDR).` and where HDR video goes: `HDR video is shown
  as HDR.`, `HDR video is tone-mapped to SDR.`, or, on a card whose video
  processor cannot convert it, `HDR video is shown as SDR with approximate
  colours: this card does not convert it.`
- Not included: decoding Dolby Vision profile 5, HDR10+ dynamic metadata,
  tone mapping to the display's own peak under PQ output, and an exact
  conversion of BT.2020 SDR video.

### Picture

- Play interlaced video (TV recordings, DVDs) field by field: each field is
  shown for half a frame, so 50i moves 50 times a second instead of 25 and
  pans and scrolling text no longer judder. An interlaced frame now costs
  the graphics card four conversions instead of one, in its own pane only.
  Progressive video, and deinterlacing set to Off, are unchanged.
- Fix SDR video looking flat and grey on a Windows HDR display with RTX
  Super Resolution and 10-bit output: NVIDIA's driver wrote that output as
  PQ. A pane that Super Resolution enlarges now uses 8-bit output, and
  `QuadDeck.log` says `RTX Super Resolution on the 10-bit SDR output` once;
  other panes stay 10-bit.
- Stand an upright phone video up in the graphics card's video processor
  where it can rotate, so NVIDIA RTX Video's status indicator in the corner
  is upright too. Other pictures are still turned as they are drawn, with
  the same result. After the video processor, a custom shader sees the
  upright picture. The log says `Video processor: turns a picture itself`,
  or `does not turn a picture; the pane's draw does`.

### Subtitles

- Show the picture subtitles inside a video: Blu-ray's PGS, DVD's VobSub
  and DVB, in local files and Emby videos. Choose them from the subtitle
  menu or with `Alt+L`. They are drawn where the source placed them and
  scale with the window and zoom.
- Automatic choice is unchanged: text subtitles (embedded, files beside the
  video and Emby's) are chosen by language first, and picture subtitles only
  when no text subtitle fits. An Emby account's own default subtitle is
  followed when it is a picture subtitle.
- Picture subtitles ignore the subtitle font, size, position and box, and do
  not move up for the bar. They are decoded only while chosen, so one chosen
  during playback appears with its next line. After a seek, an MKV shows the
  line already under way; other containers, such as m2ts, show the next.
- When the subtitle canvas has the video's shape (a 1080p canvas over 1080p
  or 4K video, a DVD), it follows the picture. Otherwise, as with a 16:9
  canvas over a film stored as 1920x800, it is scaled to the picture's
  height and centred, as mpv does.
- Not shown: separate `.sup` and `.idx` files, and picture subtitles stored
  beside the file on an Emby server, which stay greyed as
  `(pictures, not shown)`. The `Sub` badge still counts text subtitles only.
- Read GBK, Big5, Shift-JIS and Korean subtitle files whose names carry no
  language on a Windows set to a Western language. Without a suffix such as
  `.chs` or `.cht`, the code pages of the preferred subtitle languages are
  tried in turn and the first that reads the whole file is used, then the
  system's. Western and Cyrillic subtitles are not misread. On a Chinese,
  Japanese or Korean Windows the system code page is still tried first.

### Playback

- Continue from the frame on screen when Play follows Pause, instead of
  seeking every video again, which with four 4K videos took a second or
  more. Only the videos you hear pick their sound up again. A full seek is
  still made when a video was paused on its own, has ended, has not started,
  is still opening or failed, when a photo is open, or when the position
  was changed while paused.
- Retry a network or share read that fails, as when a server restarts, a
  connection drops, Wi-Fi switches or a NAS goes offline: after 0.25 s,
  doubling to at most 4 s, for up to 30 s. Playback continues from the frame
  it stopped at, or from the current position if the other videos have moved
  on, and the sound from the sample it stopped at. Each retry is logged.
  Errors that a retry cannot fix, such as a damaged file or HTTP 401, 403 or
  404, are reported at once. A cached file on a NAS share whose handle was
  lost still does not recover.
- Keep the display on and the computer awake while a video plays. When the
  window is minimised only sleep is prevented. Paused, ended, closed, or with
  only photos open, Windows behaves as it normally does.
- Write the settings file on a background thread, so a slow disk no longer
  freezes the picture for seconds after a setting changes, most noticeably
  when switching Vibrance+. Several changes in a row write only the latest,
  a failed write is tried once more after 1.2 s, and closing still writes
  the final settings before QuadDeck exits.

### Audio

- Take the output latency off at the playback speed: at 2x only half of it
  was taken off before.
- On an output where the effects cannot run (more than eight channels, or a
  device that refuses them), apply volume above 100% to the output itself
  instead of ignoring it. There is no limiter of QuadDeck's after it, so
  very loud passages may distort. The Audio tab and the log say that the
  effects do not run on that output.
- Free the queued sound when the audio engine stops on an error, such as an
  unplugged device.

### Emby

- Fix `Emby: HTTP 500` for every video on Emby 4.11.0.18. The `PlaybackInfo`
  request now says what the player is: it plays any container and codec
  directly, needs nothing transcoded, and reads every subtitle format
  itself, so the server still names its default subtitle.
- Check the server's certificate for HTTPS video, photo and audio streams,
  including the connections made by seeking and reconnecting; browsing and
  signing in already did. A self-signed, expired or untrusted certificate
  now stops a video from opening, and a revocation list that cannot be
  fetched may too. `http://` servers are unaffected.
- Drop a deletion whose confirmation would open while the graphics device is
  lost; it used to leave the player disabled with no window to answer.

### Video sets and dialogs

- Reuse the check of the video sets list made within the last minute when
  every video was found. A missing file, an unknown item or a server that
  did not answer is checked again each time. A file moved within that
  minute shows as missing once the minute has passed.
- Remember which sets were tried by the set, not by its place, so moving or
  deleting sets no longer skips an untried set or tries a failed one again.
- Cut a set name longer than 120 characters without leaving half an emoji,
  which made the list impossible to save from then on.
- Accept paths longer than 260 characters in the dialogs that open videos,
  subtitles, shaders and presets, save presets, and import and export video
  sets. Past 260 characters these dialogs closed as if cancelled.

### Settings files

- No format changed: settings remain `QCONFIG 26`, styles `QSTYLE 4` and
  video sets `QSETS 1`.

## 1.6.0 (2026-11-17)

This release steps through the videos one at a time with `Tab`, gives a
video shown alone its own Fit / Fill / Stretch and zoom, adds an **Info
pages** switch to a person's page, and shows a video recorded upright on a
phone the right way up. Settings are now `QCONFIG 26` and styles `QSTYLE 4`,
which 1.5.0 and earlier cannot read, so back up `settings.qconfig` before
trying this version. Video sets remain `QSETS 1`.

### Several videos

- Press `Tab` to show the next video alone, in the order they are on screen:
  from all videos to V1, then V2, V3 and so on, and from the last back to
  V1. `Shift+Tab` goes the other way, starting from the last video. Stepping
  never returns to all videos; use `Esc` or `Shift+0` for that. Holding
  `Tab` does not run through the videos, it does nothing with one video, and
  in a text box it still belongs to the box.
- Find both keys on `F5 → Keys` under **Arrangement**, after **All videos**,
  as **Solo the next video** and **Solo the previous video**; change, clear
  or reset them there. If you had already given `Tab` or `Shift+Tab` to
  another action, it stays with that action.
- Keep Fit / Fill / Stretch and zoom separately for a video shown alone and
  for videos shown together. A video is alone when it is the only one open
  or when one of several is in Solo; whichever video that is, it uses the
  same single setting. Videos shown together still have one each. Neither
  changes the other: choose Fill alone and Fit together, and alone is still
  Fill when you come back.
- **View / zoom** in the right-click menu, and **All videos** and **Zoom
  all** on `F5 → Picture`, show and change the setting in use on screen;
  with no video open they change the one for videos shown together.
- After upgrading, a video shown alone starts with the first pane's earlier
  setting, so a single open video looks as it did. `.qstyle` files carry
  the new setting as well.

### Emby

- Use the new **Info pages** button on a person's page. Lit, which is the
  default, a title opens its information page first where its library shows
  them, as before. Turned off, a movie on any person's page plays at once
  and a series lists its seasons.
- The switch belongs to people's pages only. It does not change any
  library's own **Info pages**, so titles in a library still open their
  information pages and those still lead to a person. Favorites and
  searches across all libraries have no such button and follow each
  library's setting.

### Picture

- Show a video recorded upright on a phone the right way up. Such a file
  stores a sideways picture with a rotation mark, which QuadDeck did not
  read, so the video lay on its side. Rotations of 90°, 180° and 270° are
  now followed for local files and Emby videos alike.
- Work out the arrangement, Fit / Fill / Stretch, zoom and subtitle position
  from the upright picture, so an upright phone video is simply a tall one.
- A video whose mark also mirrors the picture is still shown as stored.
  There is no command to rotate a video by hand and no switch to turn the
  automatic rotation off.
- `QuadDeck.log` adds `turned=90` (or 180, 270) to the `Decoder setup` line
  of such a video: the clockwise angle that stands the picture upright.

### Settings files

- `QCONFIG 24` allows the two new keys on `shortcut` lines, `QCONFIG 25`
  carries `QSTYLE 4` with its new `alone` line, and `QCONFIG 26` adds the
  person page's switch to the `embybrowser` line. Files from 1.5.0 and
  earlier are read as before and keep their values.
- An earlier version that finds a `QCONFIG 26` file leaves it untouched and
  runs with its defaults.

## 1.5.0 (2026-11-10)

This release replaces saved sessions with a list of video sets, can delete a
video from the Emby server once you turn that on, loads Emby lists in full,
keeps the list beside the picture on the video you step to, and joins each
video's Linked controls into one capsule. Settings are now `QCONFIG 23`,
which 1.4.0 and earlier cannot read, so back up `settings.qconfig` before
trying this version. Styles remain `QSTYLE 3`; sessions saved by earlier
versions can no longer be opened.

### Video sets

- Replace saving and opening sessions with a list of video sets kept inside
  QuadDeck. `Ctrl+S` adds the videos playing together to the list without a
  dialog. `Ctrl+O`, the new video sets button left of full screen on the
  bar, or **Video sets** in the right-click menu opens the list on the
  right, and a set's **Play** plays the whole set.
- Keep only the videos and how they are shown: one to five local files and
  Emby items, mixed as you like, by their places, with the arrangement and
  the enlarged pane. Positions, offsets, speed and repeat are not kept; each
  video starts from its beginning and other settings stay as they are.
- While a set from the list plays, `Page Up` / `Page Down`, the media
  previous / next track keys and the bar's buttons step to the previous or
  next set as a whole, for example from four videos straight to the next
  set's three. Replacing a video by hand or adding one to an empty pane
  returns to stepping files.
- Choose what follows a set's end from the drop-down above the list: **Stop
  at its end**, **Repeat the set**, **Next, then stop** (the default),
  **Repeat the list** or **Shuffle**. Each set's **⋯** menu moves it up or
  down, renames or deletes it.
- Save the list automatically as `videosets.qsets` beside `settings.qconfig`.
  **Export…** writes it to a `.qdeck` file; **Import…**, double-clicking or
  dropping a `.qdeck` file adds its sets, skipping those already listed.
  Explorer calls `.qdeck` files **QuadDeck video sets** once **Register…**
  has been used again. A session saved by an earlier version is refused
  with a notice.
- Check every video when the list opens and say, in orange-red, when a local
  file is missing, Emby is signed out or signed in to another server, or the
  server no longer has the item or does not answer. An Emby item no longer
  found under its id is searched for by name, and a single match is kept
  under its new id.
- Skip a set whole when one of its videos cannot play, rather than playing
  it with a pane missing, and say which set and video were skipped and why.
  Previous skips backwards, every set is tried at most once, and when none
  can play a set chosen by hand leaves the picture as it was.

### Emby

- Delete a movie, episode or video from the Emby server after turning on
  **Delete videos from the Emby server** at the bottom of `F5 → General`;
  it is off by default. **Delete from server…** then follows the favorites
  item when you right-click a list row, poster, thumbnail or information
  page, and **Delete from Emby server...** follows it in a playing Emby
  video's menu. Series, seasons, folders, collections, playlists and
  libraries cannot be deleted, since that would take everything in them.
- Never delete a favorite. The server is asked again before the
  confirmation and again before deleting; an episode is kept when its series
  or season is a favorite, or when the server does not say.
- Confirm in a small window of its own, centred on the player, with the
  title and type, the video that will close, every file the server says it
  will delete and a warning that this cannot be undone. Only clicking
  **Delete** deletes, never a key; `Esc`, the close button, `Alt+F4` and
  **Cancel** cancel. Videos keep playing while it is open. Once the server
  confirms, the item leaves the lists, **Continue watching**, the information
  page and every video's queue.
- Load the rest of a library, folder, series or favorites page in the
  background after its first 300 items, 1,000 at a time and up to 20,000
  (2,000 for a person's titles), without scrolling to the end. **Shuffle**,
  **Play in order** and **Repeat the list** now reach the whole list instead
  of the items loaded so far. Random order no longer repeats items after a
  manual **Refresh**. Opening a library of several thousand videos now takes
  a few dozen requests.
- Leave **Posters** out of home video and photo libraries, their folders and
  searches in them; where Posters was chosen they show **Thumbnails**.

### Lists and the playing video

- Mark the folders a playing video is in, at every level, in that video's
  colour, with a scrollbar mark that scrolls to the folder. In Emby the
  video's folder, season and series are marked at once and the folders and
  library above them once the server has said; collections and playlists
  are not marked. With several videos in one folder it takes the colour of
  the first.
- Keep a docked F6 list or Emby browser on the video you step to with
  `Page Up` / `Page Down`, the bar's or media previous / next buttons or the
  right-click menu: its row, or the row of its folder, season or series,
  scrolls to the middle of the list. A step to the next video at the end of
  one follows only while the row that was playing is in view, and scrolling
  or clicking the list stops the following.

### Controls and windows

- Join each video's buttons into one capsule from the left in Linked mode:
  tag, pause and time, then sound, subtitles, repeat, Solo and close. The
  time keeps its width so the buttons do not move while it counts. The
  volume slider stands upright above the sound button in the video's colour
  and stays within its picture. The row to the right of the capsule is
  picture: click, double-click and drag work there. The same single capsule
  is used in Independent mode when a pane is too short for its seek bar.
- Double-click the picture of the only video to pause or resume it, as
  `Space` does. With two or more videos a double-click still enters and
  leaves Solo.
- Stop a drag on a single picture from moving the window; drag the title
  strip instead. In a window the title strip now stays while the pointer
  rests on it, with the pointer shown. Buttons covered by another window no
  longer light, keep the bars visible or show their tips.
- Move **Decoder** from the Playback tab to `F5 → Video`, under **Graphics
  card**. It is dimmed, and every video decodes in software, on the WARP
  renderer or when QuadDeck falls back to WARP, where choosing Hardware used
  to stop videos opening.
- Show the explanation of a dimmed settings row when the pointer rests on
  it, and set choices that wrap below their label at the right of the row.

### Text boxes

- Right-click the Emby search box, the sign-in boxes, a video set's name
  box or a number box in the settings window for **Cut**, **Copy**,
  **Paste**, **Delete** and **Select all**, as in Windows' own boxes. The
  password box never offers Cut or Copy.
- Drop the line breaks at either end of pasted text: a password copied with
  a trailing line break used to fail to sign in. Pasting with nothing to
  paste no longer clears the selection, and a character such as an emoji is
  pasted whole or not at all.

Deleting from the Emby server has been tried by hand on a test server. The
other new controls have been built and packaged but not yet exercised by
hand in the running player; live Emby lists, input methods and real media
remain unverified.

## 1.4.0 (2026-10-28)

This release moves settings into a window of their own, lets you choose
keyboard shortcuts, adds audio output, sound and video output settings, and
fixes crashes and stalls on damaged or unusual media. Settings are now
`QCONFIG 22`, which 1.3.0 and earlier cannot read, so back up
`settings.qconfig` before trying this version. Sessions and styles remain
`QDECK 6` and `QSTYLE 3`.

### Settings and keys

- Open settings in a window of their own with `F5` or **Settings...**,
  instead of a sheet over the right of the picture. Its tabs form a column
  down the left with an icon each; the chosen tab's rows scroll beside them.
  The window belongs to the player: it stays above it, minimises and closes
  with it and has no taskbar button. It first opens landscape in the middle
  of the player; drag its caption or the tab's title to move it and its
  edges to resize it, and it returns where it was left for the rest of the
  run. `F5`, `Esc`, its close button or `Alt+F4` put it away. Playback keys
  still reach the player while it has the keyboard.
- Keep the right-hand sheet for the Emby browser and the F6 folder list
  only; its **‹ Settings** switch is gone. Signing out of Emby, or a sign-in
  that expires, closes the browser's sheet.
- Group each tab's rows on rounded cards, as Windows 11 Settings does, with
  headings above the cards and hints under the rows they explain. Size
  choices and buttons to their text and set them at the right of their row;
  a choice that does not fit wraps into an even grid below its name.
- Draw the settings window, browser, menus, drop-downs, sync panel, bottom
  bar and control strips in one palette: the same buttons, chosen segments,
  40 × 20 switches, boxes, dividers, scrollbars and corners. The settings
  window has the player's caption strip, with a square close button.
- Choose keyboard shortcuts on the new **Keys** tab: up to two keys for each
  action, grouped by Playback, Volume and audio, Files and lists, Subtitles,
  Arrangement, Picture colour and Window. Click a key box and press the new
  key, with `Ctrl`, `Alt` or `Shift` if wanted; **Clear this key**, **Default
  keys** and **Reset all keys to the defaults** undo changes. A key does one
  thing, so taking it from another action says which one lost it. Windows'
  own combinations, lone modifiers, lock, input-method and media keys cannot
  be assigned. Menus and tooltips show the current keys. `Ctrl` is now part
  of a key: `Ctrl+Space`, `Ctrl+M` and similar no longer act as the key
  without it.
- Add default keys for the six arrangements (`Alt+1`–`Alt+6`), for sorting
  by release date, last played and last updated (`Ctrl+1`, `Ctrl+2`,
  `Ctrl+3`), and for showing one video alone (`Shift+1`–`Shift+5`) or all
  videos again (`Shift+0`). A key you had already given to another action
  stays with it.
- Turn off the input method outside the Emby search box and the server and
  user-name fields, so letter shortcuts work while a Chinese or other input
  method is active, and the Keys tab can take letter keys.

### Multi-video controls

- Add a sync button beside the arrangement button with two or more videos.
  Its panel switches **Linked** / **Independent** seek bars and has a row for
  each video: offset steps of 0.1 s and 1 s, the current offset, a repeat
  switch, and the mouse wheel for 0.1 s steps; **Reset all offsets** (`0`) is
  at the bottom. Videos keep playing while it is open.
- Leave the per-video seek bars out in Linked mode and seek with the bottom
  bar, which marks where each shorter video ends in its colour. A Linked
  strip is two rounded capsules, and the "All · linked" label is gone.
- Keep each F6 or Emby video's offset in Linked mode: seeking, the arrow
  keys and Stop place every video at the shared time plus its own offset,
  and a negative offset still waits. Changing one video's offset moves only
  that video. Repeat Linked F6 and Emby videos together: those that end
  first hold their last frame until the last one ends, then all restart with
  their offsets.
- Switch straight from one video shown alone to another with
  `Shift+1`–`Shift+5`, or with the new **All videos** and **V1**…**V5**
  buttons in the **All** capsule and the Linked bar; press the same key, or
  **All videos**, to show every video again.
- Show each video's place in its list in the window title, for example
  `Name  (3/48)`, and the number of videos on each Emby browser page.

### Audio

- Choose the audio output under **Audio → Output**, `F5 → Audio` or
  `Ctrl+Shift+A`. **Windows default** follows Windows' choice; an unplugged
  device falls back to the default with a notice and is used again when it
  returns. Every video's sound continues at the picture's position, and the
  audio engine restarts by itself after an error.
- Add **Speakers** (Stereo or every speaker of the device), a **Volume can
  reach** limit from 100% to 200%, a sound delay for every video and for each
  one (`Shift+,` / `Shift+.` / `Shift+/`, 50 ms steps), **Keep pitch at other
  speeds** (on by default), and a **Preferred audio language**.
- Add effects for the whole mix: a 10-band equalizer with preamp, 18 presets
  and vertical faders; loudness normalisation; night mode; swapped, mono,
  left-only or right-only channels and voice removal; voice emphasis;
  headphone crossfeed and stereo widening. `Alt+A` steps the audio track, and
  `Shift+E`, `Shift+N`, `Shift+D`, `Shift+T`, `Shift+V` and `Shift+A` switch
  the effects.
- Rebuild the Audio tab around drop-down lists that open below their box,
  as Windows combo boxes do, and a spin box for the volume limit.
- Follow audio timestamps by resampling slightly instead of cutting or
  repeating samples, which removes the clicks some MKV and MP4 files had.
  Learn files whose timestamps run at another rate, ignore a single stray
  timestamp, catch up a lasting shift slowly, and fill a real gap with
  silence. The log records each correction.
- Replace the limiter with one that looks 5 ms ahead and lowers the gain
  smoothly, so loud passages above 100% volume no longer clip.
- Rebuild the resampler when a track changes channel layout, sample format
  or rate, such as a broadcast recording switching between 5.1 and 2.0;
  switching from 5.1 to 2.0 used to crash.
- Stop the equalizer and loudness normalisation from releasing a stored
  thump or click when a band or the normaliser is switched back on.
- Tick a newly chosen audio track at once in the menu of a video that is not
  being heard.

### Video output

- Add a **Video** tab and a **Video** submenu: the renderer (Direct3D 11,
  Direct3D 11 with bitblt for older capture tools, or WARP on the CPU), the
  graphics card, the display used for full screen, 8-bit or 10-bit output,
  deinterlacing, the scaler for enlarged pictures (Bilinear, Bicubic, Lanczos
  or Nearest neighbour), PC or TV levels, and brightness, contrast,
  saturation, hue and gamma. The colour keys follow PotPlayer: `W` / `E`,
  `Shift+R` / `T`, `Y` / `Shift+U`, `I` / `Shift+O`, and `Q` to reset.
- Open on WARP when no graphics card can make a device, as in a virtual
  machine, with only the Microsoft Basic Display Adapter or while a driver
  is being installed, and say so; a device without video support decodes in
  software instead of failing.
- Show anamorphic video at its display aspect ratio, including 16:9 DVDs
  and 1440 × 1080 HDV, following the container's aspect first and then each
  frame's.
- Skip a damaged video or audio packet instead of stopping the video, and
  continue after a damaged stretch when opening, resuming or seeking into
  it. The log summarises the decode errors passed over.
- Draw PotPlayer and MPC shaders whose `main()` reads only `TEXCOORD0` (or
  another coordinate) correctly instead of as one flat colour; a shader the
  vertex stage cannot feed is refused with a message naming what it reads.

### Emby and browsing

- Make the browsers' filters toolbar buttons that switch at once:
  **Unplayed**, **All videos** and **Info pages**, plus **♥ Favorites**,
  which lists the current page's favorites, including those in subfolders.
  The F6 list's view is a **Thumbnails** button.
- Search the current library, folder, series or season when searching from
  it, with **In *folder*** / **Every library** under the search box.
- Draw thumbnails of non-square-pixel videos, such as portrait phone videos,
  in their real shape.
- List a local folder that contains a broken link, skipping the link.
- Keep the viewer's audio track, subtitle and delays when videos reopen
  after a decoder, renderer or graphics-card change.
- Do not overwrite `settings.qconfig` with defaults when the window could
  not be created at start.

The new controls have been built and packaged but not yet exercised by hand
in the running player. Real-media playback, live Emby, input methods,
physical audio devices, HDR and display-transition acceptance remain
unverified.

## 1.3.0 (2026-10-20)

This release organises multi-video controls around each video, draws every
menu inside the window and extends the F6 folder list. Settings are now
`QCONFIG 15`, which 1.2.0 and earlier cannot read, so back up
`settings.qconfig` before trying this version. Sessions and styles remain
`QDECK 6` and `QSTYLE 3`.

### Multi-video controls

- Give each video a control strip along its bottom edge: its seek bar, a
  coloured tag that opens its menu, pause and time on the left, and volume,
  sound, subtitles, repeat, Solo and close on the right. Tags show on every
  pane while the pointer moves; the rest of the strip shows over the hovered
  pane and stays up while in use. Narrow panes drop controls in a fixed order,
  and subtitles rise above the strip.
- Give each video its own subtitle switch: on its strip, in its rows of the
  Subtitles settings page, and `Alt+H` for the video last under the pointer.
  The switches are saved and follow videos when panes are swapped; an older
  settings file's single switch applies to every video. The bottom bar's
  subtitle button remains only for a single video.
- Replace the bottom bar with a centred **All** capsule (play, stop, master
  volume, arrangement and fullscreen) when several videos have no shared
  timeline. It stays centred beside a docked browser, appears when the pointer
  reaches it, and lifts the subtitles of the panes beneath it.
- Colour each video by position (V1 blue, V2 orange, V3 green, V4 purple, V5
  pink) across its tag, seek and volume bars, lit buttons, outlines, menus,
  settings rows and its playing marks in the F6 and Emby lists. A single video
  keeps the accent blue.

### Menus

- Draw the right-click, subtitle and arrangement menus and the browser's
  drop-downs inside the window, with submenus, keyboard navigation and
  scrolling in short windows. A browser opened from the right edge no longer
  closes while the pointer is over a menu, the system menu or an input-method
  candidate list.
- Remove the Ascending / Descending row from the Sort menus; choosing the
  checked order again reverses it, as its shortcut does.

### Browsing and opening

- Let the F6 list open subfolders and go back with **↑ Up**, leaving out
  hidden and system folders, and list **All videos** under a folder in one
  list (up to 5,000 videos and 16 levels), sharing the Emby browser's
  Folders / All videos choice. Previous / next follow the expanded list.
- Mark playing videos on the F6 and Emby list scrollbars; clicking a mark
  scrolls that video to the middle of the list. An Emby list keeps loading
  pages until a video played from that list appears.
- Add an **Open file** button to the empty window and **Open file...** to its
  right-click menu. Videos opened into an empty window with `O` now start
  playing together from the beginning.
- Keep fades running in an empty window, where a closed settings sheet or
  Emby browser stayed drawn. Every fade runs on one clock that showing or
  hiding the controls no longer restarts.

### Keyboard

- Change the master volume by 5% with `↑` / `↓`, unmuting on `↑`. Step videos
  with the media previous / next track keys as with `Page Up` / `Page Down`,
  which now also work for a single video the pointer has not visited.
- Pass application-command keys to Windows' default handling, so media and
  volume keys keep working while QuadDeck is in front.

The new controls, menus and keys have not yet been exercised by hand in the
running player. Real-media playback, live Emby, input methods, physical media
keys, NAS interruption, physical audio, HDR and display-transition acceptance
remain unverified.

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
