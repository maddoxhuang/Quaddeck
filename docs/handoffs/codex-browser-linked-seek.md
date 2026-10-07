# Browser linked seek bars

Agent: Codex
Base: `d025e93`
Branch: `codex/browser-linked-seek`

The user clarified that selecting Linked for F6/Emby +Add videos should leave
positions untouched until a rail is clicked, and that clicking 10:00 in a
20-minute video should move a 40-minute video to its own 10:00. Browser queues,
EOF and repeat remain per-pane; the browser seek choice starts Independent and
is not persisted. A shorter source clamps at its own end, while the linked
target continues for longer sources. An unresolved, failed or unknown-length
video blocks a partial alignment and shows a specific notice.

Tests: synthetic App regression cases cover F5 selection, pane and bottom
rails, arrows beyond the short source, EOF holds, paused panes, queued device
recovery, single Emby and photo exclusion. The prescribed Windows build passed
with 13/13 CTests on this branch. No live video, NAS stream, UI gesture or
Emby server was run for this change; R02 remains open.

Risks: Different playback rates can make panes drift after an instant of
alignment. Per-pane A-B or whole-source repeat can remap the requested second.
Emby streams with unknown duration cannot join this seek mode. Live decode,
audio and progress reporting still need R02 validation.

Next: Exercise the added R02 case with real local and Emby media, then have the
designated integrator review and merge this branch if it behaves as intended.

## 2026-10-03: independent browser bottom timeline

Base: `bee03b6`. A later Emby report described a bottom duration such as
40 minutes when the open videos together were shorter. The code can produce
that display: browser additions keep the old master clock, and `tick()` maps
each new source's end onto it. This matches the symptom but is not a log of
the user's exact incident. The user chose to hide the aggregate bottom time
and seek rail in multi-pane Independent mode. Pane rails and the other bottom
controls remain; Linked restores a rail in source seconds, and a single
browser video keeps its own bottom rail. The window title omits the aggregate
time too. Resolving Linked streams show `0:00 / 0:00` until a source duration
is known. An empty bottom row cannot pass drag gestures to a pane below.

Tests: `powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1`
passed, including 13/13 CTests and packaging. Synthetic App regression covers
the 30-minute-old-clock / 10-minute-video 40-minute case, hidden bar hit
testing, Linked restoration, unresolved streams, one Emby video and a manual
deck. Core tests sweep the no-timeline layout across widths 1–1920 pixels.

Media: Not run. R02 retains live local/Emby playback, gestures, audio, EOF
and progress-report checks. Next: Integrator review, then run R02 with real
media before treating the UI behavior as live-validated.
