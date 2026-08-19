# Zaro Video — Build Plan

A non-linear video editor in the mold of Adobe Premiere Pro.
C++20 / CMake / Qt 6 / FFmpeg, GPU compositing via Qt RHI.

**Strategy:** build one thin vertical slice that goes all the way through — import →
timeline → trim → realtime playback → one effect → export — and only then widen it
toward Premiere's feature surface. Every phase below ends in something runnable.

---

## 0. Ground truth

Adobe's feature page is JS-gated and could not be fetched, so the feature inventory in
§7 is assembled from Premiere Pro's shipping feature set. Treat it as a checklist to
correct, not as scripture.

### Environment (verified on this machine, 2026-08-17)

| Dependency | Version | Notes |
|---|---|---|
| FFmpeg | 8.1.1 | libavcodec 62.28, libavformat 62.12, libavfilter 11.14, libswscale 9.5, libswresample 6.3 |
| Qt | 6.11.1 | qtbase, qtdeclarative, qtmultimedia, qtshadertools, qtsvg, quicktimeline |
| SDL2 | 2.32.10 | audio output fallback |
| CMake | 4.3.2 | `ninja` **not installed** — `brew install ninja` |
| Compiler | Apple clang 16 | arm64 |
| HW accel | VideoToolbox | h264/hevc/prores encode + decode |

---

## 1. Architecture

Four layers, hard boundaries. The core must never link Qt GUI — that is what keeps the
edit engine testable headless and keeps a future CLI renderer / render farm possible.

```
┌───────────────────────────────────────────────────────┐
│  app/          Qt shell: docking panels, workspaces    │
│                timeline widget, monitors, inspectors   │
├───────────────────────────────────────────────────────┤
│  ui-core/      Widget-level reusable pieces:           │
│                RHI monitor surface, scopes, meters     │
├───────────────────────────────────────────────────────┤
│  core/         NO GUI DEPENDENCY                       │
│    time/       Rational, Timecode, TimeRange           │
│    model/      Project→Sequence→Track→Clip, commands   │
│    edit/       ripple/roll/slip/slide/razor/3-point    │
│    media/      demux, decode, hwaccel, caches          │
│    render/     compositor graph, effect nodes          │
│    audio/      mixer graph, meters, resampling         │
│    io/         project (de)serialization, export       │
├───────────────────────────────────────────────────────┤
│  platform/     FFmpeg, Qt RHI, audio device, fs        │
└───────────────────────────────────────────────────────┘
```

### Non-negotiable design decisions

**1. Rational time everywhere. Never floating-point seconds.**
`Rational{int64 num, int64 den}` for every position and duration, and frame indices for
timeline positions. Floats are how NLEs get one-frame drift, clips that won't butt up
against each other, and audio that slides out of sync over a 40-minute timeline.
Support 23.976 / 24 / 25 / 29.97 DF+NDF / 30 / 50 / 59.94 / 60 and arbitrary rates.

**2. Every mutation is a Command.**
The undo stack is not bolted on later; it is the only write path into the model. A
command carries do/undo, a merge policy (dragging a clip = one undo entry, not 300), and
a description string for the History panel. This also becomes the scripting and
collaboration substrate later.

**3. Source-referencing, non-destructive model.**
A `Clip` is `{source_id, source_range, timeline_start, speed, enabled, effects[],
transitions}`. Media is immutable and referenced by a stable id + hash so relinking,
proxies and consolidation are model-level operations, not file surgery.

**4. Effects are GPU shader nodes, authored once.**
Write GLSL, compile with Qt's `qsb` (qtshadertools is installed), run through QRhi →
Metal today, Vulkan/D3D12 on Linux/Windows later. Do **not** build the effect system on
libavfilter; libavfilter is CPU-bound and its graph model fights per-frame keyframing.

**5. The render graph is pull-based and deterministic.**
`composite(timeline_time) -> Frame` must be pure given the project state. Playback is
then just "call this on a clock", export is "call this as fast as possible", and the
render cache is memoization. One code path, three uses.

**6. Colour is tagged, never assumed.**
Every frame carries primaries / transfer / matrix / range from the moment it is decoded.
Compositing happens in linear or a defined working space. Getting this wrong at the start
means rebuilding the entire pipeline when Log/HDR footage arrives.

---

## 2. Repository layout

```
Zaro-Video/
├─ CMakeLists.txt          top-level, presets, options
├─ CMakePresets.json       macos-debug / macos-release / (later) win, linux
├─ vcpkg.json              manifest — cross-platform reproducibility
├─ cmake/                  FindFFmpeg.cmake, shader compile rules, warnings
├─ core/
│  ├─ time/  model/  edit/  media/  render/  audio/  io/
│  └─ tests/               Catch2, headless, runs in CI
├─ shaders/                *.frag/*.vert → qsb bundles
├─ ui-core/
├─ app/
│  ├─ panels/              bin, timeline, monitors, effect controls, audio
│  └─ main.cpp
├─ tools/
│  ├─ zaro-probe           dump media metadata
│  ├─ zaro-frame           extract frame N → PNG  (Phase 1 proof)
│  └─ zaro-render          headless project → file (Phase 3 proof)
├─ docs/                   this plan, ADRs, format specs
└─ testdata/               small generated clips, checked in
```

Dependency policy: `find_package` against Homebrew Qt/FFmpeg for local dev now; a
`vcpkg.json` manifest kept in sync from day one so Windows/Linux is a build config, not a
port. Never `#include <libav*>` outside `core/media` and `core/io`.

---

## 3. The vertical slice (Phases 0–4)

Target: a person can import footage, cut it, watch it play back in sync, add a dissolve,
and export a file. This is the whole point of the plan — everything in §7 is deferred
until this works.

