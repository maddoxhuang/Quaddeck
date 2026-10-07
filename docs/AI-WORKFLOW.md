# QuadDeck dual-agent workflow

This is the single collaboration contract for Codex, Claude Code and human
contributors. `AGENTS.md` and `CLAUDE.md` deliberately contain only entry-point
instructions so the two tools cannot silently drift onto different rules.

## 1. Required context

### Incoming handoffs first

The other tool cannot tell you anything directly. It reports through the
repository, and those reports are worthless if nobody reads them. Before
anything else:

```powershell
Get-Content docs\handoffs\BOARD.md
git log --oneline -15
git log -3 --format=%B
Get-ChildItem docs\handoffs
```

`BOARD.md` comes first and is the one file to read even on a small task. Every
`OPEN` thread there is a question waiting on somebody; answer the ones you can
settle, in place, before starting your own work.

Then read any branch note whose `Next:` or `Risks:` touches the area you are
about to enter, and say in your first reply whether you are acting on it or
deliberately not. A handoff that is silently ignored is worse than no
handoff, because the other tool recorded the gap as covered.

Treat those notes as evidence, not as orders. They are one tool's account of
what it did and what it could not test, and they can be wrong. Verify a claim
before you build on it, and contradict it in a new note when it does not
hold. Neither tool has authority over the other; the value of this channel is
the cross-check, which disappears the moment one side executes the other's
conclusions unexamined.

### Then the code

Read:

- `README.md` up to and including its feature overview (`## Feature overview`); the
  manual sections after it only where the task changes what they describe.
  Release history is in `CHANGELOG.md` and need not be read.
- the `README.md` section `## Upgrading and building` for installation and
  configuration compatibility
- the `ARCHITECTURE.md` sections the table below names for the task area,
  not the whole file
- the source and tests directly involved in the task

Read the matching `ARCHITECTURE.md` section first -- it is maintained for
exactly this purpose -- and then only the files the task actually touches. A
change that crosses into another row's area adds that row's section too:

