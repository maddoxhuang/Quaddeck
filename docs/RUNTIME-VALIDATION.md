# Runtime validation register

Maintainer: Codex / Claude Code / the integrating human
Consolidated by: Codex, 2026-09-19
Historical source snapshot: `5924cf9` (maintainer's pre-release repository)

This is the shared checklist and named corpus for section 6 of
[AI-WORKFLOW.md](AI-WORKFLOW.md). Consolidation is documentation work, not a
new test run. Historical commit IDs, branch names, board thread numbers and
`git show` examples below refer to the maintainer's pre-release repository,
which is not part of the public Git history. They are retained as provenance,
not as commands that work in a fresh public clone. The public question board
starts separately; no unchecked runtime row is closed by that reset.

## Evidence and how to update it

Historical results below were reported in committed handoffs or commit
trailers. They have not been rerun on `5924cf9`. Their exact source documents
remain recoverable from that private snapshot, even after branch deletion:

```powershell
git show 5924cf9:docs/handoffs/codex-nas-read-ahead.md
git show 5924cf9:docs/handoffs/codex-review-fixes.md
```

The same command works for every filename in the retirement table below.
Old build/log paths in those documents belong to old worktrees; do not assume
the files still exist. In particular, the September 4 probe/smoke logs were
explicitly removed after the results were recorded. No old timing or package
hash identifies the current executable.

To complete a checklist row, append the tested commit and EXE hash, date,
Windows/GPU/driver and audio device, corpus IDs and actual decoder, exact
command or manual steps, result and log location. Include queue occupancy,
target/first/exact/barrier timings where relevant. For SMB runs also record
the actual share, network path, bitrate and induced outage/recovery conditions.
Keep FAILED, NOT RUN and blocked-by-missing-fixture distinct from PASS.
Keep measurements with different revisions/modes separate; do not average
unrelated one-shot runs into a performance claim.

Use disposable fixtures and an isolated test profile for settings/shutdown
checks. Never test writes against the user's real
`%LOCALAPPDATA%\QuadDeck\settings.qconfig`. Keep large media outside Git and
private diagnostic paths out of release packages.

## Named corpus

### HEVC-01: local 4K recording

Path: `C:\Users\<user>\Videos\NVIDIA\Desktop\Desktop 2026.09.02 - 23.56.46.01.mp4`

| Field | Recorded value |
| --- | --- |
| Codec / dimensions | HEVC / 3840x2160 |
| Pixel format / colour (ffprobe 9.0.1, Claude Code, 2026-09-26) | yuv420p10le, bt2020nc, smpte2084 (PQ), bt2020 primaries: an HDR10 recording. Until the RTX Video branch the player showed it through the 8-bit 709 approximation; it is not an SDR sample. |
| Average frame rate | 694410000/11656733, about 59.572 fps |
| Duration / size | 777.116 s / 6,180,662,850 bytes |
| Maximum observed packet-key gap | 3.484 s, from 154.616 to 158.100 s |
| Stress target / exact-only target | 158.000 s / 163.000 s |
| Expected target interval / final PTS | [157.999, 158.016) / 777.098 s |

On 2026-09-19 Codex verified that this path still exists and its byte length
matches. Codec, timing and gap values above are historical probe results,
not freshly measured metadata; no content hash was recorded. Re-inspect the
file before treating it as unchanged benchmark input. Packet-key flags do not
guarantee clean IDR boundaries, and an average near 60 fps does not establish
CFR. This sample is neither a 10-second-GOP sample nor an SMB measurement.

Still needed: named 4K H.264 and substantially longer-GOP inputs; known CFR
24/25/30/60/120 fps and VFR files; missing-duration/sparse/discontinuous or
damaged timestamps; video-only and audio/padding extending at least one second
past video; short/missing/broken audio and AAC/Opus encoder-delay cases; five
independent mixed-duration/aspect sources; a real NAS path. Register exact
metadata and fixture provenance before claiming coverage. Generated fixtures
must be labelled synthetic, separately from real recordings.

## Reproducible command entry points

Build in the checkout under test with the required repository command:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1
```

The seek probe takes one video per invocation. The following argument shape
was checked against `tests/seek_probe.cpp` at `5924cf9`:

```text
QuadDeckSeekProbe <video> [automatic|hardware|software] [target-seconds] [timeout-seconds] [cache|direct]
```

For HEVC-01, these are the September 15 commands (examples to rerun, not
commands executed during consolidation):

```powershell
$media = 'C:\Users\<user>\Videos\NVIDIA\Desktop\Desktop 2026.09.02 - 23.56.46.01.mp4'
.\build\windows\Release\QuadDeckSeekProbe.exe $media automatic 158 60 cache
.\build\windows\Release\QuadDeckSeekProbe.exe $media hardware 158 60 cache
.\build\windows\Release\QuadDeckSeekProbe.exe $media software 158 60 cache
.\build\windows\Release\QuadDeckSeekProbe.exe $media automatic 158 60 direct
```

`cache` forces the Windows cache/AVIO path even on a local file; `direct`
disables it.

The Emby probe needs a reachable server and is not a CTest either:

```text
QuadDeckEmbyProbe <server-url> <user> [password] [--save <emby.qauth path>]
```

It signs in through the player's own EmbyClient, lists the libraries, and
with `--save` writes the sign-in file the player reads at start (the token
DPAPI-protected), so `QuadDeck.exe emby://<serverId>/<itemId>` then plays
that item without the dialog. Without `--save` it logs the session out. Omitting that argument uses automatic network detection. Use a
registered real network path for SMB tests; the probe's metadata inspection
scans the full file. CTest does not automatically invoke this media probe.

With the player's saved sign-in the probe only reads (GET requests, no
sign-out):

```text
QuadDeckEmbyProbe --auth <emby.qauth path> --subtitles
QuadDeckEmbyProbe --auth <emby.qauth path> --subtitle-streams <item id> [--names] [--save-subtitles <folder>]
```

The first prints the account's subtitle preference and counts every video's
subtitle streams by codec and by whether they are inside the file. The
second lists one item's streams, asks `GET /Items/{id}/PlaybackInfo` which
subtitle the server would choose, and fetches each text stream as SRT, and
as ASS where that is its own format, reporting bytes, cues, cues placed at
the top, and the time taken.

## Historical evidence retained

| Record | What was reported | Boundary of that evidence |
| --- | --- | --- |
| September 4, `d2438c3` async-open baseline | Release build, 3/3 CTests and packaging; HEVC-01 probe passed Automatic, Hardware and Software | Two-copy app smoke only; no five-file playback/listening test |
| September 9, `3a2822d` review integration (runtime changes through `65738ac`) | Release build, 7/7 CTests and packaging; HEVC-01 preview, exact-only and EOF passed all three modes | Audio PCM captured before XAudio2; hidden-window UI and synthetic colour surfaces do not prove audible/visible user behavior |
| September 15, `71481f9` NAS cache | Release build, 8/8 CTests and packaging; three forced-cache modes plus direct Automatic passed HEVC-01 | Local forced-cache runs and simulated outages, not physical NAS throughput/disconnects |
| September 19, `e10c9d6` and `f5400e2` Claude fixes | Writer-held temp-file bypass and direct FFmpeg read; competing cache-reader regression fails with old cancellation; 20 fixed-code runs passed | No live recorder or NAS run; idle wakeup behavior has no dedicated automated test |
| September 19, `8991db8` split/integration | Full build, 8/8 CTests and packaging reported after combining fixes and App split | No new media validation; line-multiset comparison is evidence of a mechanical move, not full runtime equivalence |

Earlier Core tests cover interval/tick arithmetic, EOF fallback, mappings,
seek generations, duplicate trackbar commits, focus validity and narrow
geometry. Persistence tests cover UTF-8/BOM/16 MiB bounds, round trips, failed
serialization, locked replacement and artifact cleanup. Those focused results
remain valid historical evidence; the corresponding real UI rows remain open.

Review regressions added real FLAC/WAV audio-worker checks (48 kHz and
44.1-to-48 kHz resampling, sample-exact trim, EOF drain, backpressure and stale
generations). Targets 1.000 and 1.080 s passed; a 10 ms WAV seek at output
sample 470 retained the last 10 samples through EOF drain. Timestamp recovery
was checked for overlapping submissions. Shader tests compiled 32 legacy and
translated inputs; the old converter failed 18 translations. Thirty sets of
eight colour patches gave maximum channel error 2/1/2 for software/NV12/P010
(allowed 3); the old renderer returned 16 for limited-range black instead of
0. This covers SDR BT.601/709 on the tested adapter, not HDR/BT.2020 conversion.

Cache regressions used a controllable offline byte source: after filling,
32 KiB (eight simulated seconds at 4 KiB/s) remained readable within 200 ms.
Five caches shared a 256 KiB test budget and released reservations on close.
AVIO size/read/seek/short-tail/EOF, cancellation, retry, independent cursors,
forced-cache FLAC seeks and QCONFIG 5-to-6 migration were reported passing.
These do not establish eight seconds of real video playback during an outage.

All values in the next table are milliseconds, one observation per mode and
revision. First/exact refers to the preview-enabled 158 s seek, followed by
exact-only at 163 s and endpoint. They are not comparative benchmarks.

| Historical record / input path | Mode | First / exact | Exact-only | EOF | Close | Final queue |
| --- | --- | --- | --- | --- | --- | --- |
| September 4 / local | Automatic | 76.8 / 107.8 | 93.0 | 93.1 | not recorded | not recorded |
| September 4 / local | Hardware | 60.6 / 90.9 | 61.9 | 63.1 | not recorded | not recorded |
| September 4 / local | Software | 153.1 / 325.4 | 342.7 | 393.7 | not recorded | not recorded |
| September 9 / local | Automatic | 156.2 / 219.0 | 61.9 | 77.0 | not recorded | not instrumented |
| September 9 / local | Hardware | 64.2 / 158.6 | 46.8 | 62.8 | not recorded | not instrumented |
| September 9 / local | Software | 233.1 / 438.9 | 370.0 | 528.7 | not recorded | not instrumented |
| September 15 / forced cache | Automatic | 108.3 / 153.5 | 46.5 | 62.2 | 31.6 | 0/4 |
| September 15 / forced cache | Hardware | 61.6 / 92.5 | 61.5 | 61.8 | 20.1 | 0/4 |
| September 15 / forced cache | Software | 123.8 / 340.6 | 392.0 | 515.4 | 54.2 | 0/16 |
| September 15 / direct | Automatic | 94.7 / 126.1 | 46.4 | 62.2 | 19.4 | 0/4 |