### Phase 0 — Foundations ✅ **complete**
*Goal: the repo builds, tests run, and time arithmetic is provably correct.*

- ✅ CMake + presets (`debug` / `release` / `asan`) + ninja + warnings-as-errors,
  clang-format, clang-tidy.
- ✅ Catch2 test target, GitHub Actions running the headless suite on macOS and Linux.
- ✅ `Rational`, `RationalTime`, `TimeRange`, `Timecode` (incl. drop frame at 29.97 /
  59.94 / 119.88).
- ✅ `ZARO_CHECK` assertion policy — invariants that must not be compiled out of release.
  A proper `std::expected`-style error type is deferred to Phase 1, where the first real
  fallible operations appear.

**Done when:** `ctest` passes a timecode suite that includes drop-frame round-trips at
29.97 and 59.94 across a 24-hour range, and rate conversions between 23.976 and 25.

**Result:** 35 tests green across all three presets. The drop-frame implementation is
additionally cross-checked against FFmpeg's `av_timecode_make_string` — an independent
implementation of the same SMPTE rules — and matches on all 2,589,408 labels in a
24-hour day at 29.97, plus full sweeps at 59.94 DF, 29.97 NDF, 25 and 23.976. See
[ADR-001](adr/0001-rational-time.md).

### Phase 1 — Media I/O ✅ **complete**
*Goal: get correct frames and samples out of real files, fast.*

- ✅ `MediaProbe`: streams, duration, rate, colour tags, rotation/display matrix, channel
  layout, start timecode.
- ✅ `Decoder`: libavformat/libavcodec, VideoToolbox hwaccel with software fallback,
  frame-exact seek. Random access goes through a **conform index** — a packet scan that
  learns every frame's exact timestamp — because containers misreport frame counts and
  VFR footage has no arithmetic relationship between index and time.
- ✅ `VideoFrame` (CPU planes, always colour-tagged, move-only), `AudioBuffer`
  (planar float32).
- ✅ Audio: decode + `swresample` to a canonical float32 planar working format.
- ⏸️ **Deferred: thumbnail and waveform generation with a content-hashed disk cache.**
  Nothing consumes them until the bin and timeline exist, and doing the cache properly
  — content hashing, eviction, invalidation, background scheduling — is its own chunk of
  work that belongs next to its consumer. Moved to the head of Phase 4.

**Done when:** `zaro-frame movie.mov 1234 out.png` is frame-exact against
`ffmpeg -vf select` for H.264, HEVC, ProRes, and a VFR phone clip; and a 4K ProRes file
decodes above realtime through VideoToolbox.

**Result:** 65 tests green across `debug`, `release` and `asan`.
`scripts/verify-frame-exact.sh` compares raw decoded planes against FFmpeg's own decoder
in each file's native pixel format: **46 frames byte-identical** across ProRes 10-bit,
H.264, HEVC, VFR, 29.97 drop-frame, and a mixed A/V clip. 4K ProRes decodes at **7.5x
realtime**.

Two bugs worth recording, both found by the fixtures rather than by reading the code:

1. **Seeking landed one frame late in long-GOP codecs.** MP4 and MOV index keyframes by
   *decode* timestamp, but a stream with B-frames presents out of decode order, so the
   keyframe indexed before a target can present *after* it — and decoding forward from
   there never reaches the requested frame. Fixed with a doubling seek backoff.
2. **Asking for the same frame twice returned its successor**, because already-consumed
   frames cannot be reached by decoding forward and the position check used `<` where it
   needed `<=`.

The first was initially masked by a test whose tolerance was wider than the fixture's
frame-to-frame difference. The ladder fixture now steps four code values per frame,
comfortably outside lossy-codec noise, so a one-frame error cannot hide inside the
tolerance.

### Phase 2 — Model + edit engine (headless) ✅ **complete**
*Goal: the whole editing brain, with zero pixels.*

- ✅ `Project / Sequence / Track / Clip / MediaRef`, ids stable across save/load and
  phantom-typed so a `ClipId` cannot be passed where a `TrackId` belongs.
- ✅ Command stack: do/undo/redo, merging, bounded depth, History descriptions.
  Undo restores a snapshot rather than computing an inverse — see
  [ADR-004](adr/0004-snapshot-undo.md).
- ✅ Edit operations, each with tests: overwrite, insert (+ripple, incl. all-track sync),
  razor, lift, extract, ripple delete, trim, ripple trim, roll, slip, slide, move
  (including across tracks), add/remove track. Snapping is a pure function over the
  sequence, with a duration threshold rather than a pixel one, so it stays independent of
  zoom.
- ✅ Serialization: versioned JSON, with unknown fields preserved through a load/save
  cycle and matched by id rather than array position.
- ⏸️ **Deferred: three- and four-point editing, linked A/V selection, sync locks.** All
  three are defined by interactions that do not exist yet — a source monitor with in/out
  points, a selection model, a track-header control. Building them now would mean
  inventing the semantics twice. They move to Phase 4 alongside the UI that gives them
  meaning.

**Done when:** a headless test constructs a 20-edit sequence, undoes every step back to
empty, redoes to the end, saves, reloads, and asserts byte-identical model state.

**Result:** 113 tests green across `debug`, `release` and `asan`. The exit-criterion test
asserts the state after *every one* of the 20 edits, walks undo back through all 20
recorded states to an empty timeline, redoes forward through the same states, and
round-trips through JSON. A fuzzer throws 6,000 randomly-parameterised operations at the
model across 40 seeds, checking after each one that no clips overlap, no duration is
negative, no clip reads past the end of its source, and no id is duplicated — and that a
*refused* edit left the model completely untouched.