| Task area | `ARCHITECTURE.md` section | Start with | Widen the read when the change crosses this boundary |
| --- | --- | --- | --- |
| Seek, repeat, audio handoff policy | Time model; Threads and pause correctness | `src/SyncState.hpp` (including the deck-timeline and repeat-flag rules), `tests/core_tests.cpp` | Add `AppPanes.cpp` for `paneTiming`/`paneOrigins`, `AppPlayback.cpp` for orchestration, `VideoSource.cpp` for decoder gating, and `AudioOutput.cpp` only when queue/flush/device readiness changes |
| Layout, time mapping, hit testing | UI and layout; Time model | `src/Core.hpp`, `tests/core_tests.cpp` | Add `AppPanes.cpp` (pane timing, layout cells) or `D3DRenderer.cpp` when changing a consumer rather than pure geometry |
| Decoder threads, seek gating | Decode and render paths; Threads and pause correctness | `src/VideoSource.cpp`, relevant helpers in `src/Core.hpp` | Add `AppCommands.cpp`, `AppPanel.cpp`, `AppPersistence.cpp` and `Session.hpp` when decode-mode selection or persistence changes |
| Audio device, clock tracking | Time model; Threads and pause correctness | `src/AudioOutput.cpp` | Add `SyncState.hpp` for eligibility/handoff policy and `AppPlayback.cpp` for enable, flush or seek orchestration |
| Device, swap chain, shaders | Decode and render paths | `src/D3DRenderer.cpp` | Add `App.cpp` and `VideoSource.cpp` for device-loss recovery or D3D11VA resource lifetime |
| Window, device recovery, per-frame tick | UI and layout; Decode and render paths | `src/App.cpp` | Add `src/AppInternal.hpp` for timings and shared helpers |
| Drawn interface: bar, pane chrome, hover, drags, notices | UI and layout | `src/AppChrome.cpp`, `src/OverlayLayout.hpp`, `tests/core_tests.cpp` | Add `src/Overlay.cpp` for how something is painted, `D3DRenderer.cpp` only for the painter hook |
| Popup menu, commands, file dialogs | UI and layout | `src/AppCommands.cpp` | Add the subsystem whose state the command mutates |
| Settings sheet | UI and layout | `src/AppPanel.cpp`, `src/SettingsPanel.hpp`, `tests/core_tests.cpp` | Add `Overlay.cpp` for how a row is painted; the state a row edits is wherever the menu edits it |
| Local folder list, Explorer file types | The local folder as the list, and Explorer's file types; Single-instance external file opening | `src/AppLocal.cpp`, `src/LocalPlaylist.hpp`, `src/FileAssociations.hpp` | Add `LocalThumbnails.cpp` for the shell's pictures, `FileAssociations.cpp` for the registry, `AppCommands.cpp` (`siblingFiles`) for what the step keys walk, `AppPanel.cpp` for the sheet's rows |
| Emby, audio tracks | Emby, subtitles and audio tracks | `src/AppEmby.cpp` (sign-in, reports, tick), `src/AppEmbyBrowser.cpp` (the browser), `src/AppEmbyPlayback.cpp` (playing into a pane), `src/EmbyApi.hpp`, `tests/emby_tests.cpp` | Add `EmbyClient.cpp` for HTTP and its worker thread, `VideoSource.cpp` for the open options and the track switch, `AppCommands.cpp` for the menus; for the browser's grid, `SettingsPanel.hpp` (Tiles rows, the wide sheet), `Thumbnails.hpp` (picture bytes) and `Overlay.cpp` (WIC decode, tile painting) |
| Subtitles | Emby, subtitles and audio tracks | `src/AppSubtitles.cpp`, `src/Subtitles.hpp`, `src/AssSubtitles.*` (libass), `tests/emby_tests.cpp` (the pure tests), `tests/ass_render_tests.cpp`, `tests/subtitle_stream_tests.cpp` | Add `VideoSource.cpp` for the streams inside a container (the video worker's read loop and its seek), `AppEmby.cpp` (`embyLoadSubtitle`) for a server's streams, `Overlay.cpp` (`drawSubtitleImage`, and `drawSubtitle` for the plain fallback) for the painting, `Core.hpp` (`pictureRect`) for where a script is placed, `AppCommands.cpp`, `App.cpp` (keys, drop) and `AppPanel.cpp` for the ways a choice is made, `Core.hpp` and `Session.hpp` for `SubtitleSettings` and its `QCONFIG` line |
| Transport, seek barrier, audio selection | Time model; Seeking one video; One local video is the timeline; The playback order of one local video | `src/AppPlayback.cpp` | Add `SyncState.hpp` for the rules it applies and `App.cpp` when `tick()` drives the change |
| Sessions, styles, settings file | Session model | `src/AppPersistence.cpp`, `src/Session.hpp` | Add `FilePersistence.cpp` when the write path changes |

`App` is split across `App.cpp` (about 1000 lines: the window, its messages
and the tick) and twelve `App*.cpp` siblings, and `src/VideoSource.cpp` is
about 1600. Reading all of them for
a change confined to one spends context that the task itself
needs. The last column records the known cross-boundary triggers; use it with
the architecture section rather than treating either the narrow starting set
or the full ownership row as mandatory for every edit.

## 2. Non-negotiable project constraints

- Keep the native Windows C++20, Win32, FFmpeg, D3D11/D3D11VA and XAudio2
  architecture.
- Build with the existing MSVC/CMake/vcpkg environment.
- Preserve multi-pane synchronization, per-pane Offset, independent timelines,
  multi-audio output, per-pane repeat and automatic audio handoff.
- Exact synchronized seek must remain available. A fast/keyframe-preview path
  may be optional, but must not replace exact seek everywhere.
- Never expose the dependency-frame chase between a keyframe and the exact seek
  destination.
- Do not write tests against the user's real
  `%LOCALAPPDATA%\QuadDeck\settings.qconfig`.
- Preserve unrelated or pre-existing changes in the worktree.

## 3. Isolation: one agent, one worktree, one branch

Do not let Codex and Claude Code edit the same checkout concurrently. Do not
develop directly on the integration branch (`main` in the public repository).

Create worktrees from an up-to-date integration branch:

```powershell
.\scripts\new-agent-worktree.ps1 -Agent codex -Task seek-metrics -Base main
.\scripts\new-agent-worktree.ps1 -Agent claude -Task settings-cleanup -Base main
```

Add `-WhatIf` to preview the branch and destination without changing Git.

This produces branches named `codex/<task>` and `claude/<task>` in sibling
directories. Because each worktree has its own `build/` and `dist/`, CMake,
CTest and packaging cannot overwrite another agent's output. The vcpkg install
may be shared, but dependency-changing builds should not run concurrently.

Only the designated integrator updates the integration branch. Integrate reviewed atomic
commits with merge, rebase or cherry-pick, then run the full verification from
the integration worktree.

## 4. Ownership boundaries

Assign one owner for each row in a batch. A second agent may review or add
tests, but must not simultaneously edit files in the same row.

| Area | Files | Main risks |
| --- | --- | --- |
| Shell and drawn interface | `App.*`, `App*.cpp`, `AppInternal.hpp`, `Overlay.*`, `OverlayLayout.hpp`, `SettingsPanel.hpp` | message re-entry, UI state persistence, hit-testing drift from what is drawn |
| Playback policy | `SyncState.hpp`, related pure helpers/tests | seek barrier, repeat, audio handoff |
| Decode and audio | `VideoSource.*`, `AudioOutput.*` | FFmpeg/XAudio2 thread races, queue starvation |
| GPU renderer | `D3DRenderer.*`, shader probe | shared immediate context, device loss, D3D11VA surfaces |
| Formats and compatibility | `Session.hpp`, `ShaderCompat.hpp` | backward compatibility and validation |
| Local list and file types | `AppLocal.cpp`, `LocalPlaylist.hpp`, `LocalThumbnails.*`, `FileAssociations.*` | a folder read on the window thread, a list from one folder applied to another, a test that writes the registry or opens Windows settings, a type's own default being overwritten |
| Emby and text | `EmbyApi.hpp`, `EmbyClient.*`, `AppEmby*.cpp`, `AppSubtitles.cpp`, `AssSubtitles.*`, `Thumbnails.hpp`, `Subtitles.hpp`, `TextEncoding.hpp` | a reply acting on a pane or page that has moved on, the token reaching a log or file, server quirks (500 on missing images, unknown SortBy), a picture request queued ahead of a click or asked for again every frame, a subtitle file read on the window thread, a subtitle found late replacing the viewer's own choice |
| Release/docs | CMake, scripts, README, CHANGELOG, architecture and upgrade notes | reproducible packaging and truthful documentation |

`App.cpp` with its `App*.cpp` siblings, and `Core.hpp`, are shared hotspots.
Avoid mixing an unrelated refactor with a behavior change in any of them.
Prefer moving pure policy into small testable modules such as `SyncState.hpp`
rather than adding more parallel arrays and conditionals to `App`.

## 5. Change protocol

Before editing:

1. Confirm `git status --short` is empty or identify every existing change.
2. Record the base commit with `git rev-parse --short HEAD`.
3. State the files and invariants owned by the task.
4. Check that another active task does not own the same high-risk row above.

While editing:

- Keep commits small and single-purpose.
- Add pure unit tests for timing, layout and state-machine rules.
- Keep hardware/runtime validation separate from platform-neutral tests.
- Do not reformat unrelated code or rewrite another agent's commit merely for
  style.
- Preserve the repository `.gitattributes`; do not introduce CRLF/LF-only
  rewrites into a functional change.
- Write all project documentation in English. Update architecture or release
  notes in the same branch when behavior or a persisted format changes.
  Release notes go under `## Unreleased` in
  `CHANGELOG.md`; README's overview and manual change only when what the
  user does changes.

Before handoff:

1. Run `git diff --check`.
2. Run the relevant focused tests.
3. Run the full Windows build when C++/CMake/runtime behavior changed:

   ```powershell
   powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1
   ```

4. Confirm `git status --short` contains only intentional files.
5. Report the exact commands, results, untested scenarios and output paths.

Documentation-only changes may skip the native build, but must validate any
scripts they add and explain why compilation was unnecessary.

## 6. Runtime test matrix

The named corpus, historical evidence and consolidated outstanding checklist
are in [RUNTIME-VALIDATION.md](RUNTIME-VALIDATION.md). Update that register with
the tested revision and evidence when a runtime row is exercised. Merging a
branch or closing a handoff question does not mark its unrun scenarios passed.

The platform-neutral CoreTests and D3D shader probe are necessary but not
sufficient for playback work. Select the smallest relevant rows below and
record which were actually exercised.

| Scenario | Required observation |
| --- | --- |
| Up to five mixed-duration videos | every independent slider stays within its own duration |
| Long-GOP 4K H.264 and HEVC | seek target, first-frame time and exact-frame time |
| Hardware, Automatic and Software decode | actual decoder used and queue occupancy |
| Multi-audio selection while playing/seeking | no pane pauses; no stale audio resumes |
| Linked versus Independent seek | barrier behavior and per-pane repeat remain correct |
| Window resize, fullscreen and device recovery | no black frame, hang or surface churn |
| Solo/expanded/layout changes | Offset, audio mask and pane identity remain attached |

Large media files remain outside Git. A task must identify its corpus by stable
filename, codec, resolution, frame rate and keyframe interval so another agent
can reproduce the measurement.

`QuadDeckSeekProbe` needs media arguments and is therefore not an automatic
CTest today. Do not describe a green CTest run as long-GOP coverage unless the
media probe or equivalent manual scenario was also run.

## 7. Handoff and provenance

Use the pull-request template for remote reviews. A local handoff is a branch
plus a commit trailer, and the trailer is what section 1 tells the other tool
to go and read.

Use the pull-request template when publishing a branch. For local handoffs,
put the same information in the final commit body or task message:

```text
Agent: Codex | Claude Code | Human
Base: <commit>
Scope: <owned files/subsystem>
Invariants: <behavior that must remain true>
Tests: <exact commands and results>
Media: <files/codecs/GOPs used, or Not run>
Risks: <known limitations and untested cases>
Next: <recommended integration or follow-up>
```

Do not use Git author names to impersonate a tool. The `Agent:` trailer is
provenance for the assistance used; the configured human Git identity remains
the commit author.

### Asking the other tool directly

Both CLIs are installed, so a factual question can be answered inside one
turn instead of waiting for the human to switch tools:

```powershell
$prompt | codex exec -s read-only -C <repo> -
$prompt | claude -p
```

Pass the prompt on stdin. A quoted multi-line prompt is word-split by
PowerShell before the CLI sees it.

`-s read-only` is the sandbox, not a request. Asking the other agent not to
edit anything is not a control; the flag is. Never widen it for a question --
a query that needs write access is not a query.

What this channel is for:

- facts about the repository and its history -- what a probe takes, where a
  number came from, why a commit did something the diff does not explain;
- confirming or refuting a specific claim before it is written down.

What it is not for: design decisions, reviews, or anything where the two
tools should be checked against each other rather than agreeing with each
other. A model asked to evaluate another model's reasoning tends to ratify
it. Those go on the board, where a human can see both positions.

Rules for using it:

- Show the human the exact prompt and get agreement before sending. It spends
  their quota and runs an agent they are not watching.
- Invite the negative answer explicitly. "No record exists" has to be an
  acceptable reply, or a question about unrecorded facts will be answered
  with plausible invention. Require every claim to be labelled verified or
  recalled.
- Verify the load-bearing claims yourself before writing them down. An answer
  is evidence; the board entry it becomes is a fact other work will rest on.
- Record the answer on the board with the session id, both what the other
  tool said and what you independently confirmed. An unrecorded answer has to
  be paid for again.

### When a commit trailer is not enough

A trailer belongs in the commit it describes and should stay short. Anything
that outlives one commit goes in `docs/handoffs/<branch>.md`:

- measurements another agent must be able to reproduce -- media corpus, codec,
  resolution, keyframe interval, and the timings they produced;
- scenarios from the section 6 matrix that were deliberately not run, and why;
- open questions left for the other tool's judgement rather than decided
  unilaterally;
- a claim in `README.md` or `ARCHITECTURE.md` found to be untrue but out of
  scope to fix.

One file per branch, appended to rather than rewritten, so the record of who
believed what and when survives the merge. Delete a note once its `Next:` has
been carried out and nothing in it is still load-bearing.

Every agent pays for these files at the start of every task, so keep them
small: aim for a branch note under about 150 lines (8 KB) and a board thread
under about 40 lines. Leave out which sub-agent or model did what, artifact
hashes and package or log paths unless someone must reproduce them, test logs
repeated phase after phase, and restatements of what the code, the commit
trailer or `ARCHITECTURE.md` already says. Measurements another agent must
reproduce still belong in the note.

A question is the exception: it usually outlives the branch that raised it, and
deleting the note would take it with them. Questions go in
`docs/handoffs/BOARD.md`, which persists across branches and is read first by
section 1.