Diagnostics reported D3D11VA for Automatic/Hardware and CPU for Software.
September 15 cached runs reported 256 MiB resident, zero retries, enabled=1;
direct reported zero cache allocations and enabled=0. The recorded UDTA
metadata retry was non-fatal. The September 4 app smoke loaded two copies of
HEVC-01, advanced its title to 0:03 and 0:08, remained responsive at four and
nine seconds, and completed its initial barrier in 129 ms. It checked the
user settings hash/write time stayed unchanged; it did not listen to output.
The historical 1.6-3.8 s four-source barrier figure has no retained corpus or
log and must not become a benchmark for HEVC-01.

## 2026-09-26: HEVC-01 seek probe, partial R06 / R07 evidence

Measured by Codex on `codex/seek-probe-hevc01`, from the unchanged source
revision `5aec10cadc3f737329f330e55d0aba00eda2ef0c`. The documentation commit
containing this section is not the revision used to build the probe.

- Worktree: `C:\Users\<user>\Documents\QuadDeck\build\agent-worktrees\QuadDeck-codex-seek-probe-hevc01`.
- Executable: `build\windows\Release\QuadDeckSeekProbe.exe`.
- EXE SHA256, unchanged before/after all nine calls:
  `E4649FB68B7EDC412FE93254D97D45F59BEEA9AC8D0212E9DAC62FCBDBC8FB83`.
- Measurement date/time: 2026-09-26, approximately 15:00:45-15:01:18 NZST
  (UTC+12); exact per-call timestamps are in the `.run.json` files.
- Windows 11 Pro 25H2, x64, build `10.0.26200.9550`.
- GPU inventory: NVIDIA GeForce RTX 4060 Ti, driver `32.0.16.1664`; Intel Arc
  Graphics, driver `32.0.101.8991`; both driver dates 2026-08-24. The probe
  creates the default hardware D3D11 device but does not report its adapter
  name. These results must not be attributed specifically to either GPU.
- Build uses the existing MSVC/CMake/vcpkg script; restored FFmpeg package
  version `9.0`. Independent metadata inspection used installed
  `ffprobe 9.0.1-full_build-www.gyan.dev`.
- Audio device: not opened by the seek probe; no speaker/audio-output test.