Three bugs, all found by tests rather than by reading the code:

1. **A clip was invisible on its own first frame.** `Track::clipAt` used `lower_bound`,
   which stops *at* a clip starting exactly on the query time; stepping back from there
   lands on the previous clip. The compositor would have asked for a frame at a cut and
   been told there was nothing there.
2. **Ripple trim corrupted the track when shortening.** It shifted the following clips
   before replacing the trimmed one, so the track passed through a state where two clips
   overlapped. Whichever order is chosen, one direction breaks — so the fix was to
   rebuild the track in a single pass and have no intermediate state at all.
3. **Media duration was silently lost on load**, because it was written as a rational
   string and read back as a `{frames, rate}` object. Every trim bound disappeared the
   moment a project was reopened. Caught by asserting that re-saving a loaded project
   produces byte-identical output — a much sharper check than comparing models.

### Phase 3 — Compositor + playback
*Goal: the hard part. Frames on screen, audio in sync, scrubbing that feels alive.*

Split in two, because the deterministic half is verifiable headlessly and the realtime
half is not. `composite(t)` being pure is what makes the split clean: export is that
function called as fast as possible, playback is the same function on a clock.

#### Phase 3a — the render graph and export ✅ **complete**

- ✅ Colour working space decided and implemented: scene-linear, Rec.709 primaries,
  float RGBA, premultiplied alpha ([ADR-005](adr/0005-working-colour-space.md)).
- ✅ Clip transform node: position / scale / rotation / anchor / opacity, blend modes,
  and track blending bottom-up. Inverse-mapped and bilinear-sampled, so the shader port
  produces the same picture.
- ✅ `RenderGraph::composite(t)` — pure and deterministic given a `FrameSource`.
- ✅ Audio graph: per-clip gain and pan → per-track gain and pan → mix, sample-accurate.
- ✅ Frame cache: strict LRU with a budget **in bytes**, because a working-space frame is
  8 MB at 1080p and 33 MB at 4K — a cache sized in entries is one that works until
  someone opens UHD footage.
- ✅ `zaro-render`: headless project → mov/mp4, and `zaro-cut` to build a project from
  media so the path can be exercised without a UI.
- ⏸️ **Not done: the QRhi shader path.** The CPU implementation here is deliberately
  first, because it is the oracle a GPU renderer's golden-frame tests compare against.
  A GPU renderer with no independent reference is one nobody can prove anything about.

#### Phase 3b — realtime playback ✅ **engine complete**, GPU path outstanding

- ✅ **Playback engine**, with the audio device's consumed-sample count as the master
  clock ([ADR-006](adr/0006-audio-is-the-clock.md)). Bounded frame queue, explicit drop
  policy, catch-up that skips the backlog rather than working through it, seek, and
  continuous speed changes.
- ✅ JKL shuttle with a rational speed ladder — rational because speed multiplies into the
  position mapping, and 1/3 as a double puts the playhead visibly out within a minute.
- ✅ Lock-free SPSC ring buffer feeding a real SDL2 audio device, and `zaro-play`, which
  plays a project against it and reports sync.
- ⏸️ Outstanding: scrub-request coalescing, and a preview window.

#### Phase 3c/3d — the GPU compositor ✅ **complete**

- ✅ Compositor on QRhi: one GLSL source compiled by `qsb` for Metal, Vulkan and D3D,
  transform and opacity as a uniform, blend modes as pipeline state
  ([ADR-007](adr/0007-gpu-compositor-on-qrhi.md)).
- ✅ Golden-frame tests against the CPU reference, covering scale, position, opacity,
  rotation in both directions, and all four blend modes.
- ✅ **Phase 3d: the YUV texture path.** The decoder's planes are uploaded as they are and
  the colour conversion happens on the GPU.

At 1080p, compositing alone, the GPU was *slower* than the CPU — 54 fps against 63 —
because each frame crossed the bus as 8 MB of float RGBA and came back as another 8 MB.
The stages did not add up either: decode at 1840, conversion at 103 and compositing at 63
predict 38 fps in series against a measured 9, the rest being per-frame traffic — clearing,
cloning into the queue, caching — each moving another 8 MB. **The pipeline was
memory-bandwidth bound on float RGBA frames, not compute bound.**

Uploading planes instead, and converting on the GPU, changes the picture:

| Path (1080p, convert and composite) | Throughput |
|---|---|
| CPU | ~40 fps |
| GPU, result stays on the GPU | **520–590 fps** |
| GPU, result read back | ~95 fps |

The spread on the middle row is real: the run is short enough that command submission
dominates, so it varies between repeats. The other two are steady to within a frame or
two. What matters is not the exact figure but the shape — the readback costs more than
everything else in the pipeline put together.

Playing a 1080p59.94 timeline for real, over fifteen seconds: the CPU path presents 101
frames and drops 664, with picture lagging the clock by three. The GPU path presents 701
and drops 198, with an offset of zero and no audio underruns — seven times the frames,
and in sync rather than merely close.

It is still not presenting all 59.94. The remainder is the readback, which `zaro-play`
does because it has nowhere to put a texture, and the single-threaded render loop. A
preview window removes the first and Phase 4 addresses the second.

**The conversion needs its own pass**, which was not obvious. Folding it into the sampler
matched the reference exactly at 1:1 and diverged under scaling, because a bilinear filter
applied to encoded Y'CbCr interpolates in gamma space — the same error
[ADR-005](adr/0005-working-colour-space.md) rejects for blending, reappearing in the
sampler. Converting into a linear surface first costs one GPU pass and no bus traffic.

**Done when:** a 3-clip sequence plays at 1080p59.94 with locked A/V sync for 10 minutes,
scrubbing stays responsive, and the exported file's audio drift is 0 samples end-to-end.

**Result so far:** 256 tests green across `debug`, `release` and `asan`. The export half
of the criterion is met and measured, not asserted: `scripts/verify-av-sync.sh` renders
the flash-and-click fixture, then extracts picture and sound from the *output file*
independently and compares them — **0 samples of drift over 250 frames, with all 10
flash/click pairs aligned to within 1 sample (0.02 ms)**.

Audio is addressed by an exact rational relationship to the frame number rather than by
adding a per-frame duration to a running total. At 29.97 there are 1601.6 samples per
frame; accumulating that as a rounded integer drifts by a sample every few frames and by
a visible lip-sync error over an hour.

One bug worth recording, found by that harness and invisible to every other check:
**every export was one frame short.** `prores_ks` does not set packet durations, so the
muxer inferred each packet's duration from the gap to the next — and the final packet has
no next. It landed in the file with a duration of zero: present in the sample index, so
the container reported the right frame count, but outside the stream's declared duration
and undecodable. The file looked complete and decoded one frame short.

#### Phase 4a — the preview window ✅ **complete**

- ✅ `ProgramMonitor`, a `QRhiWidget` whose compositor **adopts the widget's own GPU
  device**, so the composited texture is drawn straight to the screen and never touches
  system memory. This is the readback that Phase 3d measured as the difference between
  roughly 550 fps and 95.
- ✅ Transport: space, J/K/L shuttle, arrow-key frame stepping, home/end, a scrubber, and
  a drop-frame-aware timecode readout.
- ✅ Playback against the audio clock, with audio on its own thread ([ADR-006](adr/0006-audio-is-the-clock.md)).
- ✅ `presentToImage`, which is both what a thumbnail wants and what makes the
  presentation path testable without a window.

Two things the compositor needed for this, both worth noting because they are not
obvious from the offscreen case:

- **A frame it does not own.** A widget has already opened a GPU frame by the time it
  asks for a picture, so `beginFrameOn` records into the caller's command buffer rather
  than opening a second one.
- **A device it does not own.** Textures cannot cross `QRhi` instances, so the compositor
  adopts the widget's device instead of creating its own.

**The preview rendered vertically flipped**, and the self-test that counted lit pixels
reported 96.8% either way. It was caught by comparing a captured frame against a
reference extracted through the byte-exact `zaro-frame` path — the timecode burn-in had
moved from the top to the bottom. The cause is that the composited texture is written
with row 0 as the top of the picture while the present quad maps texture V=0 to the
bottom of clip space, which flips on Y-down backends (Metal, Vulkan, D3D) and not on Y-up
ones (OpenGL). There is now a headless test that renders a deliberately asymmetric frame
and checks which end it comes out at, verified to fail when the correction is removed.

#### Phase 4b — the timeline panel ✅ **first cut**

- ✅ `ui::TimelineLayout` — a **toolkit-free** layer holding all the geometry: time↔pixel
  mapping, zoom anchoring, row layout, hit-testing and culling. Thirteen test cases cover
  it, none of which need a window. The widget on top is then mostly painting.
- ✅ `TimelineWidget`: ruler with drop-frame-aware timecode, track headers with mute and
  lock indicators, clips culled to the visible range, playhead that follows playback.
- ✅ Interaction: scrub, select, drag-to-move with snapping, razor at the playhead,
  lift and extract, undo/redo, zoom and scroll. **Every edit goes through the command
  stack**, so all of it is undoable by construction rather than by remembering to make it
  so — and a whole drag collapses to one undo step via the merge key.
- ⏸️ Outstanding: trim by dragging clip edges (the hit-testing distinguishes the edges
  already, the interaction is not wired), waveforms and thumbnails, the project bin,
  effect controls.

Three bugs, all caught by the layout tests before any of it was painted:

1. **`timeForX` rounded instead of flooring**, so a click near the right edge of a clip
   resolved past its exclusive end and hit nothing. The same bug class as the playback
   clock's `positionAt`, for the same reason: a pixel sits *inside* a frame for that
   frame's whole width.
2. **The visible range rounded rather than ceiling**, leaving a half-frame sliver
   unpainted at the right edge — wrong for a culling range, which must over-cover.
3. **A short clip was all trim handle and no body**, so it could never be selected or
   dragged. Each grab zone is now capped at a third of the clip's width.

And one caught only by looking: `zoomToFit` ran before the widget had been laid out, so it
fitted to a width the widget did not yet have. Deferred to the first resize.

#### Phase 4c — trimming and waveforms ✅ **complete**

- ✅ **Trim by dragging clip edges**, with Alt for ripple trim. Trims are expressed as
  deltas, so each mouse move is measured against where the edge *actually landed* rather
  than where the pointer wanted it — a clamped trim would otherwise accumulate the
  difference over a drag.
- ✅ **Waveforms**, the last thing carried forward from Phase 1. Peaks are min/max per
  bucket, not averages: averaging a symmetric signal gives zero everywhere and draws a
  flat line. Bucket boundaries are counted across calls, so a waveform is identical
  however the decoder happened to chunk its output.
- ✅ **The content-hashed disk cache**, also deferred from Phase 1. Keyed on size,
  modification time and a sample of the bytes at each end — deliberately *not* a hash of
  the whole file, because hashing a hundred gigabytes of camera media on import would
  make importing unusable, and the cost of being wrong is a stale waveform rather than a
  corrupted project.
- ✅ Generation runs on a background thread; a project that freezes while it opens is
  worse than one whose waveforms arrive a moment late.

Two bugs found along the way, neither in the new code:

- **`ProjectIo` never serialized audio stream info**, so a loaded project silently had no
  audio metadata at all. It now round-trips. The waveform generator was also changed to
  try every media reference rather than only those the project file *claims* have audio:
  that cached info is a cache, and gating work on it means a missing field becomes a
  missing feature.