All paths below are relative to this worktree unless stated otherwise. Raw
evidence is retained in `build\hevc01-2026-09-26\` (abbreviated **L** below),
including full stdout, stderr and the EXE-adjacent diagnostic delta for every
invocation, environment/metadata JSON, the runner and build logs. These are
ignored build artifacts, not committed logs; retain this worktree/evidence
directory when integrating the documentation. `L\SHA256SUMS.txt` records the
evidence-file hashes. No media or log is added to the release archive.

### Build and execution accounting

Worktree creation used the repository script, explicitly pinning the base:

```powershell
.\scripts\new-agent-worktree.ps1 -Agent codex -Task seek-probe-hevc01 -Base 5aec10c -DestinationRoot 'C:\Users\<user>\Documents\QuadDeck\build\agent-worktrees'
```

| Result | Operation | Evidence / limitation |
| --- | --- | --- |
| FAILED | Initial `powershell -ExecutionPolicy Bypass -File .\scripts\build-windows.ps1` | Sandbox denied `C:\Users\<user>\vcpkg\buildtrees\0.vcpkg_dep_info.cmake`; stopped during configure. `L\build-windows.sandbox-failed.log`, exit 1. |
| PASS | Same required build command with approved filesystem access | Release build, 8/8 CTests and packaging completed, exit 0. `L\build-windows.log`, `L\build-windows.exit.txt`, `build\windows\Testing\Temporary\LastTest.log`. Compiler output includes FFmpeg-header C4244 and `/DNDEBUG` to `/UNDEBUG` D9025 warnings. |
| PASS | Nine sequential probe invocations, once each | All 27 built-in seek stages printed `result=PASS`, every stdout ended with `close_ms`, and no 60 s watchdog fired. No timing averages or reruns. |
| FAILED | Native process exit-code capture | The PowerShell wrapper retained `ExitCode: null` in all nine `.run.json` files. PASS labels here come from complete probe stdout, not an asserted exit code 0. Wrapper completion is not native exit-status evidence. |
| NOT RUN | Player, visible UI, audio output and the broader R06/R07 matrices | The GUI player was not launched. Neither R06 nor R07 is completed by this probe-only sample. |

The native `60` argument bounds source readiness and each seek separately;
metadata scanning and close are outside that internal timer. The retained
`L\run-probes.ps1` also enforced a 60 s whole-process watchdog and captured
stdout/stderr separately. Before every invocation it checked that
`build\windows\Release\QuadDeck.log` was writable, so diagnostics stayed
beside the EXE; the per-call `.diagnostic.log` contains its appended bytes.
The backend column below quotes those decoder diagnostics, not stdout's
requested `mode=` or an independent GPU trace.

All nine stderr files contain three `UDTA parsing failed retrying raw`
messages and no other messages. These warnings did not prevent the printed
PASS results. No source or test file was changed.

The actual user's `settings.qconfig` content hash and length were identical
in the before/after snapshots, but its modification time advanced from
02:58:49.1915539Z to 03:02:19.7234793Z, after the last probe had finished.
Thus a globally unchanged user-profile timestamp is **not established**.
The probe links no App/settings persistence, and this task launched no player;
the timestamp change is not attributed to a process without audit evidence.
See `L\real-profile-before-probes.json`, `L\real-profile-after-probes.json`
and `L\profile-comparison.json`. The earlier sandbox enumeration returned an
empty array and is not used as proof that the real profile was absent.

### Corpus re-inspection before measurement

HEVC-01 was read from its registered path:
`C:\Users\<user>\Videos\NVIDIA\Desktop\Desktop 2026.09.02 - 23.56.46.01.mp4`.

| Result | Field | Observed now | Comparison with registration |
| --- | --- | --- | --- |
| PASS | File size | 6,180,662,850 bytes | Exact match; path exists. |
| PASS | Codec / dimensions | HEVC / 3840x2160 | Match. |
| PASS | Container duration | 777.115533 s | Rounds to registered 777.116 s; not an exact six-decimal historical comparison. |
| PASS | Average frame rate | 694410000/11656733; probe prints 59.572 fps | Rational matches. `r_frame_rate=60/1` does not establish CFR. |
| PASS | Maximum packet-key gap | 3.484 s, 154.616 to 158.100 s | Reconfirmed by each probe's full packet scan. |
| PASS | Last video packet | PTS 777.098344 s, duration 0.017189 s | PTS rounds to registered 777.098 s. |

Independent ffprobe inspection reported video duration `777.115533` and AAC
audio duration `777.115542`; this is not the fixture with audio extending at
least one second past video. File size and modification timestamp remained
the same after the calls. No historical content hash exists, so metadata
agreement is not a byte-for-byte identity claim.

Metadata commands (the full installed ffprobe path is in `L\environment.json`):

```powershell
$media = 'C:\Users\<user>\Videos\NVIDIA\Desktop\Desktop 2026.09.02 - 23.56.46.01.mp4'
$ffprobe = 'C:\Users\<user>\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0.1-full_build\bin\ffprobe.exe'
Get-Item -LiteralPath $media | Select-Object FullName,Length,LastWriteTimeUtc
& $ffprobe -v error -show_entries 'format=filename,format_name,duration,size:stream=index,codec_name,codec_type,width,height,time_base,start_time,duration,avg_frame_rate,r_frame_rate,nb_frames' -of json $media
& $ffprobe -v error -select_streams v:0 -read_intervals '770%' -show_packets -show_entries 'packet=pts_time,duration_time,flags' -of json $media
```

Both ffprobe commands exited 0 with empty stderr. Their full outputs are
`L\media-metadata.json` and `L\media-tail-packets.json`; file facts are in
`L\media-file.json` and `L\media-file-after.json`.

### Exact probe commands and log mapping

These argument vectors were run once each, in this order, through the retained
runner. `direct` was selected explicitly for the requested 163 and endpoint
calls whose cache mode was unspecified. Forced `cache` on HEVC-01 is a local
cache/AVIO test, not an SMB measurement.

```powershell
$probe = '.\build\windows\Release\QuadDeckSeekProbe.exe'
& $probe $media automatic 158 60 cache
& $probe $media hardware 158 60 cache
& $probe $media software 158 60 cache
& $probe $media automatic 158 60 direct
& $probe $media automatic 163 60 direct
& $probe $media software 163 60 direct
& $probe $media automatic 777.116 60 direct
& $probe $media hardware 777.116 60 direct
& $probe $media software 777.116 60 direct
```

| Run | Requested mode / cache / CLI target | Decoder reported | Result | Full stdout |
| --- | --- | --- | --- | --- |
| 01 | automatic / cache / 158 | D3D11VA (8 threads) | PASS | [01-automatic-158-cache.stdout.log](../build/hevc01-2026-09-26/01-automatic-158-cache.stdout.log) |
| 02 | hardware / cache / 158 | D3D11VA (1 thread) | PASS | [02-hardware-158-cache.stdout.log](../build/hevc01-2026-09-26/02-hardware-158-cache.stdout.log) |
| 03 | software / cache / 158 | software (8 threads) | PASS | [03-software-158-cache.stdout.log](../build/hevc01-2026-09-26/03-software-158-cache.stdout.log) |
| 04 | automatic / direct / 158 | D3D11VA (8 threads) | PASS | [04-automatic-158-direct.stdout.log](../build/hevc01-2026-09-26/04-automatic-158-direct.stdout.log) |
| 05 | automatic / direct / 163 | D3D11VA (8 threads) | PASS | [05-automatic-163-direct.stdout.log](../build/hevc01-2026-09-26/05-automatic-163-direct.stdout.log) |
| 06 | software / direct / 163 | software (8 threads) | PASS | [06-software-163-direct.stdout.log](../build/hevc01-2026-09-26/06-software-163-direct.stdout.log) |
| 07 | automatic / direct / 777.116 | D3D11VA (8 threads) | PASS | [07-automatic-777.116-direct.stdout.log](../build/hevc01-2026-09-26/07-automatic-777.116-direct.stdout.log) |
| 08 | hardware / direct / 777.116 | D3D11VA (1 thread) | PASS | [08-hardware-777.116-direct.stdout.log](../build/hevc01-2026-09-26/08-hardware-777.116-direct.stdout.log) |
| 09 | software / direct / 777.116 | software (8 threads) | PASS | [09-software-777.116-direct.stdout.log](../build/hevc01-2026-09-26/09-software-777.116-direct.stdout.log) |

For each linked stdout, the same basename also has `.stderr.log`,
`.diagnostic.log` and `.run.json`. All timestamps, exact argument vectors,
watchdog results and the EXE hash are retained there.

### Reading the three stages correctly

At this revision every invocation performs three seeks. CLI `158` performs
preview 158, **exact-only 163**, then endpoint. CLI `163` performs preview 163,
**exact-only 168**, then endpoint; it does not select an exact-only 163 call.
The requested exact-only 163 evidence is therefore in runs 01-04 (Automatic
01/04 and Software 03), not relabelled from runs 05/06.

The endpoint argument `777.116` is at/past the registered final PTS. The probe
clamps its preview target to container duration minus 0.5: `776.615533`
(printed `776.616`), then chooses exact-only `771.615533` (stage heading prints
`771.6`). Its separate endpoint seek always uses native container duration
`777.115533` (metadata prints `777.116`; stage heading prints `777.1`).

The tables below retain all 27 observations separately. First/exact times are
the summary's printed milliseconds. `exact_pts / exact_duration / exact_end`
are transcribed from the corresponding `visible ... exact=1` line, which
prints three decimals. The summary itself inherits one-decimal formatting:
for example run 01 prints `158.0 / 0.0 / 158.0`, despite its exact visible line
printing `157.999 / 0.016 / 158.016`. Those verbatim summaries remain in the
logs. The printed duration `0.0` is rounding, not a zero-duration observation;
do not recompute printed end by adding already-rounded PTS and duration.

Targets in these tables are displayed to three decimals using the metadata
and stage-selection rule above. Coverage is the half-open interval comparison
at that displayed precision; it is separate from the probe's PASS test.

#### Preview stages

| Run | Target (s) | First / exact (ms) | exact_pts / exact_duration / exact_end (s) | Covers target? | Result |
| --- | ---: | ---: | --- | --- | --- |
| 01 | 158.000 | 140.1 / 201.9 | 157.999 / 0.016 / 158.016 | Yes | PASS |
| 02 | 158.000 | 108.5 / 154.2 | 157.999 / 0.016 / 158.016 | Yes | PASS |
| 03 | 158.000 | 187.1 / 480.9 | 157.999 / 0.016 / 158.016 | Yes | PASS |
| 04 | 158.000 | 108.0 / 139.2 | 157.999 / 0.016 / 158.016 | Yes | PASS |
| 05 | 163.000 | 92.5 / 139.8 | 163.000 / 0.016 / 163.015 | Yes | PASS |
| 06 | 163.000 | 189.3 / 409.7 | 163.000 / 0.016 / 163.015 | Yes | PASS |
| 07 | 776.616 | 93.0 / 124.1 | 776.615 / 0.016 / 776.631 | Yes | PASS |
| 08 | 776.616 | 46.6 / 93.2 | 776.615 / 0.016 / 776.631 | Yes | PASS |
| 09 | 776.616 | 185.7 / 451.4 | 776.615 / 0.016 / 776.631 | Yes | PASS |

#### Exact-only stages

| Run | Target (s) | First / exact (ms) | exact_pts / exact_duration / exact_end (s) | Covers target? | Result |
| --- | ---: | ---: | --- | --- | --- |
| 01 | 163.000 | 62.6 / 62.6 | 163.000 / 0.016 / 163.015 | Yes | PASS |
| 02 | 163.000 | 62.5 / 62.5 | 163.000 / 0.016 / 163.015 | Yes | PASS |
| 03 | 163.000 | 749.4 / 749.4 | 163.000 / 0.016 / 163.015 | Yes | PASS |
| 04 | 163.000 | 78.9 / 78.9 | 163.000 / 0.016 / 163.015 | Yes | PASS |
| 05 | 168.000 | 77.0 / 77.0 | 168.000 / 0.016 / 168.016 | Yes | PASS |
| 06 | 168.000 | 450.5 / 450.5 | 168.000 / 0.016 / 168.016 | Yes | PASS |
| 07 | 771.616 | 108.4 / 108.4 | 771.615 / 0.016 / 771.631 | Yes | PASS |
| 08 | 771.616 | 77.5 / 77.5 | 771.615 / 0.016 / 771.631 | Yes | PASS |
| 09 | 771.616 | 449.3 / 449.3 | 771.615 / 0.016 / 771.631 | Yes | PASS |

#### Endpoint stages

| Run | Target (s) | First / exact (ms) | exact_pts / exact_duration / exact_end (s) | Covers target? | Result |
| --- | ---: | ---: | --- | --- | --- |
| 01 | 777.116 | 77.8 / 77.8 | 777.098 / 0.017 / 777.116 | No* | PASS |
| 02 | 777.116 | 77.5 / 77.5 | 777.098 / 0.017 / 777.116 | No* | PASS |
| 03 | 777.116 | 727.4 / 727.4 | 777.098 / 0.017 / 777.116 | No* | PASS |
| 04 | 777.116 | 93.9 / 93.9 | 777.098 / 0.017 / 777.116 | No* | PASS |
| 05 | 777.116 | 95.3 / 95.3 | 777.098 / 0.017 / 777.116 | No* | PASS |
| 06 | 777.116 | 509.6 / 509.6 | 777.098 / 0.017 / 777.116 | No* | PASS |
| 07 | 777.116 | 93.3 / 93.3 | 777.098 / 0.017 / 777.116 | No* | PASS |
| 08 | 777.116 | 93.9 / 93.9 | 777.098 / 0.017 / 777.116 | No* | PASS |
| 09 | 777.116 | 501.0 / 501.0 | 777.098 / 0.017 / 777.116 | No* | PASS |

**Endpoint coverage qualification:** the reported interval
`[777.098, 777.116)` excludes the rounded endpoint `777.116` shown in the
table (also the CLI argument in runs 07-09), hence the **No*** entries.
Strict containment of the native target `777.115533` is
not established by the printed precision: that target is within rounding
uncertainty of the reported end. The independent final packet values
`777.098344 + 0.017189 = 777.115533` at six printed decimals are consistent
with reaching the final-frame boundary. Endpoint PASS checks a single exact
observation matching the independently scanned expected final packet PTS;
it does not require the interval to cover container duration. All nine
endpoint summaries print `last_packet_pts=777.1 expected_packet_pts=777.1`.

Cache diagnostics after the stages: runs 01-03 each report enabled=1,
resident_bytes=268435456 and retries=0; runs 04-09 report enabled=0 and zero
resident/fetched bytes. Final queue is 0/4 for Automatic/Hardware and 0/16
for Software. These terminal snapshots are not sustained queue-occupancy or
multi-source measurements. No App seek-barrier timing is produced here.

This is partial R06 evidence for one local HEVC input and partial R07 evidence
for its ordinary endpoint. It does not supply the named H.264 / roughly 10 s
GOP case, the CFR/VFR/damaged-timestamp matrix, the padded-audio or video-only
endpoint fixtures, non-reference-frame classification, multiple-source
synchronization, physical NAS behavior, renderer/UI acceptance or audio
output. **R06 and R07 remain unchecked.** These single observations were not
averaged, load-isolated or used to claim a mode-to-mode performance improvement.

## Open runtime checklist

Every row is still open at consolidation. Partial automated or earlier local
coverage above must not be mistaken for completion of the broader scenario.

- [ ] R01 - Five-source load and throughput: five independent mixed-aspect,
  mixed-duration videos; include 4K H.264/HEVC long-GOP sets in Automatic and
  Hardware, and compare Software where supported. Record actual decoder,
  queue capacity/occupancy, conversion/device-lock contention, sustained
  progress and synchronized seek latency. Five-cache byte tests are insufficient.
- [ ] R02 - Timeline gestures: drag master and every pane near start/middle/end
  with sharply different durations in Linked/Independent modes; include paused,
  waiting and positive-Offset panes and A-B loops. Confirm the intended target,
  one generation on release, no snap-back, no blue remnants, no moving HWND
  under capture, and no overlay kept visible by pointer movement outside the
  foreground player. Pause a pane, clear its Offset, resume and check alignment.
  For F6/Emby +Add, select Linked without seeking, then click 10:00 in a
  20-minute pane and confirm a 40-minute pane lands at 10:00. Continue with the
  bottom rail and arrows past the shorter pane's end; check each queue, EOF,
  audio selection and Emby progress report stays pane-local. Before selecting
  Linked, confirm Independent multi-pane playback shows no aggregate bottom
  time or seek rail and no stale aggregate time in the title, while each pane
  rail and the bottom playback/volume controls remain usable. Recheck the
  bottom rail after selecting Linked and after returning to one browser video.
- [ ] R03 - Five-way audio and identity: enable all outputs, set distinguishable
  per-pane volume/mute, replace V5 and swap with V1 while audible. Test slow and
  fast audio probing, handoff through V5, and repeat these in both seek modes.
  Confirm voices start without checkbox toggles, sound follows pictures, mute
  restores its prior volume, and no stale sound, unexpected pause or handoff loop.
  In three-pane Auto with one portrait, replace upper-right V2 with landscape
  and portrait files; V2 and the other physical cells must stay in place.
- [ ] R04 - Async replacement and repeat: near end-of-deck replace the longest
  selected-audio source by dialog, drop and PageUp/PageDown; repeat during an
  exact barrier and while paused. Verify stable master extent, rebuilt barrier
  generations and duration-aware correction after metadata arrives. Toggle two
  audio outputs around Independent EOF; natural repeat must keep its selected
  voice while a failed generation may hand off. Exercise A-B set/enable/clear
  before and after wrap and while paused; unchanged mappings must not reseek.
- [ ] R05 - Barrier/audio edge cases: shorter-than-video, absent and damaged
  audio; pause during an active barrier then resume at the same target. Exact
  video must resume without an unnecessary 30 s wait, queued tail audio must
  drain before handoff, and stale generations must never satisfy the barrier.
  Validate AAC/Opus encoder delay and real speaker output separately from PCM tests.
- [ ] R06 - Exact frame matrix: known 24/25/30/60/120 fps CFR and VFR; include
  sparse/discontinuous/damaged timestamps and zero frame-duration inputs.
  Record exact_pts, exact_duration, exact_end, first/exact elapsed times in all
  three decode modes. For mixed-rate synchronized cuts/motion, intervals must
  cover the same instant. Fast mode may expose one permitted preceding keyframe
  but no dependency chase; exact-only must show only its exact result. Add a
  named 4K H.264 and roughly 10 s GOP case beyond HEVC-01's partial coverage.
- [ ] R07 - Endpoint correctness: video-only and padded H.264/HEVC whose audio
  extends at least 1 s past video, in all three decode modes. Seek to 100% and
  verify the true final packet/frame interval, including a non-reference final
  frame; no lingering keyframe preview or barrier timeout. Missing/inaccurate
  stream duration remains a separate damaged-metadata case.
- [ ] R08 - Visible layout/DPI: five-source OneRow and Landscape through pane
  widths 335, 303, 254, 195, 141 and 92 px; also very short panes and a narrow
  global dock. Verify clipping, sibling Z-order, usable V/menu fallback, slider
  rail and text, dock animation/re-entry/capture, continuous resize, fullscreen
  and differing monitor scales. With three/five sources choose each V focus,
  cross 3/4/5 counts by remove/re-add, and check valid focus, Solo, hit targets,
  swap and all per-pane controls. Per-Monitor V2 awareness landed in eb2160a
  and has not been seen on screen; mixed-scale monitors are part of this row.
  Drawn interface (BOARD thread 7, `59ab84f`/`d7ffd6e`), never used with a
  pointer: the F5 sheet's scrolling, clipping, controls and close gestures;
  the bottom-edge bar; pane pill and chips on hover; both volume rails; the
  `2`/`M` notices; Direct2D text cost as `lockwait`/`lockheld` growth. The
  application icon (`75ecd6d`) is unseen in the taskbar, title bar and
  Alt+Tab, after a shell icon-cache refresh, and on a monitor of another DPI.
- [ ] R09 - Persistence in the UI: save/reopen five-pane sessions and test copies
  of real QDECK 5, QCONFIG 4 and QSTYLE 2 documents when available. Verify the
  fifth pane's defaults and retained view/audio state. In an isolated profile,
  drag settings/shader sliders, verify complete repaint and coalesced saves,
  test locked/unlocked save and bounded retry, then normal shutdown while a
  save is pending. Confirm final state persists and failure preserves the old
  document. A crash may leave a recoverable .rollback file; MAX_PATH dialogs
  and network/cloud atomic-replace support remain limitations.
- [ ] R10 - Slow I/O and real NAS: close/replace during slow FFmpeg open/probe/read
  and test actual UNC/mapped-share detection, cache switch on subsequent opens,
  paused fill, seeks, budget changes, throughput dips, disconnect/reconnect and
  cancellation latency. Verify resident bytes, retries, data integrity and
  cleanup; record rather than assume bounded kernel cancellation. Include a
  live writer-held recording that falls back to uncached FFmpeg and concurrent
  audio/video demands after f5400e2. Verify an idle full cache wakes for new work
  and detects quota changes; the one-second fallback is not dedicated test coverage.
- [ ] R11 - Device recovery: exercise actual device loss, an initial failed
  rebuild and a later successful retry. Verify source identity, position,
  playback intent and shader state survive; transport, seek, drop, close and
  session actions during recovery must update the eventual restored state.
  No permanent black frame, hang or surface churn. The null-HWND ShaderProbe
  failure case alone does not cover successful App-level recovery.
- [ ] R17 - The next video shows none of the previous one's time (added by
  Claude Code, 2026-10-02, branch `claude/video-playback-time-persist-0a514b`
  from `cb65fbf`). The human's 2026-10-01 log: an Explorer-opened file, then
  F6 Play of others while the master clock ran on, and keyframe seeks on the
  bar right after. Checked by regression only
  (`onlyVideoShowsNoTimeFromTheOneBefore`, core `barTimelinePane`): bar text,
  title and bar drag of the only F6 / Emby pane, and Page Down, Page Up and a
  drop on one manual video, all with synthetic panes. To see: open a file
  from Explorer, play a minute, F6 Play another -- bar and title start at
  0:00 with the new length, a bar drag lands where pointed (on a keyframe);
  the same with the next Emby item, including the moment it resolves; Page
  Down on one Explorer file starts at 0:00 playing, and paused when paused;
  among two manual videos Page Down on one keeps the deck time.
- [ ] R18 - The window without a title bar (added by Claude Code,
  2026-10-02, branch `claude/potplayer-hidden-window-style-b5ad2a` from
  `46f6ac7`). The human asked for PotPlayer's "auto hide main skin while
  video screen is active". Checked by core tests (`captionLayout` for every
  width 1-1920, `windowFrameHitTest`, the pill and sheet offsets) and the
  App regression. Seen live on Windows 11 (build 26300, 3840x2160 at 200 %,
  empty window, posted messages and PrintWindow only): no Windows title bar,
  the client starting at the window's top with a 13 px frame on the sides
  and bottom, the strip and bar up with nothing loaded, the F5 sheet's
  header below the strip, `WM_NCHITTEST` answering HTTOP / HTTOPLEFT /
  HTCAPTION / HTLEFT / HTBOTTOM where expected, and the drawn Close closing
  the window. That check restored a minimized window, a size change; the
  human's first ordinary launch (2026-10-03) still showed the Windows title
  bar above the strip, because CreateWindowExW's bare-RECT WM_NCCALCSIZE
  was left to the default and nothing resized the window. Reproduced and
  fixed with a launch started SW_HIDE: caption 58 px before, 0 px after.
  Still to see, on Windows 11 and on Windows 10 if available:
  the caption wakes from the top and bottom edges and hides with the bar
  while a video plays; Minimize,
  Maximize/Restore and Close, with their tooltips and the red Close; a drag
  on the strip moves the window and snaps it to an edge or a corner, a
  double-click maximizes and restores, a right-click and Alt+Space open the
  system menu; the top band resizes (corners diagonally), the sides and the
  bottom resize outside the picture as before; maximized, the strip meets
  the screen's top, nothing is cut off, and an auto-hide taskbar still
  rises at its edge; Alt+Enter in and out of fullscreen from a normal and
  from a maximized window; a second monitor with another DPI; the F5 sheet,
  the docked F6 list and the whole-window Emby browser start below the
  strip and Close stays reachable; with one video a drag on the picture
  moves the window (and restores a maximized one), with two it still swaps
  panes; the right-edge browser zone still opens the list rather than
  resizing.
- [ ] R24 - The F6 list and the Emby browser mark videos with subtitles
  (added by Claude Code, 2026-10-07, branch
  `claude/f6-menu-video-subtitle-marker-10fc0b` from `74e0375`). The
  folder list is checked by regression only: `markSubtitleFiles` (emby
  tests), `localFolderIsTheF6List` (a `.chs.ass` beside one video, a near
  miss beside another, a stream reported inside a third; list `tag` and
  `tileTags`; kept across a forced refresh) and
  `localProbeFindsTextSubtitleStreams` (written Matroska files: SubRip
  counts, PGS does not). Not seen: the pill itself was never drawn on
  screen, as the human was at the machine. To see: F6 on a fansub folder
  of the share in List, Posters and Thumbnails -- `Sub` before the length
  on rows with an `.ass`/`.srt` of their name or a text track inside, none
  on the rest; it is readable on the playing (accent) row, does not cover
  the Add button, and on a narrow docked sheet the name gives way first;
  `Refresh` after dropping a subtitle file in marks its video.
  The Emby browser marks the same way (same branch, at the human's
  request). Checked against the human's server, read-only, with
  `QuadDeckEmbyProbe --auth ... --subtitle-flags`: list replies carry no
  `HasSubtitles` (0 of 15393); asked by id as the browser asks, 589
  videos marked, exactly the 589 with a text stream, 154 requests, 243 KB
  (largest reply 42 KB); item 109320 (PGS only) is not matched by the text
  codecs while `HasSubtitles=true` matches it. Regression:
  `embyBrowserMarksVideosWithSubtitles`.
  Not seen in the real player: marks appearing a moment after a page, on
  Posters/Thumbnails over watched bars, the resume row and a search; a
  click right after a page opens is not held up noticeably by the
  questions queued ahead of it.
- [ ] R22 - Subtitles inside an Emby item's file (added by Claude Code,
  2026-10-03, branch `claude/gridman-subtitles` from `1fe4032`). The human
  saw neither embedded stream (2, Simplified Chinese and Japanese, and 3,
  Traditional) nor the two ASS files beside the video (13, 14) of Emby item
  124393, the R21 film. Measured before the change with GET requests from a
  scratch probe, nothing played: `Subtitles/2` and `/3` as `Stream.ass` and
  as `Stream.srt` each failed after 31-32 s (WinHTTP 12002), every time;
  13 and 14 came as ASS in 69-101 ms (3944 cues each) and libass drew them
  with the fonts installed for the user. The player's log showed the
  timed-out requests holding the one subtitle client. After: a scratch
  FFmpeg probe on the same file from the share finds ASS streams 2 and 3,
  with their headers (4445 and 4444 bytes), at the indexes the server gives,
  and decodes 58 events of each in the first 120 s (47 MB read, 2.7 s) and
  54 after a seek to 3000 s. Checked by regression
  (`subtitlesAreChosenForTheViewer`, `embyResumeAndEndStaySourceLocal`):
  the options, the choice, the server's default inside the file, the
  fallback to the server, the stream options. Not run, because playing
  reports to the human's server: the player on that item through Emby --
  stream 2 shown from the start, 3 and back at the menu, 13 and 14 within a
  second, a seek into a line, the attached fonts, Alt+L through all four.
- [ ] R21 - Audio after a seek in a Matroska film on the share (added by
  Claude Code, 2026-10-03, branch `claude/emby-gridman-universe-no-audio-148864`
  from `392c552`). The human heard no sound in `<share>\...\[XKsub&LoliHouse] GRIDMAN
  UNIVERSE [BDRip 1080p HEVC-10bit FLAC ASSx2].mkv` (6.6 GB, 7070 s, HEVC 10-bit 1080p, FLAC 48 kHz s32 stereo,
  Cues for the video only; K: is a share on the NAS). Measured
  offline with the production audio worker (scratch probe, no window, no
  device): before the fix a seek to 4000 s gave its first 8 s of audio after
  31.9 s through the cache and none in 65 s direct (FFmpeg read errors);
  FFmpeg alone seeking the audio stream took 4.6 s to 600 s and 43.4 s to
  3000 s, and 60-80 s to 1500 s in both Gurren Lagann films on the same share.
  After: 4000 s in 4.1 s, 5685.9 s in 1.6 s, 600 s in 2.5 s (open, seek and 8 s
  of audio, cached), 6541.5 s direct in 1.8 s; identical peak levels at 4000 s
  before and after. Not run: the player itself on that file (drag, arrow
  keys, exact seek, audio heard), a Matroska film from Emby, and whether the
  video's stalls in the same log (`decoded=0.00/s`) go with the audio.
- [ ] R20 - Subtitle files opened from Explorer (added by Claude Code,
  2026-10-03, branch `claude/subtitle-file-default-association-70f43c` from
  `2f4168e`). Checked: the plan and status tests (core), the sidecar and
  video-for-subtitle rule (emby tests), and the App regression
  `externalSubtitleFilesFindTheirVideo` with hidden windows and no decoder:
  a subtitle file goes on the pane of its video or on `subtitlePane()`; a
  launch of a video and its subtitles opens one deck with them; a subtitle
  alone into an empty deck lists a temporary folder off the window thread,
  holds later launches, opens `Clip One.mkv` rather than `Clip.mkv` and
  reads the SRT; none found is a notice; a video opened during the read
  takes the subtitles; a subtitle ahead of its video in one launch, or in
  the launch before (queued, or arriving within the 750 ms wait), goes on
  that video, a better name to come beats a shorter one open, and a pane
  still opening gets a notice. The wait's length against real Explorer
  multi-select launch spacing is unmeasured. Not run: Register on this machine (it writes
  `HKEY_CURRENT_USER`; the human presses it, after which the status line
  should read Registered rather than the older-build note), choosing
  QuadDeck for `.ass` and `.srt` in Default apps (PotPlayer holds them
  machine-wide today), a double-click on a subtitle file with the player
  closed, with its video playing, and with another video playing, a
  subtitle file on the NAS, and the subtitle actually drawn on the video.
- [ ] R19 - ASS subtitles drawn by libass (added by Claude Code,
  2026-10-03, branch `claude/ass-rendering` from `e67b038`; RTX 4060 Ti,
  one 3840x2160 display at 200 %, HDR on, so the player's output was 10-bit
  PQ). The human asked for "the various ASS features, such as position".
  Seen in the running player once, `ass demo.mkv` (synthetic, made with
  ffmpeg 9.0.1: `gradients` 1280x720 30 fps 30 s, libx264 `-g 60`, BT.709,
  an ASS stream and a TTF attachment, a copy of Segoe Script renamed
  "Qegoe Script" so that only the attachment can supply it): the \pos sign,
  the attached face, \frz with its shadow and the blur glow in their places
  over the picture, through the PQ overlay path. That file had been muxed
  without `-max_interleave_delta 0` and eight of its twelve events lay at
  about 23 s of the file; they did not show at 5 s, which is how the
  player reads embedded streams (as the demuxer passes them), not a fault
  of the drawing. The rest was seen offline, by a probe that drives
  `VideoSource` in software and `AssSubtitles` as the player does and
  writes the picture: the remuxed demo with all twelve events (\pos, \move,
  \an8 with \fad, \k/\kf karaoke, \frz, \blur, \clip, \p1 shape, two bottom
  lines stacked, the attached face); a plain SRT beside an MP4 (italic,
  <font color>, {\an8} at the top, the old white-and-outline look); and
  read-only from the share, `<series>/Season 01/S01E12.zh-CN.ass` (a SweetSub
  release, 1139 events, 17 styles, faces Source Han
  Sans/Serif, FOT-CinemaCN, FOT-Budo, all installed here) at 152.6, 190.5,
  1215.5, 2565.5, 2750.5 and 2783 s: bilingual dialogue in its two styles,
  the opening's blurred lines top and bottom, \pos captions. Per frame at
  3840x2160: static 0.1 ms; the opening's fades about 4 ms; the demo's
  frame full of moving effects 8-9 ms (22 ms before compositing went to
  pieces); a style's first line 20-50 ms (its face loaded). The one player
  run put the window in the foreground for about a second while the human
  was typing elsewhere, despite the off-monitor settings and the minimised
  start that kept it behind on 2026-10-01; the cause is not known, so no
  further run was made. Not seen: anything in the player after that run --
  above all an MKV from the share with its subtitles and attachments
  inside, a seek into a karaoke line, Alt+PgUp/PgDn and Alt+Up/Down on a
  script with signs, the bar lifting the bottom lines, a pane in Fill and
  zoomed, several panes with subtitles, the box on a plain file; anything
  from Emby (an ASS stream fetched as ASS and drawn so); a script naming a
  face that is neither installed nor attached; SDR output. To see first:
  play one fansub MKV with fonts attached and one with an ASS beside it,
  through an opening and a sign.
- [ ] R16 - Subtitles chosen and shown on their own (added by Claude Code,
  2026-10-01, branch `claude/subtitles` from `562d162`; RTX 4060 Ti, one
  3840x2160 display at 200 %, Windows language list en-US then zh-Hans-CN,
  system code page 936). The human asked for a subtitle function; the
  existing one never showed anything on Emby for them, and the first part
  of this entry is why.
  Server, read-only with the saved sign-in (GET only, nothing played):
  `QuadDeckEmbyProbe --auth <emby.qauth> --subtitles` -- account subtitle
  mode `Smart`, no subtitle language; 13954 videos, 576 with subtitles, all
  576 with a text stream, 91 with a stream flagged default; by stream: ass
  beside the file 418 in 376 videos, ass in the file 62 in 31, srt beside
  7686 in 161, ssa beside 2233 in 13, subrip in the file 563 in 60, sup
  beside (pictures) 5 in 5. `--subtitle-streams` on items 110613, 110527,
  109853 and 110741: `GET /Items/{id}/PlaybackInfo` named no default
  subtitle (-1) for any of the four, 110527 and 109853 having a stream
  flagged default; every text stream came as SRT (0.04-25 s, the longest
  the server's first extraction of a stream inside the file) and, where it
  is ASS or SSA, as ASS too (0.03-1.8 s), with equal cue counts; of 110613's
  373 cues 61 are placed at the top by the SRT's `{\an}` and 72 by the ASS.
  110787 has 186 SSA files beside it, 110741 twenty-six SRT.
  Player, the Release build of the working tree, driven by posted window
  messages and captured with `PrintWindow`; synthetic fixtures made with
  ffmpeg 9.0.1 and not kept: `testsrc2` 1280x720 30 fps 40 s, libx264
  `-g 60`, no audio; `embedded.mkv` with a SubRip stream (`chi`, default,
  five cues, one `{\an8}`, one 80 characters long, one from 21 s to 39 s)
  and an ASS stream (`eng`; a line layered twice in a top-aligned style, a
  `\pos` sign in the upper half, a `\p1` drawing); `sidecar clip.mp4` with
  `sidecar clip.cht.srt` and `sidecar clip.eng.ass` beside it. Seen: the
  SubRip stream chosen and shown at open (`2 text subtitle stream(s) are
  read with the video`, `stream 1 inside the file is shown`); a two-line
  cue at the bottom with the `{\an8}` line at the top; the long line
  wrapped inside the pane and standing clear of the pinned bar; the bar's
  subtitle button highlighted; Alt+L to the ASS stream showing its line at
  once, the layered line once, the sign at the top, no drawing; Alt+PgUp
  three times and Alt+Up five times (130 %, 16 %), `,` (+0.50 s) and the
  sheet's Subtitles section showing all three; Page Down to the sidecar
  clip showing the `.cht.srt` rather than the `.eng.ass`; Alt+L loading
  the `.eng.ass`; five Right-arrow keyframe seeks while paused landing at
  6, 12, 18, 24 and 30 s with the line that began at 21 s on screen at 30.
  Chinese lines drew in a Chinese face. The first run put the window on
  screen and took the foreground from the human's own program for about a
  minute before it was handed back; later runs started minimised and were
  restored behind every other window (`SetWindowPos(HWND_BOTTOM)`), which
  `PrintWindow` still captures. `settings.qconfig` was replaced by a test
  copy for the runs and put back (SHA-256 equal before and after;
  `emby.qauth` never written).
  Not run: anything played from Emby -- it would write play state into the
  human's account -- so the player fetching a stream as ASS, the fallback
  to SRT, the choice on a real item and a menu of 186 streams are the
  probe plus regressions only; the popup menu and the bar button's menu
  (modal; the regression builds the menu and reads its state); Alt+O's
  dialog; the Alt keys from a real keyboard -- the player now refuses the
  menu mode Windows enters when Alt is let go on its own (`SC_KEYMENU`),
  and what was run is the posted sequence Alt down, H down, Alt up, H up,
  Space (the subtitles hid and playback started), which is not a
  keyboard: Alt+H with Alt let go first, then Space, must pause and not
  open the system menu, and Alt+Space must still open it; a subtitle file
  dropped on a pane; a real fansub MKV from the
  share, and what the seek's early read costs there; a stream of pictures
  in a real file (the CTest's fixture has one); several panes with
  subtitles at once; subtitles over 10-bit PQ output; a legacy file in a
  code page other than the system's outside the regression (Big5 and GBK
  bytes there). To see first: play an anime episode from Emby and an MKV
  with its subtitles inside from the share; Alt+L, Alt+H, `,` and `.`;
  the bar's button; drop an `.ass` on the picture.
  Later the same day, the box behind the lines made a choice and off by
  default (the human had run the build and asked whether the translucent
  frame can be turned off). Seen, by the same posted messages and
  captures, `embedded.mkv`: with a `QCONFIG 11` settings file -- what the
  human's own file is since they ran the first build -- the lines come
  without the box, white with a solid black outline, legible over the
  pattern's yellow, cyan and blue; the sheet's `Dark box behind the lines`
  is off, a click on it puts the box back at once; the file the player
  saved on closing is `QCONFIG 12` with `subtitles 1 0 1 0.06 1`, and the
  next start shows the switch on. Not seen: the outline over a real film's
  white (snow, sky, a title card) at a small pane in a grid of five, where
  it is one pixel wide; PQ output. The second start of that run came up on
  screen and in the foreground for a second or two before it was put
  behind the other windows: the first instance had saved the on-screen
  placement the script gives it, so the settings file no longer named a
  place off every monitor. The test copy must be put back before every
  start, not only the first. The human's `settings.qconfig` was restored
  afterwards (SHA-256 equal to before the run; `emby.qauth` not written).
- [ ] R15 - One video follows the master timeline (added by Claude Code,
  2026-09-29, branch `claude/single-video-timeline`). The human saw a
  looping video whose bar did not reset; their `settings.qconfig` had
  Independent seek bars and every pane's repeat flag on. Checked by
  regression only (`singleVideoFollowsTheMasterTimeline`). To see: one
  video with "Repeat one" loops and the bar returns to the start each
  time; arrow keys move the bar; the pane's repeat chip toggles the rule;
  with two videos Independent mode and per-video repeat behave as before;
  closing one of two while the other has wrapped past its end.
- [ ] R14 - Local folder list and Explorer file types (added by Claude
  Code, 2026-09-28, branch `claude/local-playlist`). Checked: the App
  regressions (`localFolderIsTheF6List`: folder listed off the window
  thread, F6 docks it, the playing file marked, sort keys re-order the list
  and the walk, a chosen row takes the pane; the edge opens the folder of a
  local video) and the shell thumbnail of
  `C:\Users\<user>\Videos\test1.mp4` through `LocalThumbnailer` (a 436534
  byte BMP, asked for at 440x248) and its length through the folder scan
  (6.442 s). The regressions also cover lengths arriving, the order by
  length with unread lengths last, and each list coming back where it was
  left. Not run: the list with a pointer in the
  real player, thumbnails of videos on the NAS (a provider may have to read
  the file), a folder of thousands, and everything about file types -- the
  agent never ran Register, which writes `HKEY_CURRENT_USER` and opens
  Windows' Default apps; the human presses it. To see: Register, choose
  QuadDeck for `.mp4` in Default apps, double-click a video in Explorer
  (a new QuadDeck window plays it with its folder in the list), Remove,
  and the status line after each.
- [ ] R13 - Emby, subtitles and audio tracks (added by Claude Code, 2026-09-26,
  branch `claude/emby`, server `<nas>` Emby 4.11.0.3 at
  `http://<nas-ip>:8096`, a password-less test user). Run so far,
  with the desktop LOCKED so nothing was seen: `QuadDeckEmbyProbe` signed in
  through the player's own EmbyClient and wrote `emby.qauth`;
  `QuadDeck.exe emby://<server-id>/109672` (86
  Eighty-Six S1E12, mp4 h264 1080p) resolved the item, opened the direct
  stream with the token header (D3D11VA, Super Resolution and RTX Video HDR
  applied), the server's /Sessions showed the test user's session playing it
  paused at 0, and after closing showed nothing playing and PlayCount 1;
  the test user's position was reset afterwards. `QuadDeck.exe <jpeg>` opened
  a one-frame mjpeg source (software decode, no audio) without error. Not
  run: the sign-in dialog and browser with a pointer, playing from the
  browser, resume and autoplay, next/previous, progress reports while
  actually playing (only Playing and Stopped were observed), subtitle
  rendering and the Subtitles menu (Gurren Lagann 110010 has an external ASS
  stream; Made in Abyss 93470 has two), the Audio track menu (110011 has
  four audio streams), a photo item (the library has EnablePhotos=false,
  so none exists), a second display or device recovery with an Emby pane,
  an expired token, and a server that refuses direct play. 2026-09-27,
  after the thread 10 fixes: the same locator run repeated on
  109675 (S1E13) with the desktop still locked -- resolved, opened,
  reported, stopped, position reset afterwards -- so the serial-lookup
  handlers work against the real server; nothing new was seen.
  2026-09-27, the grid browser (views, sort, filters, pictures, search as
  the playlist, the sign-in page): only the unit and App regressions were
  run; the desktop was not free for a pointer. Not seen against the real
  server: pictures arriving and decoding in the tiles (the image endpoint
  itself was checked with curl on 2026-09-26, including its 500 for an item
  without one), each `SortBy` on the 10120-video library "1" and its
  `Filters=IsUnplayed` (checked with curl on 2026-09-26 for
  `IsFolder,SortName` and the per-field orders only), `Random` paging (the
  server's random order is not stable across `StartIndex`, so `Show more`
  may repeat items), the flatten toggle on that library (300 of 10120 a
  page), Page Up/Down along a search's results, the wide sheet on a second
  display's DPI, and the bitmap cache under a device loss.
  2026-09-27, later: the human ran that build and saw no thumbnails -- the
  picture client's replies were never pumped (fixed, with the regression
  extended to wait for the replies). Added on the same day and not seen
  running: the browser docked over the right quarter of a playing video
  (F6; the human saw the first, half-width shrinking version and asked
  for this one), the Previous/Next bar buttons, the playback orders at the end of
  the only video (Ctrl+4/6/7/8/9) against real files and against an Emby
  list, and `\` toggling the A-B loop. The human then explained
  Ctrl+4..9 are PotPlayer's playlist *sort* keys; they are sorts now
  (name, size, date, length, random). `Fields=Size` and `SortBy=Size`
  (also `IsFolder,Size`) were checked with curl as the test user on 2026-09-27
  against 4.11.0.3; `FileSize` is a 500. Not seen: the re-sorted playlist
  walked with Page Up/Down, and a local folder in size or date order.
  Same day, later: drag-scrolling the sheet and its scrollbar, the playing
  item's mark, progress beside length on rows and tiles, and the right-edge
  opening (and self-closing) of the docked browser are covered by App
  regressions driving `handleVideoMessage` and `updateHoverControls` on
  the hidden window; none was seen with a real pointer.
  2026-09-27, the seek delay the human reported on Emby: item 102779
  (4K60 HEVC mp4, keyframes every 10.0 s by ffprobe). FFmpeg 9.0.1 CLI on
  the stream as the test user: exact seek to 177 s + one frame 5.14 s (d3d11va),
  3.32 s (software), keyframe seek 0.50 s, open + one frame 0.52 s. The
  player's own `VideoSource` through `QuadDeckSeekProbe` on the same
  stream (hardware, direct): exact preview seek exact_ms=372, exact-only
  140, end seek 156; keyframe landing exact_ms=80 (landed 170.0, audio
  generation re-aimed), forward 80 (landed 180.0). The human's session log
  (dist/QuadDeck/QuadDeck.log, RTX Video HDR on, 10-bit PQ): present
  13-15 ms and held 14.5-16.7 ms per frame, decode 55/s for the 60 fps
  stream -- the lock across Present starving the decoders. Not seen: the
  new GPU wait in a real session (the shader probe renders through it
  without hanging), the keyframe landing through the whole player, and
  the filled grid. Later the same day: Super Resolution is no longer set
  on a processor that does not magnify (the human's log had it on at
  3840x2160 -> 3840x2160 together with RTX HDR), and the volume rail is a
  vertical popup on the speaker; both unseen in a real session (core
  layout tests and the shader probe only).
  2026-09-30, branch `claude/emby-playlists`, playlists and libraries
  shown without their folders, server now 4.11.0.4. Read-only, through
  the player's saved sign-in (a password-less account), no password used and
  nothing played, so no report reached the server: `QuadDeckEmbyProbe
  --auth <emby.qauth> --library 39239 playlists --playlist 119996`
  listed the three playlists with `ChildCount` (403, 0, 0) and the 403
  entries with `PlaylistItemId` 1, 2, 3, ... in the playlist's order;
  `--folder 119996`, the request the browser made before, returns the
  same entries by name (places 55, 73, 3, 371, ...). Checked with plain
  GETs: `/Playlists/{id}/Items` ignores `Filters=IsUnplayed` (total 403)
  while `/Users/{id}/Items?ParentId=` honours it (398); `SortOrder`
  without `SortBy` does not reverse a playlist; the flat listing of
  library "1" is 12903 videos and a 300-item page takes about 0.2 s in
  any order. Seen in the running player (the dist build driven with
  posted window messages and captured with PrintWindow, 3840x2019 at
  200 %, the pointer untouched): the playlists library with its item
  counts; playlist 119996 opening in its own order with `Playlist`
  chosen, all 403 entries there at the list's end with no `Show more`;
  `Name` re-sorting it and `Playlist` bringing the order back; library
  "1" with `Show: Folders | All videos`, `All videos` listing videos,
  and further pages arriving as the list was scrolled; the same pages in
  the browser docked beside a local clip, the eight sort segments
  wrapping six and two. Not run: playing anything from a playlist or a
  flat list against the server (Page Up/Down, the playback orders and
  the two-alike case are App regressions only) -- it would have written
  play state into the human's own account; a playlist of more than a
  thousand entries; a page that fails on a real network; the pictures
  catching up after a long fast scroll (after 3000 wheel steps the
  tiles on screen were still without pictures three seconds later).
  2026-09-30, later, same branch: the human ran the build (07:02-07:10)
  and reported that the player did not get what Emby had just updated.
  From `QuadDeck.log` and the server's activity log, read-only: item
  114403 was played in QuadDeck to 6243 s, left at 07:06:22, chosen again
  at 07:06:39 and four more times, and each time played on from about
  2807 s -- the position its list entry had when the list was fetched --
  while the server held 6833 s; at 07:09 an item was played in the
  server's web client, which the browser, staying up, could not show.
  Two causes, both fixed: the resume position was the list entry's
  unless that was zero, and a list was asked for only when the browser
  was opened. Seen after the fix in the running player (posted messages,
  a local silent clip loaded, nothing played from the server):
  `QuadDeck.log` had `Emby: asked again for home, 7 items, nothing new`
  35 s after the docked browser opened and `asked again for
  library:114041, 201 items, nothing new` 40 s after that library was
  opened; two PrintWindow captures of the library 36 s apart, across a
  refresh, were byte-identical (no row appearing, no scroll, no blank
  tile); the navigation row showed its four buttons legibly docked at
  480 DIP. Not run: a refresh bringing real news (the agent makes no
  change on the server), the resume position against the server (App
  regression only), the window coming back to the front with a pointer,
  a press under a refresh, the navigation row at the 320 DIP minimum.
  2026-09-30, later still: the human said what they had meant -- videos
  the server had newly found did not show in the player. Read-only,
  saved sign-in: of 14012 videos 1124 have item numbers above 122000
  (990 in folder 120602 `<folder>`, 62 in 122686, 2 movies in library
  114041); every request the player makes returns them -- the folder
  (2184 items), the flat library (12891), the movie library (201), and
  `SearchTerm` by name -- so nothing is withheld by the server. But
  `DateCreated` of those videos is the file's date (2023-12-20,
  2026-04-18), `SortBy=DateCreated` and `/Items/Latest` list them there,
  and none of the seven orders brought them to the front.
  `SortBy=DateLastSaved` answers 200 alone and as `IsFolder,DateLastSaved`
  in a library, a folder, a flat library, a series library and a
  playlist, 0.06-1.3 s a page of 300, and puts the newly found first
  (123183, 123182, ... in library 47096; 123200, 123199 in 114041);
  `DateAdded` is a 500; the `DateLastSaved` field itself is not
  returned even when asked for. Seen in the running player (posted
  messages, nothing played): the movie library with eight orders on one
  line, and with `Updated` chosen the two newly found movies first and
  the line reading "what the server found or changed last comes first".
  Not run: `Updated` in a folder and in the flat library in the player
  (the requests were run by hand), the order after the server refreshes
  metadata of old items, a server whose ids are not numbers.
  2026-10-01, Codex, `codex/emby-movies-tv` from `586a368`: movie/Series
  year and contextual episode labels, season counts/fallbacks, Episode
  search, paged TV hierarchy and bounded season completion were exercised
  by the API/App regressions; all 10 CTests, Release build and packaging
  passed. Eight actual production Overlay WARP renders with synthetic
  metadata were generated and inspected at 1x/2x scale, including a 320 DIP
  dock. Year, S/E range, count, progress and watched labels were legible;
  final long-series-name fixtures retain the episode code at the beginning
  (the episode name comes first when numbering is absent). Long titles and
  the sort hint retain their existing narrow clipping. These are offscreen
  fixtures with missing-image placeholders,
  not pointer interaction, real covers or a physical DPI change. Separately,
  an anonymous GET-only survey of existing saved authentication found Movie
  entries in both movie libraries and Series -> Season -> Episode types in
  two TV samples; all ten sampled episodes had runtime and were playable.
  That survey used Items/ParentId, not the new /Shows paging paths. No real
  media was played, no playback records were changed and no account details
  appear in the new evidence. Latest-position/rewatch behavior remains the
  existing policy, now covered for Movie and Episode with synthetic replies.
  Evidence, exact commands, artifact hashes and the unchanged real-server
  interaction/playback gaps were in the branch note, retired at `ff5b46d`
  (`git show ff5b46d:docs/handoffs/codex-emby-movies-tv.md`); images and logs
  are under `build/validation/` of Codex's worktree. R13 remains open.

  Codex follow-up on `codex/emby-movies-tv` from `7c75afc`: full Movie/Series
  information pages and QCONFIG 13 per-server/library switches now have
  mock state and production Overlay/WARP evidence. Fifteen readbacks were
  inspected, including seven new fixtures for wide/narrow/2x movie details,
  expanded synopsis, missing metadata, Series season/episode controls and
  F5 library switches. Synthetic poster/backdrop bytes exercised WIC drawing
  with pixel-difference checks; the new scenes use actual App row builders.
  Final Windows Release build/packaging passed 10/10 CTests (23.35 s), and
  focused App, Emby, Core and Persistence tests passed. Details, logs, hashes
  and explicit review/test boundaries were appended to the same retired note.
  No real credentials/server/history, user-window input, real cover request,
  stream/resume/report or display/HDR acceptance was exercised in this phase.
  No R13 runtime checkbox is completed by these mock/offscreen results.

  Codex multi-pane follow-up from `69c8f7e`: pure queue/seek-barrier tests,
  mock App state tests and nineteen production Overlay/WARP scenes cover
  automatic empty-pane Add, muted admission, explicit replacement/cancel,
  target identity, account isolation, per-pane reports/queues, duplicate
  handling and local/Emby coexistence. Four new readbacks include a full
  App layout with two synthetic panes beside the detail browser, target and
  audio chrome, the All transport label, and five-pane replacement choices.
  Native Release compilation and 10/10 CTests passed (23.67 s); the standard
  replacement guard preserved the running old package, and a fresh separate
  candidate package was created with matching Release hash. A final rerun
  reproduced all nineteen inspected readbacks byte for byte.
  These simulate pane state and artwork; they do not decode concurrent
  streams or measure audio output, exact-frame seek, GPU load, real Emby
  session ownership or server-side progress. No real credentials, server,
  playback history or user window was accessed. R01-R13 runtime obligations
  remain open as before. Exact commands/results and protected package paths
  were appended to the same retired note.

  Still to see from the Codex phases above and BOARD thread 19: the server's
  answers to the `/Shows` season/episode paging and the missing-SeriesId
  requests; real covers and backdrops on the detail pages, used with a
  pointer; several Emby panes reporting at once from one device (server
  acceptance unverified); F6 and the right edge reopening a real `Random`
  list without reshuffling it, while explicit `Random` and `Refresh` do.
- [ ] R12 - NVIDIA RTX Video (added by Claude Code, 2026-09-26, branch
  `claude/rtx-video`): with an RTX GPU driving an HDR-enabled display, switch
  on `F5 -> Picture -> RTX Video Super Resolution` and `RTX Video HDR` and look
  at the picture. What was checked on this machine (RTX 4060 Ti, driver
  616.64, `\\.\DISPLAY9` HDR on, SDR white 288 nits) is only what
  `QuadDeck.log` can show: the driver accepted both extensions at the probe,
  the swap chain switched to 10-bit PQ, Super Resolution was accepted on the
  real 3840x2160 processor, and the interface layer composited legibly in a
  GDI window capture. Not checked by eye: whether the HDR picture looks
  right, whether Super Resolution visibly sharpens an upscaled SDR source,
  the interface layer's brightness against a real SDR window, a display
  whose HDR is switched while playing, a window carried to a second display,
  device recovery in PQ mode, and five panes with both features on (GPU
  load). HEVC-01 is HDR10 and is passed through, so it exercises the
  pass-through path, not TrueHDR; `C:\Users\<user>\Videos\test1.mp4`
  (H.264 720x1280 BT.709 SDR, 25 fps, 6.44 s, unregistered) is the SDR
  source used for the TrueHDR and magnification log lines.

  Codex, 2026-10-01, `codex/rtx-hdr-vibrance` from `1376fa2`: the built-in
  Vibrance shader now distinguishes RTX-produced PQ from native PQ per pane.
  The native `QuadDeckShaderProbe` rendered/read back 354 synthetic PQ patches
  at four parameter combinations, with additional neutral, gray, warm/cool,
  near-black, highlight, saturated-edge and slider-direction checks. Maximum
  measured linear-Y relative error was 0.0000762329 (about 0.0076%). It also
  checked native/refused-PQ passthrough, unchanged SDR/software behavior,
  alternating routes, b2 unbinding and the same checks after device rebuild.
  The required Windows build passed all 10 CTests and packaging. Evidence and
  code/artifact fingerprints: `build/hdr-vibrance/`, detailed in the
  retired branch note (`git show ff5b46d:docs/handoffs/codex-rtx-hdr-vibrance.md`).
  This is shader pixel validation, not a test of actual TrueHDR inference or
  visual acceptance on the display. No media playback, 4K60 cost measurement,
  five-pane HDR load or Windows HDR hot-switch test was run. R12 stays open.
  To see (merged as `e2f983f` before it was done): `F5 -> Picture ->
  Vibrance+` with RTX Video HDR on SDR footage, intensity 1.00 against
  1.2-1.5, on faces, bright signs and dim colour gradients; native HDR10 on
  the same display must bypass it; measure the 4K60 and multi-pane cost
  before calling playback smooth.

## 2026-10-01: software SDR / RTX HDR, partial R01 / R12 evidence

Codex, base `12b137e`, current `codex/emby-movies-tv` HDR-only change. The desktop,
Windows 10.0.26300, RTX 4060 Ti, driver 32.0.16.1714. No user frame or desktop
playback was inspected; no display setting, Emby state or running app was changed.

- Native D3D11 capability query (no decoder created): all three exposed H.264
  profiles return three NV12 configurations at 1920x1080, 4096x2160 and
  4096x4096, but zero at 4320x2160. Evidence outside Git:
  `../hdr-diagnostics/h264-capabilities.txt` and its source/command record.
- Production software NV12 upload -> video processor, offscreen synthetic
  gray patches: 4320x2160 -> 4320x2160, 4320x2160 -> 3840x1920 and
  1920x1080 -> 1920x1080. Every TrueHDR request was accepted and blitted.
  Enabled gray white read back as approximately 398-402 nits; the corrected
  disabled/refused path, with SDR white set to 288, read back as 287.075 nits.
  The initial driver-only disabled path had incorrectly yielded 10000 nits,
  which is why the production fallback now performs an explicit conversion.
- ShaderProbe tests the production BGRA fallback at SDR whites 80/288/1000,
  including RGB gamut conversion, Normal/Invert/neutral Vibrance effect order,
  reuse after device rebuild, and exclusion of native PQ/HLG CPU frames from
  the SDR transform. NV12/BGRA upload counters check unchanged-serial reuse.
- Actual production `VideoSource` Automatic on locally generated six-frame
  H.264 Main / 60 fps / 709 limited gray clips: 4320x2160 level 6.0 returns
  CPU yuv420p, 8-bit, original metadata retained, NV12 eligible; 1920x1080
  level 4.2 returns D3D11VA/NV12. Both retained AVFrames remain valid after
  closing the producer. `QuadDeckHdrDecodeProbe <synthetic4320> <synthetic1920>`
  exited 0; `hdr-actual-decode-probe.log` records both PASS results. The larger
  clip's initial hardware-setup diagnostics are the expected Auto fallback.
- Focused Core/SoftwareVideoFrame/ColorConversion/ShaderProbe: 4/4, 1.84 s.
  The native optional command is
  `QuadDeckColorConversionTests.exe --software-rtx-probe` (exit 0).
  Logs live in the isolated validation tree
  `../QuadDeck-codex-rtx-software-hdr/build/validation/` as
  `hdr-focused-tests-final.log` and `hdr-software-rtx-probe-final.log`.

Extension acceptance and synthetic output differences do not establish AI
inference, NVIDIA watermark visibility, the actual samples' display appearance,
multi-pane 4K60 performance or the remaining R12 monitor/recovery checks.
The complete local build and synthetic decoder result were appended to the
branch handoff, retired at `ff5b46d`; existing unrun checklist entries remain open.

The user's sample behind this change (filename not recorded) is H.264 Main
L6.0, 4320x2160, 60 fps, 8-bit 4:2:0, BT.709 limited -- past the 4096x4096
H.264 limit NVIDIA's NVDEC guide gives for AD10x, which is why the synthetic
4320x2160 clip above fell back to the CPU. The comparison file was H.264 Main
L4.2, 1920x1080, 60 fps, colour unspecified. Only their metadata was read;
neither was played.

## Local folder multivideo and wrapped replacement explanation

The desktop, `codex/emby-movies-tv`, follow-up to `c603e67`: the required Windows
Release build and packaging passed with 11/11 CTests (21.91 s), including
the expanded App regression suite (17.47 s). Seven production App/Overlay
WARP readbacks at `build/validation/local-multipane-ui-final/` cover a
three-pane local browser and a five-pane replacement chooser at wide,
300/320/340 DIP and 320 DIP at 2x. The offscreen CLI checks DirectWrite
wrapped-note height/ink and thumbnail/Cancel hit geometry, using synthetic
artwork and filenames. Core also checks nonfinite/stale width/DPI measurement
fallback. Logs: `build/local-full-build-retry.log`, `build/local-ui-fixtures.log`.

Mock state checks cover local/Emby mixed panes, mute/audio-mask preservation,
independent queues and EOF, cancel/stale replacement, delayed directory and
length replies, and adoption of an Explorer-opened single source. No real
media, real server/playback reports, user windows or system settings were
used. This adds narrow UI and orchestration evidence; it does not check off
live decoder/audio throughput, monitor behavior or unrun R13 acceptance.

## Windows single-instance external opens

The desktop, `codex/single-instance`, base `43f6149`, 2026-10-01. The isolated test
receiver uses production named-pipe election/transport and the real App
intake/Add methods with unopened synthetic sources. Its namespace, temporary
files, hidden test HWNDs and child-process handles are private to the test.
It never enters production `App::run`, loads the user's settings/auth, controls
a user window or opens real media/Emby. Test results and exact commands were
in the branch note, retired at `ff5b46d`
(`git show ff5b46d:docs/handoffs/codex-single-instance.md`).

The required full Windows build and packaging passed all 12 CTests in
33.76 s (AppRegression 19.51 s; SingleInstance 9.32 s). The dedicated final
verbose process run passed in 9.54 s, retaining the original live process
and hidden HWND during stopped intake while a secondary timed out after
8,079 ms. Six simultaneous processes recorded one Primary/five Forwarded
and exactly one receiver. Final source hashes and the independent Astra
review are recorded in the handoff. A test-only atomic snapshot publisher
needed bounded retries for observed transient Windows error 5; final failure
remains fatal and no process/window assertion was weakened.

Nine production App/Overlay WARP readbacks at
`build/validation/single-instance-ui-final/` passed the fixture assertions,
including the external full-five explanation at 320 DIP at 1x and 2x. These
check wrapped explanation/Cancel/thumbnail geometry with synthetic artwork.
The App regression checks external deduplication/reveal, batch cancellation,
manual chooser deferral, stable captured queues and startup audio policy.

Live Explorer/default association invocation, native foreground/minimized
restoration, actual decoder/audio continuity and alternate Windows
user/session/integrity rejection remain unrun. The process harness's activation
seam is not evidence of desktop focus behavior. No existing R01-R13 obligation
is closed by this added orchestration/UI evidence. The old binary cannot
forward requests; an isolated candidate must itself be selected by Open with.
The 2 MiB pending-payload, replay-cache and 60-second retention bounds were
reviewed in source but not exhausted at runtime. An accepted request is lost
if the receiving process crashes; delivery is not durable by design.

The desktop, `claude/single-instance-fixes` on `f6c8a9a`, 2026-10-01 (Claude Code).
A batch reaching an empty deck now opens through `loadFiles` and starts
together. Only whole device-name path components are refused. The required
build/package passed 12/12 CTests in 32.73 s (AppRegression 19.57 s;
SingleInstance 9.26 s). Coverage is synthetic only: no real media, user
window, Explorer launch or settings file was used. A scratch harness compiled
against the production `SingleInstance.cpp` on Windows 11 build 26300 accepted
`Con.Air.1997.1080p.BluRay.mkv`, `Nul.Points.mkv` and
`\\nas\media\Aux.Armes.2019.mkv`; the unfixed source refused all three. The
live rows above remain unrun.

## Deferred product decisions and known boundaries

These are not failed acceptance tests and are not authorized implementation
work merely because the register lists them:

- Adjustable dividers: define normalized row/column weights, minimums and
  reset, derive hit regions from activeLayoutCells, suppress pane swap during
  divider capture, define mapping across counts/focus/Solo/layouts, then choose
  persistence versions. Keep this separate from five-pane validation.
- Per-Monitor V2 DPI awareness was declared in eb2160a; R08 now verifies that
  declaration rather than the earlier virtualized behavior.
- A persisted choice between fast interactive seeks and exact synchronized
  seeks is a separate product choice; both current policies must be preserved.
- Per-pane decode mode was asked about, never requested. Automatic falls back
  to software only at open and only for capability, never for throughput;
  `App::decodeMode_` is one global and `changeDecodeMode` reopens every
  source, although `VideoSource::open` already takes a mode per source;
  `decodeThreadBudget` would need the real number of software panes. No mixed
  CPU/GPU throughput was measured. Automatic runtime fallback was advised
  against: a reopen costs a visible seek, and an empty queue does not say
  whether the GPU, the disk or the renderer is short. Codex (2026-08-30)
  agreed it needs an explicit request and a named media test. Full analysis:
  `git show ff5b46d:docs/handoffs/claude-per-pane-audio-and-offset-reset.md`.
- Further extraction of audio-mask and gesture policy follows the App split;
  it is not performed by this documentation task.
- Initial missing audio PTS permits only estimated positioning. BT.2020,
  gamut/transfer conversion and HDR tone mapping remain unsupported. Cached
  immutable files exclude writers; e10c9d6 falls back to direct input on open
  failure. Finite buffering cannot compensate for sustained insufficient bandwidth.

## Retired branch-note map

All filenames below are under `docs/handoffs/` in `5924cf9`. Their integration
requests are complete; remaining verification obligations were transferred,
not waived. Full original narratives, exact old package hashes and log paths
are available with `git show 5924cf9:docs/handoffs/<filename>`.

| Retired note | Surviving evidence / follow-up location |
| --- | --- |
| codex-async-open-responsive-dock.md | September 4 probe/smoke; R01-R06, R08, R10-R11; DPI/divider decisions |
| codex-five-pane-layouts.md | Core capacity/format checks; R01, R03, R08-R09; divider design decision |
| codex-frame-interval-exact-seek.md | Core interval/tick/EOF coverage; R06-R07 |
| codex-nas-read-ahead.md | HEVC-01, September 15 measurements, cache/AVIO evidence; R01, R05, R08, R10-R11 |
| codex-persistence-audio-repeat-hardening.md | Persistence evidence; R04, R09; rollback/path/atomic-replace limits |
| codex-playback-progress-stability.md | Core generations/mapping, release staging; R02-R07, R10 |
| codex-progress-gesture-hardening.md | Core gestures and UTF-8 bounds; R02, R08-R09; fast/exact UI decision |
| codex-responsive-pane-overlays.md | Exhaustive geometry/focus checks; R08; required build script resolved Path/PATH shell conflict |
| codex-review-fixes.md | September 9 media, PCM/shader/colour/native regression evidence; R01, R05-R08, R11; HDR limits |
| codex-stable-pane-identity-audio-reopen.md | Core identity and voice-rebinding evidence; R02-R03 |

The Claude per-pane note was kept at that time, with its Codex cross-check.
No additional implementation or runtime pass was performed during consolidation.

A second retirement on 2026-10-01 (BOARD thread 26, Claude Code) removed the
notes below once their branches were in `master`. Their last text is at
`ff5b46d`: `git show ff5b46d:docs/handoffs/<filename>`. As before, what was
still open moved here and no row was marked passed.

| Retired note | Surviving evidence / follow-up location |
| --- | --- |
| claude-per-pane-audio-and-offset-reset.md | R02 (paused-pane Offset reset); per-pane decode under deferred decisions |
| codex-emby-movies-tv.md | R13 Codex entries; software SDR / RTX HDR and local multivideo sections |
| codex-rtx-hdr-vibrance.md | R12 |
| codex-single-instance.md | Windows single-instance external opens |
| codex-small-app-icon.md | R08 |