- **The preview self-test asserted the monitor was at least 5% lit at one timecode**, which
  is fixture-dependent — the click fixture is legitimately black except on flash frames,
  and the check failed on correct output. It now samples across the sequence and keeps the
  brightest.

I also twice reported a clean build from a stale binary, because the error grep matched
`error:` and missed `AutoMoc error`. Broadened.

#### Phase 4d — effect controls ✅ **complete**

The transform and opacity parameters have existed in the model and the compositor since
Phase 3a and were unreachable from the UI. They are now editable.

- ✅ Four new edit operations — `makeSetTransform`, `makeSetBlendMode`, `makeSetClipAudio`,
  `makeSetClipEnabled`. They cannot move a clip or collide with a neighbour, but they are
  commands anyway, because the command stack is the only write path into the model: an
  "obviously safe" mutation that bypassed it would be the one operation undo did not
  cover.
- ✅ Merge keys, so dragging a value is one undo step rather than one per pixel — while
  toggling *enabled* deliberately does **not** merge, because two toggles are two
  decisions rather than one gesture.
- ✅ An Effect Controls panel driven *from* the model rather than holding its own copy: it
  re-reads after every edit and after undo. A panel that trusted its own widgets would
  drift the first time something changed the clip from elsewhere.

Verified end to end rather than by inspection: the self-test renders a frame, lowers
opacity to 0.15 through the same command path the panel uses, renders again, and requires
the picture to have darkened. Mean brightness goes 107.0 → 15.5. If the panel and the
compositor were not connected, that check fails.

One UI bug worth recording: with nothing selected, the disabled panel showed **Scale 0.000
and Opacity 0.000**. Those are meaningful values — a clip scaled to nothing — so a panel
displaying them reads as a broken clip rather than as no selection. The empty state now
shows the identity transform.

#### Phase 4e — transitions ✅ **complete**

The plan's Phase 4 asks for "cross-dissolve transition + the transform effect wired end to
end". The transform half landed in 4d; this is the other.

- ✅ A `Transition` type on the track, spanning a range that **straddles** the cut. The
  clips stay adjacent and never overlap, so every invariant the track already enforces
  still holds and removing a transition needs no decision about where the clips should go.
- ✅ During the span, the outgoing clip is read *past* its out point and the incoming one
  *before* its in point, reaching into the handles either side. `sourceTimeAt` already
  extrapolates linearly, which is exactly the mapping wanted.
- ✅ A dissolve is refused where there are no handles, rather than silently shortened or
  filled with black — and refused where there is a gap rather than a cut.
- ✅ Both render graphs, CPU and GPU, with a golden test asserting they agree across a
  dissolve. They are separate traversals, and a preview that disagrees with the export is
  the worst kind of disagreement because it is only found after delivery.
- ✅ Serialization, Cmd+D to add one at the playhead, and the diagonal every editor draws.

One latent bug surfaced while adding this: **track `gainDb` and `pan` were never
serialized.** The decoder read them, the encoder never wrote them, and no test had set a
track gain and round-tripped it — so a mix would silently flatten on save. The same shape
as the audio-stream-info gap found in 4c, and worth noting as a pattern: an encoder and a
decoder written at different moments drift, and only a round-trip test with the field
actually set will catch it.

#### Phase 4f — project bin and export ✅ **complete**

- ✅ **A project bin**: media with size, duration and an audio marker; import through a
  file dialog; append to the timeline. Importing is undoable, which needed a
  `ProjectCommand` — `SequenceCommand` snapshots one sequence, and media lives on the
  project. The alternative was to let imports bypass the command stack because they are
  "only additive", which is how a write path that undo does not cover gets established.
- ✅ **An export dialog**, with progress, cancellation, and a partial file deleted rather
  than left looking like a delivery.
- ✅ **`renderSequence` extracted** so `zaro-render` and the export dialog run the same
  code. Two loops doing this would have to be kept agreeing, and the one nobody runs is
  the one that drifts. Verified by re-running the A/V sync check through the extracted
  path: 250 frames encoded, 250 packets written, 0 samples of drift, unchanged.

Before any of that, I acted on the concern raised at the end of 4e — that the round-trip
tests could not catch an encoder gap when the decoder defaults to the same value, having
found two such bugs in two phases. There is now a test that sets **every** serializable
field to a distinctive non-default value and checks each one individually after a round
trip. It found nothing further, which is the useful result: the two known gaps were the
only ones, and a third cannot now reach a project file unnoticed.

#### Phase 4g — source monitor and three-point editing ✅ **complete**

Three- and four-point editing were deferred from Phase 2 with the reasoning that they are
defined by interactions that did not exist yet. Those interactions exist now.

- ✅ **A source monitor**, which is a sequence with a single clip in it. That is not a
  shortcut — it is what a source monitor *is* — and it means the program monitor renders
  both, rather than there being a second render path to keep in agreement with the first.
- ✅ **In and out marking**, with the out point inclusive to the eye and exclusive in the
  model, and unmarked ends falling back to the whole media so that marking only an in
  point means "from here to the end".
- ✅ **Three-point editing**: `makePlaceFromSource` derives the duration from the marked
  range rather than taking it separately, which is the whole point of counting to three.
  It converts between the media's frame rate and the sequence's — twenty-four frames of a
  24fps take is twenty-five frames on a 25fps timeline, and copying the count across would
  put every edit assembled from mixed-rate media a frame short per second.
- ✅ **A keymap table** replacing the hardcoded switch. The bindings are the thing a user
  wants to see and eventually change, and a list of them reads as documentation.

Verified end to end through the widgets rather than by calling the operation: the
self-test loads media into the source monitor, steps, marks in and out, and places the
result — 49 source frames marked, 49 frames placed at the playhead.

#### Phase 4h — linked A/V and sync locks ✅ **complete**

The last two deferrals from Phase 2. Both were held back on the grounds that they are
defined by interactions that did not exist; both now have a selection model and track
headers to hang on.

- ✅ **Link groups.** A `LinkId` on the clip: clips sharing one are moved, trimmed and
  removed together. Picture and its sound arrive together and should stay together —
  dragging one and leaving the other is how a cut goes out of sync without anyone
  noticing. `zaro-cut` now links the pairs it creates, and the timeline outlines the whole
  group when one of them is selected.
- ✅ **Sync locks**, distinct from track locks. A locked track refuses all editing; a track
  with sync lock off can still be edited directly but stays put when something else
  ripples, which is how a music bed is kept from sliding every time picture is trimmed.

Two decisions worth recording, both about what to do when part of an edit cannot happen:

- **A locked track keeps its clips where they are, even when they are linked to something
  that moves.** Refusing the whole edit instead would let one locked track block editing
  everywhere it happened to be linked. The same applies to a linked clip whose trim would
  run out of source: it is left alone rather than blocking the clip actually being
  dragged.
- **Sync lock governs whether a track takes part at all, not merely whether it shifts.**
  The first version cleared the range from every track and then skipped the shift for
  unlocked ones, which left them half-edited — material removed and the gap left open.
  Doing all of it or none of it is the only coherent choice, and a test now says so.

Every operation is link-aware unconditionally, so an unlinked clip behaves exactly as it
did before links existed. The forty-five pre-existing edit tests passing unchanged is what
made that claim checkable rather than hopeful.

#### Phase 4i — markers and workspaces ✅ **mostly**

- ✅ **Markers**, carrying a duration rather than being points — most of what people mark
  is a span, and a point is a span of one frame. Modelling only the point case would mean
  discovering that later and migrating every project file. A zero duration is stored as
  one frame, because a genuinely empty range would answer "no" to containment everywhere,
  including at the marker itself.
- ✅ Drawn in the ruler, added with M, and navigated with shift-arrow.
- ✅ **Workspaces**: the panels are splitters now rather than a fixed layout, and their
  sizes and the window geometry are remembered between sessions.
- ✅ **Multi-selection** — deferred out of this phase and done in 4j, below.

**A third id bug, of a kind the round-trip tests could not see.** `highestId` — which
restarts the id counter after loading — never learned about markers, transitions or link
groups. Each was added in a later phase and the loader was not updated. The result is not
a field failing to round trip; it is a file that loads perfectly and then hands out an id
already in use, so the next marker created silently *is* an existing one.

The exhaustive field test added in 4f would never have caught this, because it never
issues new ids after loading. There is now a second test that does exactly that — loads a
project containing every kind of id and checks that fifty freshly issued ids collide with
none of them — and it was verified to fail with the fix reverted.

That is three bugs of the same shape: a new thing is added to the model, and one of the
places that has to enumerate everything is missed. The encoder, the decoder, and the id
counter are all such places.

### Phase 4 — The application
*Goal: the slice becomes a program someone can actually use.*

- Qt shell with `QDockWidget` panels and saveable workspaces (Premiere's model).
- **Project bin:** import, thumbnails, metadata columns, hover-scrub.
- **Source monitor:** in/out marking, drag-to-timeline.
- **Program monitor:** `QRhiWidget` surface, transport, safe margins, quality toggle.
- **Timeline panel:** the single biggest UI investment — custom-painted, virtualized
  drawing, waveform/thumbnail rendering, zoom + scroll, snapping, drag/trim affordances,
  ripple/roll/slip/slide tools, track headers with mute/solo/lock, playhead.
- **Effect Controls:** parameter editing + a first keyframe UI.
- Cross-dissolve transition + the transform effect wired end-to-end.
- Export dialog over the Phase 3 render path (H.264 / ProRes presets).
- Keymap layer from day one — Premiere-compatible default bindings.

**Done when:** a real 5-minute edit is cut, dissolved, and exported without touching a
CLI, and the app survives a 1-hour session without leaking or desyncing.

---

### Phase 5 — beyond the slice

§6's dependency graph puts the keyframing engine at the root: motion, effects,
masks and audio automation all hang off it, and the colour work is much cheaper
once a parameter can animate. So that is where Phase 5 starts.

#### Phase 5a — the keyframing engine ✅

`Curve` is a sorted list of keyframes and the rule for reading a value between
them: hold, linear, or cubic bezier with a handle at each end. `ClipAnimation`
maps a parameter to a curve, and a clip holds one. Position, scale, rotation,
anchor, opacity, clip gain and pan are all animatable.

**Keyframes are in source time** — [ADR-008](adr/0008-keyframes-in-source-time.md).
Stored in sequence time they are left behind by a ripple; stored relative to the
clip's start they slide against the picture whenever the head is trimmed. In
source time the clip can be moved, trimmed, razored and rejoined and the fade
still lands on the frame it was set on.

**Evaluated in seconds, not frames.** A 24fps clip on a 60fps timeline sampled
in source frames holds each value for two or three output frames — a smooth move
becomes a stutter no keyframe accounts for. A test requires all sixty output
frames to carry distinct values, and it fails with 24 when evaluation is moved
back to quantised frames.

**Held outside the keyframed range, never extrapolated.** Continuing the slope
off the last keyframe produces opacities of nine a few seconds later. Holding is
also what makes a single keyframe mean something: it pins a constant.

**Audio automation is per sample** — [ADR-009](adr/0009-automation-per-sample.md).
A gain held constant across a block steps at the block boundary, and that
boundary belongs to the audio device rather than to the edit. The test renders
the same automated clip in one block and in blocks of 64, 128, 512 and 1000 and
requires bit-identical output; it fails on all four when evaluation is moved
back to once per block.

**Bezier handles that reach past each other are scaled down until they meet.**
Handles longer than the segment describe a curve that doubles back, and a
parameter cannot have two values at one instant. Scaling both preserves their
ratio, so the drawn shape survives as nearly as a function can. Overshoot from a
*vertical* handle is left alone: landing a move with a bounce is a legitimate
thing to ask for, and only a curve that is not a function of time is a bug.

**A curve exists only for a parameter that is animated.** The static value stays
authoritative until one does, so nothing is paid in storage, serialization or
evaluation for the overwhelming majority of clips that are not animated.

**The GPU compositor needed the same change as the CPU one**, and nothing in the
headless tests would have caught it: those exercise `render::RenderGraph` only,
so a curve honoured on export and ignored in preview would have looked fine. The
preview self-test now sets a fade and measures it through the real GPU path
(213 lit → 106 halfway → 0 faded), and it was verified to fail when the GPU
traversal is reverted to the static transform.

Writing that check found two things about the test rather than the code: the
fixture is black except on its flash frames, so a fade has to be measured
against a frame that is lit to begin with, and the brightest frame was the
clip's first — which gave the ramp zero length and collapsed both keyframes onto
one instant. The dedup in `Curve::set` was working; the test was asking for a
fade with no duration.

Still to do before keyframing is usable from the UI: the Effect Controls panel
needs stopwatch toggles and a keyframe lane, and the timeline needs to draw and
drag keyframes. The engine, the serialization and both render paths are done.

---

## 4. Effort and risk, stated plainly

Premiere Pro is roughly three decades of work by a large team. Feature parity is not a
realistic target for a small team; a genuinely good editor for a *chosen* workflow is.

| Milestone | Rough effort (1 experienced dev) |
|---|---|
| Phases 0–2 | 6–10 weeks |
| Phase 3 (playback/compositor) | 8–14 weeks — the schedule risk lives here |
| Phase 4 (app shell + timeline UI) | 10–16 weeks |
| **Usable vertical slice** | **~6–9 months** |
| Colour + audio + keyframing (§7.1–7.3) | +9–15 months |
| Anything resembling parity | many engineer-years |

**Top risks**

1. **A/V sync and dropped-frame policy.** Playback is where NLEs actually die. Budget the
   time, build a sync test harness in Phase 3, measure every commit.
2. **Codec licensing.** x264 is GPL and H.264/HEVC carry patent obligations for a
   distributed product; VideoToolbox encoders sidestep this on Apple platforms. Decide the
   distribution story *before* Phase 4 export, not after.
3. **Timeline UI scope.** A professional timeline is not a widget, it is a subsystem —
   virtualization, hit-testing, drag semantics, and a dozen tool modes.
4. **Colour management retrofit.** Cheap in Phase 1, brutally expensive in Phase 8.
5. **Frame cache memory.** 4K RGBA float is ~32 MB/frame. The cache needs a real eviction
   policy and a hard budget from the first version.

---

## 5. Testing strategy

- **Headless-first.** `core/` has no GUI dependency, so edit ops, time math, and the
  render graph are all unit-testable in CI.
- **Golden-frame tests.** Render fixed timecodes from a checked-in project, compare
  against reference PNGs with a perceptual delta threshold.
- **Sync harness.** Render a clip with an audible click and a white flash on the same
  frame; assert alignment in the exported file automatically.
- **Fuzz the edit engine.** Random sequences of edit commands, then assert model
  invariants: no overlapping clips on a track, no negative durations, undo returns to the
  exact prior state.
- **Performance gates in CI.** Decode throughput and composite time per frame, tracked
  per commit — regressions here are invisible until they are catastrophic.

---

## 6. Sequencing after the slice

Rough dependency order once Phase 4 lands:

```
Keyframing engine ──┬─→ Motion / effects  ──→ Masks + tracking
                    └─→ Audio automation
Colour management ──┬─→ Scopes ──→ Primary/curves/secondaries ──→ LUTs, HDR
Render cache ───────┬─→ Background render ──→ Proxies ──→ Multicam
Text/shape layers ──→ Graphics templates ──→ Captions
Speech-to-text ─────→ Text-based editing ──→ Scene edit detect, auto-reframe
```

---

## 7. Feature inventory (Premiere parity checklist)

Reconstructed from Premiere Pro's feature set — correct anything that's wrong or missing.
Nothing here is scheduled until the vertical slice ships.

### 7.1 Editing
Source/program monitors · three & four-point editing · ripple, roll, slip, slide · razor ·
lift/extract · insert/overwrite · linked A/V · sync locks · track targeting · snapping ·
markers (clip, sequence, chapter) · nesting · adjustment layers · multicam (sync by
timecode/audio/marker, live switching) · speed/duration + time remapping with ramps ·
freeze frame · scene edit detection · subclips · match frame · replace clip/footage ·
sequence presets · timeline customization · sync/lock groups

### 7.2 Audio
Audio track mixer & clip mixer · clip gain + keyframed volume · pan/balance ·
Essential Sound style roles (dialogue/music/SFX/ambience) · auto-ducking · loudness
normalization (EBU R128) · EQ, compressor, limiter, de-noise, de-reverb · Enhance Speech ·
audio meters + peak hold · submixes and sends · channel mapping · multichannel/5.1 ·
waveform display · audio-only scrubbing · sample-accurate editing

### 7.3 Colour
Lumetri-equivalent stack: basic correction, creative looks, curves (RGB + hue/sat),
colour wheels, HSL secondaries, vignette · scopes: waveform, vectorscope (YUV/HLS),
histogram, RGB parade · LUT support (.cube in/out) · log→display transforms ·
colour management / working spaces (OCIO) · HDR (HLG/PQ) · auto colour + shot matching ·
comparison view

### 7.4 Effects, motion, graphics
Keyframing engine with bezier/hold/linear interpolation and velocity curves ·
Motion (position/scale/rotation/anchor/opacity) · blend modes · masks (shape + bezier)
with feather and mask tracking · blur/sharpen/distort/stylize library ·
chroma & luma keying · warp stabilizer equivalent · lens distortion · transitions
(dissolve, wipe, slide, morph cut) · Essential Graphics equivalent: text and shape layers,
styles, responsive design (pin to video / intro-outro), motion graphics templates ·
captions/subtitles with styling and burn-in · titles and rolling credits

### 7.5 Media, performance, workflow
Media browser · proxy workflow (create, attach, toggle) · smart rendering ·
render cache + background rendering · GPU acceleration · media relink and consolidate ·
transcode on ingest · metadata and search · production/bin organization ·
project versioning and autosave · shared projects with locking · comments/review ·
frame.io-style review integration

### 7.6 Assistive / AI
Auto transcription and speech-to-text captions (Whisper is the obvious local path) ·
text-based editing (edit by deleting words) · filler-word removal · auto-reframe for
vertical/square · remix (music retiming to length) · scene edit detection ·
object/segment masking · auto tone mapping

### 7.7 Interchange and export
Export presets and queue · H.264/HEVC/ProRes/DNx/AV1 · alpha/RGBA export · image
sequences · audio-only · social presets · watch folders · EDL, AAF, XML, **OpenTimelineIO
(highest leverage — get this early, it unlocks round-tripping with every other NLE)**

---

## 8. Immediate next steps

1080p59.94 now plays in sync on the GPU. What remains before Phase 4 is a preview window,
and the readback that still sits between the compositor and the scheduler's queue.

1. **A GPU-resident frame in the playback queue.** Preview currently reads each composited
   frame back to the CPU because the scheduler's queue holds `RgbaImage`. Removing that is
   worth 93 → 314 fps and is exactly what a preview window needs anyway.
2. **The preview window itself**, on the same QRhi device, which is where Phase 4 begins.
3. **The `DecodeMode::Auto` revisit** ([ADR-003](adr/0003-hardware-decode-readback.md)).
   The compositor now takes planes rather than converted pixels, so hardware decode's
   output has somewhere to go without a readback. This is finally the right time.
4. **Scrub-request coalescing.**

Carried forward as known work:

- Thumbnail and waveform generation with a content-hashed disk cache (Phase 1 → head of
  Phase 4).
- Three/four-point editing, linked A/V selection, sync locks (Phase 2 → Phase 4, with the
  UI that defines them).
- Chroma siting: both paths take the nearest chroma sample. Proper siting and
  interpolation is a quality improvement that should change them together.
- Audio at shuttle speeds other than 1x plays silent; it needs pitch handling.
- HDR: PQ and HLG are recognised and rejected with a clear error rather than mistreated
  as Rec.709. Tone mapping is §7.3 work.
- A display-referred working space option ([ADR-005](adr/0005-working-colour-space.md)).
- QRhi is private API in Qt 6.11, so a Qt minor upgrade may require changes in
  `platform/qrhi` ([ADR-007](adr/0007-gpu-compositor-on-qrhi.md)).

#### Phase 4j — multi-selection ✅

Shift-click adds and removes; a rubber band drawn on empty timeline selects everything it
touches; Cmd/Ctrl-A selects all; dragging any member moves the whole set; Delete lifts the
set and Shift-Delete extracts it.

**Overlap, not enclosure.** A band selects a clip it *touches*. Requiring a clip to be
wholly enclosed would make long clips nearly unselectable by band at a normal zoom, since
one clip can easily be wider than the window.

**A band confined to the track headers selects nothing.** Clamping it into the content
area instead — the obvious way to handle an out-of-range rectangle — silently turns it
into a one-frame span at time zero, which then selects whatever happens to start there.
Refusing is the honest answer to a gesture that never entered the timeline.

**Moving a set lifts everything before placing anything.** Placing clips one at a time
makes the result depend on the order they are visited: a clip moving right collides with
its own neighbour, which has not moved yet. Removing them all first and then placing them
all makes the operation order-independent, which is what "move these together" means.

**Removing a set with ripple goes latest-first.** Closing one gap shifts everything after
it, so a clip removed early by position would be somewhere else by the time its turn came.

**The primary selection is the first entry**, and it is what Effect Controls shows. A
panel of parameters has to be about one clip even when several are selected, and picking
the one the user clicked is the only choice that is not arbitrary.

**Drag deltas are measured against where the clip is now**, not against where the pointer
started. When the model refuses a step — a locked track, a collision — measuring from the
press point would keep re-applying the whole accumulated offset and make the set jump as
soon as the obstruction cleared.

**A retina display broke the first version of the test.** `QWidget::grab` returns device
pixels, so on a 2× display the band's pixels sit at twice the coordinates the synthetic
mouse events used, and a check that the band painted only inside its own rectangle failed
against an image where it did exactly that. The test divides by the device pixel ratio
now. The band check is worth keeping despite this: a rubber band is a gesture whose only
feedback is the rectangle, and painting bugs here are invisible to every other test.

The self-test drives real `QMouseEvent`s through the widget rather than calling the
operations directly, and it caught a genuine failure while being written: the release
handler had not been wired up, so bands were drawn but never committed, and the test
reported zero clips selected out of three.
