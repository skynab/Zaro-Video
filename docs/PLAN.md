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

#### Phase 5b — keyframing from the UI ✅

A stopwatch and a diamond next to every animatable parameter, keyframes drawn
in a lane along the bottom of the clip, and dragging to retime them.

**Turning the stopwatch on drops a keyframe holding the value the parameter
already had**, so switching animation on never moves anything. Turning it off
keeps the value showing *at that moment* as the new static value — reverting to
the value underneath would make the picture jump at the instant the user
switched animation off, and that value is usually the default rather than
anything they chose.

**While a parameter is animated, typing a value writes a keyframe at the
playhead.** Writing the static value instead would appear to do nothing, since
the curve wins everywhere. The panel also stops baking widget values into the
static fields of animated parameters: the widget is showing the *animated* value,
and storing it would silently change what the picture reverts to.

**The panel shows values at the playhead**, so moving the playhead re-reads
every row. An animated parameter has no single value to display.

**One diamond per instant, not per parameter.** Eight parameters keyed together
are one decision, and eight stacked diamonds in a lane six pixels tall are one
diamond nobody can aim at. Dragging that diamond moves all of them, in one
command, or undo would take eight presses to put back what one drag moved.

**Keyframes are hit-tested before clips.** They live inside a clip, so testing
the clip first means every keyframe press starts a clip drag instead.

**Alt-click deletes a keyframe.** There is no keyframe *selection* — clips have
one, and building a second selection model just so Delete has something to act
on is the half-built trap multi-selection was deferred to avoid. A modifier on
the thing itself needs no state.

**A keyframe will not be dragged on top of another**, and a set will not be
moved if any of it would collide. Landing on a keyframe destroys it, and someone
who cannot see what was underneath cannot know to undo. Drags are also clamped
to the clip: nothing samples a curve outside the clip's own range, so a keyframe
out there could never be grabbed again or seen to do anything.

**Drag deltas follow the keyframe.** Each mouse-move retimes from where the
keyframe *is*, not from where the drag started, and the drag's own record of it
is updated — otherwise the second move looks for it where it no longer is.

**Two visual checks became assertions rather than screenshots I looked at once.**
The diamonds are counted inside the lane and required to cover pixels there
where nothing was before; the first version counted the whole widget and found
2230 matching pixels before any keyframe existed, because clip names and ruler
text are drawn in nearly the same near-white. And the panel's labels were
clipped again — "Position X" reading as "Position" next to another row reading
"Position", "Rotation" as "Rotatior" — the third time a control has shipped with
its name cut in half, now fixed by giving each label the width it asks for.

The self-test drives all of it through real widgets and events: the stopwatch,
a typed value at a second playhead position, a drag of the resulting diamond,
an alt-click deletion, and the stopwatch off again. It caught a vacuous
assertion while being written — the surviving keyframe held the same value as
the static one, so "turning animation off kept what was on screen" could not
fail. It deletes the other keyframe now, and checks the two values differ before
trusting the comparison.

Still to do: bezier handles are in the model and honoured by evaluation, but
there is no curve editor to shape them, and no right-click menu to switch a
keyframe between hold, linear and bezier from the timeline.

---

#### Phase 5c — scopes ✅

Waveform, RGB parade, histogram and vectorscope, computed headlessly in
`core/render/Scopes` and drawn by a panel beside the parameters.

§6 puts colour management before scopes and scopes before correction, and the
order is the point: a grade cannot be judged by eye alone, so the instrument
comes before the tools it measures.

**Scopes measure the display signal, not the working space** —
[ADR-010](adr/0010-scopes-measure-the-display-signal.md). In linear light middle
grey reads 18 instead of 46, and every reference a colourist has is defined on
the encoded signal. Pixels are un-premultiplied first, or a dissolve would read
as a change in exposure.

**Measured on demand, never during playback.** Measuring composites a frame on
the CPU; doing it per frame would reintroduce exactly the readback the GPU path
exists to avoid. It measures through `RenderGraph`, so the scope reports what
will be delivered.

**The measurement is counts, not a picture**, so resizing or switching
instruments costs nothing, and all four come from one pass — the expensive part
is the transfer encode.

**A real bug, found by the test rather than by looking.** The panel drew nothing
at all for a fully lit frame: level 255 mapped one pixel above the plot area and
was clipped, because the span between the first and last row is one less than
the number of rows. A black frame looked perfect either way. The test asserts
*where* the trace sits rather than how much of it there is — bright at the top,
black at the bottom — and is verified to fail when the drawing is flipped. That
is the assertion worth having: the measurement is in signal order and the screen
is upside down relative to it, and getting that backwards yields an instrument
that looks plausible and reports the opposite of the truth.

Next in the colour chain: primary correction, curves, HSL secondaries and LUTs,
which now have something to be judged against.

---

#### Phase 5d — primary colour correction ✅

White balance, exposure, contrast and saturation, on both render paths, keyframable
through the engine from 5a, and measurable with the scopes from 5c.

**Applied in scene-linear light** — [ADR-011](adr/0011-grading-in-linear-light.md).
Exposure is then a multiply: one stop is exactly a factor of two at every
brightness, and the self-test measures 125.8 → 31.6 through the real panel and
the real compositor, which is that factor twice.

**Contrast pivots at 0.18, not 0.5.** Half is the middle of an encoded signal;
in linear light it is nearly two stops above middle grey, and pivoting there
would darken every picture that had contrast added to it. White balance is
normalised by its own luma for the same reason — otherwise the temperature
slider is also an exposure slider. The two ends of the contrast control are
inverses, so it can be returned to neutral by eye, and a test takes a value
through both and requires it back where it started.

**Non-positive values are left alone.** A fractional power of a negative number
is not a number, and one NaN spreads through every pixel it is averaged with.
Scene-linear values do go negative through a wide-gamut conversion.

**The constants are computed once, on the CPU.** The shader receives gains and
exponents and never re-derives what a temperature of −20 means: two
implementations of that question are two answers.

**The two paths are checked against each other directly.** Ten corrections over
a 32×32 spread of colours — including values above 1, since a scene-linear
highlight may exceed white — must agree to within the transfer table's
interpolation error. Verified to fail by more than two code values when the
shader's pivot is changed to 0.5 while the CPU keeps 0.18. A second test grades
through a fade and requires the result to be independent of the opacity.

**The five colour parameters joined the animation vocabulary**, so a grade can
be keyframed with the same stopwatches as everything else. Adding them to the
`Param` enum turned every place that has to handle all parameters into a
compile error until it was updated — which is what the exhaustive switch is for.

Next in the colour chain: curves, HSL secondaries, LUTs and shot matching.

---

#### Phase 5e — tone curves ✅ **engine, not yet an editor**

Master and per-channel curves, on both render paths, saved with the project.

**Baked into a table both paths share** — [ADR-012](adr/0012-curves-baked-into-a-shared-table.md).
The curve is evaluated only on the CPU, only when it changes, into 1024
linear-in linear-out entries per channel; the shader looks the answer up. There
is nothing in the shader that *can* drift from the reference, because it does
not implement the reference. Where the primary correction needed a parity test
to keep two implementations honest, here there is one implementation.

**Indexed by `sqrt(v / (1 + v))`.** It maps all of [0, ∞) — linear light has no
ceiling and a highlight may be several times white — onto [0, 1] in three
operations, identical on both sides. It also spends its resolution where the
picture is: middle grey lands near entry 400, a thousandth of white near entry
30. A linear index would give the shadows one entry, and the shadows are where a
curve is read most closely. Indexing by the encoded value instead would have put
the transfer functions in the shader, which is the duplication the whole design
avoids.

**Monotonic cubic interpolation** (Fritsch–Carlson). A natural spline overshoots
near a steep segment, and an overshooting tone curve puts a dark halo above a
highlight and can invert a gradient. A test walks a thousand points across a
deliberately steep curve and requires the result never to decrease; it was
verified to fail with the slope limiting removed.

**An identity curve is skipped rather than sampled**, since sampling one would
round every ungraded pixel through the table's resolution and an ungraded clip
has to come out bit-identical.

**Tables are cached against the curves themselves** rather than behind a dirty
flag — the kind of flag that is correct until an undo restores a snapshot behind
its back.

The GPU work cost one real bug: the curve texture was freed at the end of the
draw while the resource bindings still held a raw pointer to it, which is a
segfault rather than a wrong picture. The parity test found it immediately.

**Not done: the curve editor.** The curves are reachable from code and from a
project file, not from the UI. A curve editor is a real widget — click to add,
drag to shape, right-click to remove, with the four channels switchable — and
building half of one is worse than none, so it is its own phase.

---

#### Phase 5f — the curve editor ✅

A square plot with the identity diagonal for reference, a channel chooser, and
direct editing: click to add a point, drag to shape it, alt-click to remove —
the same modifier as a keyframe on the timeline, and for the same reason there
is no separate selection to delete from.

**The widget holds no model state it has not been told.** It emits a whole
`ToneCurves` on every change and is given the new value back through the normal
refresh. A panel that kept its own version of the curve would drift from the
command stack the first time an undo happened behind it.

**A drag is one undo step.** `curvesChanged` carries whether the gesture has
ended, so the panel coalesces the drag and then breaks the merge.

**The outermost points are pinned in x.** Black is where the curve starts and
white is where it ends; moving them inward leaves the ends of the range
undefined, and the held value out there is not what anyone means by dragging the
black point. Interior points are clamped between their neighbours, since two
points at one x is a vertical segment and crossing one would reorder the curve
under the pointer.

**The plot is square regardless of the panel's shape.** The diagonal is the
reference for "this curve does nothing", and it is only at 45 degrees when the
axes share a scale.

**The curve is drawn by sampling the same evaluator the render path bakes**, one
sample per pixel, so what is drawn is what will happen rather than a
smooth-looking sketch of it.

**Three failed measurements before the test was right.** The self-test asserts
that a curve reaches the *preview*, and it reported failure twice while the code
was correct: the fixture is flashes on black, so a midtone lift moves almost
nothing, and its lit frames are saturated white, where lifting the black point
cannot change anything at all. Measured on a dark frame the same edit reads 17.8
against 0.0. The lesson is about the fixture rather than the feature — a test
that picks its own measurement point has to pick one where the change it is
looking for is possible.

It did find one real bug on the way: the GPU graph passed the curve table at one
of its three draw call sites, because clang-format had reflowed the other two
past the patterns used to edit them. The clip path — the one every ordinary
frame takes — was the one left out.

Also fixed: value fields now keep the width their contents need. "0.00 EV" had
been cut to "0.00 E", which is the third time a control has shipped with its
text cut in half, and a truncated unit reads as a different number rather than
as a shorter label.

Next: HSL secondaries, LUT (.cube) support, and shot matching.

---

#### Phase 5g — HSL secondaries ✅ **engine, not yet an editor**

A qualifier — hue, saturation and luma windows multiplied together — and a
correction applied only where it selects, on both render paths, with a mask view.

**Every edge is soft.** A hard threshold gives a mask with stepped edges, and a
correction through it looks like a sticker rather than a grade. All three
windows fall off with the same smoothstep, so a mask has no seam where one axis
takes over from another.

**Hue wraps.** A window centred on red runs from about 350 to about 10, and
subtracting hues without wrapping calls those 340 degrees apart and selects
nothing — which is the first thing anyone reaching for skin tones would hit. A
window covering the whole circle keeps neutral pixels too, since grey has no
hue and "everything dark" is mostly grey.

**Hue and saturation are scene-referred; luma is display-referred.** Hue and
saturation of light are properties of the light, and computing them from an
encoded signal would make a qualifier's meaning depend on the display curve the
sequence happens to carry. Luma is the one axis where a display threshold is
what somebody means — "midtones" means the tones that look like midtones — so
those two numbers are converted to linear once, on the CPU, and everything
downstream compares in linear with no transfer function near a per-pixel path.

**A selection reaching white keeps everything above it.** Linear light does not
stop at white; a highlight three times white is not "outside the highlights",
and dropping it would punch holes in exactly the region the qualifier was aimed
at.

**The mask view is not a debugging aid.** Judging a qualifier by looking at the
corrected result is guesswork, and it is the only way to see what is selected.

**The order is primary, curves, secondary** — the order a grade is built in. A
secondary keyed on skin tone before the shot is balanced is keyed on the wrong
colour.

The qualifier is the one piece of this pipeline that had to be written twice: a
per-pixel mask cannot be baked into a table the way a curve can. So it gets the
treatment the primary correction got — six cases over a frame spanning every
hue, three saturations and brightnesses from shadow to above white, required to
agree between CPU and shader. Verified to fail when the hue wrap is removed from
the shader alone.

**The real find was in old code.** Both render graphs had three copies of the
draw setup — an ordinary clip and the two halves of a transition — and they had
drifted: the outgoing half of a transition had gone two phases with no colour
correction at all on the CPU path, and the incoming half had no tone curves on
the GPU path. Patches meant to update all three had silently matched only some,
because clang-format had reflowed the others past the patterns being matched.
Both graphs now have one `drawClip`, and a test composites a graded transition
and checks both halves; it fails if either is drawn without its grade.

**The undo budget test became a per-clip budget.** Colour correction, curves and
a qualifier between them added about a tenth to what a snapshot costs, which
pushed a flat 200 KB cap over. A flat cap would have to be argued upwards every
time a clip gains a field, without anyone deciding whether the growth was
reasonable; it now asserts what one clip costs in one snapshot, which is the
number that actually matters when snapshots hold whole sequences.

**Not done: the qualifier UI.** Same split as the curves: engine first, editor
next. A qualifier needs eyedroppers, three range controls with soft-edge
handles, and the mask toggle, and half of that is worse than none.

---

#### Phase 5h — the qualifier UI ✅

The secondary is reachable: an enable toggle, a mask toggle, a hue band, and
controls for the three windows and the keyed correction.

**The hue band exists because three numbers describe nothing anyone can
picture.** Centre, width and softness on a circle are arithmetic; the band shows
them as a selection, dimming what is outside rather than hiding it — what is
*not* selected is as much a part of reading a qualifier as what is. It draws the
window with the same wrap and the same smoothstep the mask uses, because a band
that disagreed with the selection would be worse than no band.

**The keyed correction is deliberately a subset of the primary's** — temperature,
exposure, saturation. Those are what a keyed correction is almost always for,
and every extra control is one more thing between someone and the qualifier they
are actually trying to set. Saturation and luma softness are not exposed either:
the default already keeps the edge from stepping, and they remain in the model
and in the file for anyone who needs them.

**The panel scrolls now.** Motion, colour, a curve editor, a secondary and audio
is taller than a short display, and without scrolling the last group is simply
unreachable with nothing on screen to suggest it exists.

**One more absolute pixel threshold removed.** The self-test asserted that a
wide-open qualifier reads above 100, which had been calibrated when the panel
was narrower — mean brightness of the monitor depends on how much of it the
letterbox covers, and that moves whenever a control is added. It compares the
mask against the same frame's picture instead: a white picture entirely selected
shows as a white mask, and the two readings now agree exactly (240.1 and 240.1),
against 0.0 once the luma window is narrowed to the darks.

Next: LUT (.cube) support, then the audio track mixer and Essential Graphics.

---

#### Phase 5i — look LUTs ✅

A .cube reader, a baked cube on both render paths, a file picker and an amount
control. Applied after the primary correction and before the curves: a LUT is a
look put on a balanced picture, and the curves are the adjustment made on top of
the look.

**Baked, not re-implemented** — the same decision as the curves, extended in
[ADR-012](adr/0012-curves-baked-into-a-shared-table.md). The shader holds no
.cube parser, no domain handling and no transfer function.

**Three things had to be got right for an identity LUT to do nothing**, and each
was found by the test that says so:

- *The stored values are warped like the axes are.* Storing linear values makes
  interpolation under-shoot on a convex axis.
- *The cube covers exactly the LUT's domain.* Spread further it interpolates
  across the clamp at white and dims the picture by about a percent — measured,
  before the axis was cut off at the domain.
- *The shader samples texel centres.* Reading from coordinate 0 lands half a
  texel outside the first entry and shifts the whole cube, which looks like a
  slightly wrong look rather than a sampling mistake. That one was found by the
  parity test, at a mean error of 0.034 across four cases.

**The parser refuses a file rather than half-reading it**: data before the size,
too few entries, too many, a size of one, an inverted domain. Each of those
would otherwise produce a LUT that looks plausible and grades wrongly, which is
worse than not loading. The red index moves fastest, per the format — getting
that backwards swaps red and blue in every look, so a deliberately asymmetric
fixture is what tells the two apart.

**A missing LUT is a clip that grades without it**, not a render that fails, and
the failure is remembered so a broken path is not re-opened every frame.

The LUT is stored by path rather than inline: a .cube is hundreds of kilobytes
of text, and a project that embedded one per clip would be unopenable in a text
editor. The cost is that a project can be moved away from its LUTs, which is the
same bargain the media references already make.

---

#### Phase 5j — the audio track mixer ✅

A strip per audio track — name, meter, mute, solo, gain and pan — with a master
meter beside them.

**Solo is a property of the sequence, not of a track.** Muting a track silences
that track; soloing one silences every track that is *not* soloed. Whether an
ordinary track is audible therefore depends on whether anything else is soloed,
which a track cannot answer alone — so `Sequence::isAudible` is where the rule
lives, and all three render graphs ask it rather than checking `isMuted`. **Mute
wins over solo**: soloing a track someone has muted and hearing it anyway would
make mute mean nothing.

**Meters are measured on the way through the mix**, per track post-fader, plus a
master measured on the sum — the only figure that accounts for two tracks adding
up past full scale. Peak rather than RMS: a mixer's meter answers "is this about
to clip", and RMS can sit comfortably while individual samples are over. A
loudness meter is a different instrument and belongs with the loudness work.

**The meter scale is in decibels.** A linear meter spends four fifths of its
height on the top two stops and says nothing at all about a dialogue track
sitting at −20 dB, which is where dialogue sits.

**The peak hold falls at a fixed rate rather than following the signal.** The
thing a meter is for is catching the transient that went over, and a transient
is by definition brief — a hold that dropped straight to the current level would
only ever show the current level.

**Meters keep working when the transport is stopped.** While playing they come
from the thread producing the audio, so they show what is being heard; stopped,
a short block is mixed at the playhead. A mixer whose meters die when playback
stops cannot be used to set a level, which is most of what a mixer is for.

**Strips are rebuilt only when the set of tracks changes.** Tearing them down on
every refresh would drop whatever the pointer was holding, and a fader that lets
go halfway through a drag is unusable.

A track silenced by someone else's solo is shown dimmed rather than marked
muted: it is not muted, and saying so is a lie the user would then try to undo.

**The layout needed floors.** Three panels in one column squeezed Effect
Controls to a sliver — a scope four pixels tall looks like a broken panel rather
than a small one — so each has a minimum height.

**And a fourth absolute pixel threshold went.** Adding those minimum heights
changed the monitor's size, which changed how much of it the letterbox covers,
which dropped the LUT self-test's reading from 135.6 to 99.7 against a hard
`+100`. It is stated against a white frame of the same fixture now. Every
absolute brightness threshold in this self-test has eventually been wrong; the
relative ones never have.

---

#### Phase 5k — shape layers ✅ **shapes, not yet text**

Generated clips: a rectangle or an ellipse, with size, position, corner radius,
feather and colour, on both render paths.

**A graphic is a clip, not a second kind of thing on a track.** It is trimmed,
moved, faded, graded, keyframed and linked exactly like any other clip, and
every one of those operations already works. A separate type would mean either
duplicating all of them or discovering, one at a time, which ones it had been
left out of. A test puts a fade, a grade and a move on a shape and checks all
three, and none of that code was taught about shapes.

**Shapes are positioned in the same coordinates `Transform` uses** — output
pixels from the centre of the frame — so a shape and a clip mean the same thing
by "forty pixels right", and the motion controls work on a graphic with nothing
special-cased.

**The edge is a signed distance, not an inside/outside test.** The distance
gives antialiasing and feather from the same arithmetic: coverage is a ramp
across it, and the only difference between the two is how wide the ramp is. The
ramp has a one-pixel floor, so a hard-edged shape is still antialiased — a
stair-stepped edge is the first thing anyone notices about a generated graphic.

**The GPU path rasterises on the CPU and uploads** through the compositor's
existing RGBA path. A shader would be faster and would be a second
implementation of the geometry to keep in step with the first, for a buffer that
changes only when somebody edits the shape.

**The self-test had its steps in the wrong order** and said so: it looked for
the darkest frame *after* adding a white rectangle that covers the frame, so
every position read the same and it settled on frame zero — which on this
fixture is a flash frame that is white anyway. Measured before, the same shape
reads 176.5 against 0.0.

**Not done: text.** Text needs a font engine, and `core/` links neither Qt nor
FFmpeg by design — so a text layer needs a rasteriser in `platform/` behind a
core interface, and the export tool needs to link it too. That is a real piece
of plumbing rather than an afternoon, and shapes stand on their own in the
meantime: colour mattes, lower-third backgrounds and mattes for the qualifier
all work now.

---

#### Phase 5l — text layers ✅

Text graphics, rasterised by Qt's font engine behind a core interface, drawn by
the preview and by the export tool.

**The font engine produces coverage, not colour.** An implementation renders
glyphs as an alpha mask, and this layer multiplies by the graphic's colour in
linear light. Asking the engine for coloured text and converting the result from
sRGB per pixel would convert the antialiased edges as if they were colours,
which is the reason text composited in a linear pipeline so often comes out
looking thin.

**`core/` has no font engine and is not getting one.** It links neither Qt nor
FFmpeg, and text is exactly the sort of thing that drags a toolkit in through
the back door. `render::TextRasterizer` is an interface; `platform/qtext`
implements it on Qt Gui — not Widgets, so the export tool needs no window to put
a title into a delivered file.

**No rasteriser is a countable outcome, not a blank frame.** A tool that was
never given one renders everything else and reports how many text layers it had
to skip; `zaro-render` prints a warning. A delivered file quietly missing its
titles is the worst outcome available, so the number is on the record.

**Sizes are in pixels, not points.** A point size depends on a notional DPI, and
a title has to be the same size in a delivered frame whatever the machine that
rendered it thought its screen was. Subpixel antialiasing is off for the same
class of reason: it is specific to one screen's pixel layout and is wrong the
moment the result is composited or delivered anywhere else.

**`zaro-render` builds its own `QGuiApplication`.** Qt's font engine needs one
even with no window. A library that constructed one behind its caller's back
would fight whatever the caller had already made, so the tool does it and the
rasteriser says so plainly when there is none.

**A flaky check became a deterministic one.** The mixer self-test read zero
once: meters are only updated while the panel is visible — which is right — and
whether a widget has become visible depends on how many event loops have run. It
now asks explicitly and fails with a message about visibility rather than
looking like a broken mixer.

---

#### Phase 5m — captions ✅

SubRip and WebVTT in and out, a caption track on the sequence, burn-in on both
render paths, and a menu to import, export and switch it on.

**Captions are timed in milliseconds, not frames.** Subtitle formats are defined
in milliseconds; a caption imported at 24fps, rounded to frames and written back
would come back a few milliseconds different from the file that produced it,
every time, compounding across a round trip through another tool. A test
requires writing and reading to be exact inverses.

**One reader for both formats.** They differ in a header, a decimal separator
and features nobody uses in an edit — refusing a .vtt that is a .srt with dots
in it would be pedantry rather than correctness. The reader handles optional
hours, one to three fraction digits, cue settings after the timestamps, a byte
order mark, and CRLF, because files in the wild have all of them.

**Overlapping captions are both kept.** The formats allow them, and a reader
that assumed one at a time would drop the second half of every conversation
where two people speak over each other.

**Burn-in is off by default and is its own decision.** A sidecar file delivered
alongside the picture is the normal case, and burning in has no way back once
the file is written. Captions draw over everything: they are a deliverable laid
on the picture rather than a layer in it, and a caption a later track could
cover is one nobody can read.

**The caption graphic is built by shared code**, so the CPU and the GPU put
captions in the same place — which is the whole of what a burned-in caption has
to get right.

**Importing keeps the style and the burn-in setting.** Those belong to the
sequence, not to the file: bringing in a revised script should not silently turn
burn-in off or lose a typeface somebody chose.

**An import is one undo step.** Captions arrive and leave as a file, and three
hundred undo steps would bury whatever came before them.

Two of my own mistakes, both the same shape as before: the round-trip test
compared against a sequence that had already been moved into the project, and
the burn-in self-test measured a white caption on the fixture's white flash
frame. Both said "no difference" while the code was right.

---

#### Phase 5n — OpenTimelineIO ✅

Read and write OTIO, a `zaro-otio` conversion tool, and export from the window.
§7.7 called this the highest-leverage item in the inventory and it is: an edit
can leave here and come back, or arrive from a system that has never heard of
this one.

**An OTIO track is a sequence, not a set of placed clips.** Position is implied
by order and duration, and a hole is a `Gap` — an object, not an absence.
Everything else follows: writing emits the gaps, reading accumulates durations
back into positions, and a test checks that a clip at frame 40 comes back at
frame 40 rather than at zero.

**Broadcast rates are snapped back on the way in.** OTIO writes rates as
doubles, so 24000/1001 becomes 23.976023976023978, and reading that as a ratio
gives something nobody means — a sequence that drifts against every other tool
by a fraction of a frame per hour. A rate within a hair of a standard becomes
the exact rational for it.

**The timeline's rate is carried by its start time**, because OTIO states it
nowhere else. That took a bug to notice: a sequence's start time defaults to a
24fps zero, so a 25fps timeline was being written as 24 and read back as 24. It
is rescaled into the sequence's own rate now, always.

**An unknown item still advances the cursor.** A Stack or a Transition inside a
track is skipped rather than guessed at — but its duration counts, or everything
after it lands early.

**Importing produces a project of its own.** An OTIO file names media by URL,
and matching those against media already open is a different decision from
reading the file. That is also why import lives in `zaro-otio` and not in the
window: replacing the open project needs a "save first?" that does not exist
yet, and the tool has nothing to lose.

**A flaky self-test, and the flake was informative.** The mixer check read the
meter at one arbitrary position. This fixture's audio is clicks a second apart,
so most positions are silence — and a peak hold is *designed* to keep showing
the last loud thing, so the test was reading a value held over from wherever the
playhead had been earlier. It scans for the loudest position now, and four
consecutive runs agree.

---

#### Phase 5o — masks ✅

A rectangle or ellipse that limits where a clip shows through, with corner
radius, feather and invert, on both render paths.

**A mask is in output coordinates, not the clip's.** It stays put when the clip
moves — which is what makes it a window onto the screen rather than a crop. A
test moves a clip twenty pixels right and checks the mask has not gone with it;
those are different tools and conflating them would leave neither working.

**Invert is a flag, not a second mask.** A vignette and a spotlight are the same
shape with this flipped, and drawing the complement by hand is how people end up
with two masks that have to be kept agreeing. A test walks the frame and
requires a mask and its inverse to sum to exactly one everywhere.

**Masks and shapes share their geometry.** A mask and a generated shape of the
same size cover the same pixels, checked pixel by pixel — which is what anyone
would assume on seeing the two controls side by side.

**The one place it had to be written twice is the shader.** A signed distance
depends on where the fragment lands, so it cannot be baked into a table the way
a curve can. It gets the qualifier's treatment: six cases compared directly
against the CPU, and the frame position is derived from the clip position in the
vertex shader rather than passed separately, so it cannot disagree with where
the quad actually goes. The y-flip in that derivation was right first time, and
the test was verified to fail when it is removed.

The self-test measures a mask, its inverse, and the whole picture: 6.5 through a
small ellipse, 170.0 inverted, 176.5 whole. The first two adding to the third is
the assertion worth having — it says the two halves partition the frame rather
than merely differing from it.

---

#### Phase 5p — speed and reverse ✅

Clip speed and reverse playback, on both render paths, with the audio retimed to
match. `sourceTimeAt` was called "the hook a speed or time-remap curve replaces
later" back in Phase 2; this is that.

**Speed is not stored.** It is the ratio between a clip's two ranges, which
every trim and every retime already maintains. A `speed` field would be a second
source of truth about timing, and the first time the two disagreed the clip
would play at one rate and be laid out at another. Direction is the one thing
two positive ranges cannot express, so `reversed` is the one thing stored — and
for the same reason a negative speed is refused rather than accepted as a second
way to say it.

**A reversed clip starts one frame inside its out point.** The out point is
exclusive; reading it would be reading the frame after the clip.

**Keyframes reverse with the picture.** `sourceSecondsAt` runs backwards too, so
a fade set on a frame still lands on that frame when the clip is flipped — which
is the whole reason keyframes are in source time
([ADR-008](adr/0008-keyframes-in-source-time.md)).

**The audio is resampled**, because a retimed clip covers more or less source
than it occupies. Without that the picture retimes and the sound does not, and
the gap grows for as long as the clip lasts — precisely the drift the rational
time discipline exists to prevent. The pitch moves with the speed, which is what
a plain speed change does everywhere; holding pitch is a different feature with
a different name, and pretending this one does it would be worse than not having
it.

**A retime ripples.** Speeding a clip up otherwise leaves a hole and slowing it
down runs over its neighbour. The source range is untouched: a retime changes
how long a clip occupies the timeline, not which frames it covers.

The test source gained a ramp mode — a picture whose brightness says which frame
it is — so a test can check *which* frame was fetched rather than only that one
was. That is what makes the reverse test meaningful rather than a check that
something was drawn.

---

#### Phase 5q — track EQ and compression ✅

A three-section equaliser and a compressor on every audio track, inserted before
the fader, with the strip's controls in the mixer.

**Before the fader, which is where an insert goes on every console ever built.**
Pulling the fader down then turns down what the compressor did rather than
changing what it does. That needed the mix restructuring: clips now sum into a
per-track bus, the chain runs on the bus, and only then does the fader apply.
The track meter moved with it and is measured post-fader, which is what a strip's
meter is for.

**Processing has state, so mixing is no longer a pure function of time.** A
filter's delay line and a compressor's envelope both depend on what came
immediately before — that is the point of them — but it means a seek has to
reset the chain, or the envelope from one part of the timeline follows the
playhead to another and the first moment after a jump is ducked for no reason.
`resetProcessing` is that reset and playback calls it.

**One detector across all channels.** Compressing each side separately makes the
stereo image wander whenever one of them alone is loud, which is what makes a
mix sound unstable rather than controlled. A test drives one side hard and the
other quietly and requires their ratio to survive.

**Three EQ sections, not eight.** A dialogue track needs the rumble out, the
hiss off, and one bell for whatever the room did. A parametric with eight bands
is a different tool and mostly a way to make a track worse more precisely.

**A section set to nothing is a bypass, not a near-bypass.** Three sections that
each nearly do nothing still colour a track that was supposed to be untouched,
so zero frequencies and zero gains take the filter out of the path entirely.

**A limiter is the compressor with a ratio nobody argues with.** Having both
boxes would mean explaining which runs first.

Coefficients are the RBJ cookbook's, so a filter set to 80 Hz here is the same
filter as 80 Hz anywhere else. The tests measure actual responses — 3 dB down at
the corner, 6 dB of bell being a factor of two, a 4:1 ratio turning 20 dB over
into 5 — rather than checking that numbers were stored.

One test of mine was wrong before it was right: it asserted a 20 ms attack had
audibly engaged after 1 ms, which it has not and should not. It checks the shape
of the attack now — that the reduction deepens over time — rather than one
arbitrary instant.

---

#### Phase 5r — loudness (EBU R128) ✅

Integrated, short-term and momentary loudness to ITU-R BS.1770, measured through
the real mix, with a normalise action that lands a programme on −23 LUFS.

**The measurement a delivery specification is written in.** A peak meter says
whether a mix will clip; this says whether it is as loud as the broadcaster
asked for, which is the question that gets a programme rejected.

**The published K-weighting parameters are not cookbook parameters**, and that
cost a bug worth keeping in the record. A corner of 1681.97 Hz, a Q of 0.70717
and a gain of 3.9998 dB look exactly like the arguments to an RBJ high-shelf —
they are not. Building one that way gives a filter 0.2 dB low at a kilohertz,
which failed the standard's own calibration case: a 1 kHz sine at −23 dBFS came
out at −23.25 LUFS. The standard's derivation reproduces its published
coefficients exactly, and the filters are built from it now, at whatever sample
rate the sequence runs at rather than only at 48 kHz.

**The gating is not a detail.** Without it a programme with quiet passages
measures quieter than it sounds, so everyone pushes the loud parts up to
compensate — the loudness war moving rather than ending. A test puts six seconds
of silence in the middle of a programme and requires the answer not to change.

**The reading does not depend on how the audio is fed**, checked at block sizes
from 64 to 48000 — the same reasoning as the per-sample automation in 5a.

**Sample peak, and it says so.** True peak needs oversampling to catch
inter-sample peaks; claiming it without would be a number that passes a check
the delivered file fails.

**Measured through the mix that will be delivered** — faders, pans, automation,
the processing chain and every clip gain included. Measuring the clips would
give a number about the material rather than about the programme. Normalising
moves every audio fader by the same amount rather than a master gain that does
not exist: the balance between tracks is a decision somebody made.

**Silence has no gain that would make it loud**, so the offer is zero rather
than an enormous number, and an empty range is refused rather than reported as
silence — those are different answers.

The self-test measures the fixture at −14.9 LUFS, applies −8.1 dB, and measures
−23.0.

---

#### Phase 5s — proxies ✅

A smaller copy attached to a media reference, a project-wide toggle, and an
export that ignores it.

**Attached, not generated.** Making a proxy is a transcode, and a transcode
belongs to whatever tool the footage came out of. What this has to get right is
the swap, and the swap is where the bugs were.

**The toggle belongs to the project, not to a clip.** Nobody wants some shots on
proxies and some not, and the whole reason to be on proxies is that the machine
cannot keep up with the originals.

**Export ignores it.** Delivering the small copies because somebody left a
toggle on is a mistake with no warning attached and no way back once the file
has gone out, so the render takes a copy of the project with proxies off rather
than trusting the state it was handed.

**Paths are resolved once, when the media source opens.** A source deciding per
read would have to be told when the toggle moved, and the decoders it had
already opened would still be on the old files. Switching therefore means
reopening — which is what found two real bugs:

- **Reopening the media aborted the application.** `startWaveforms` assigned
  over a `std::thread` that was still joinable, which is an immediate
  `std::terminate`. It presented exactly that way: switching proxies on killed
  the app instantly.
- **The monitor froze after any re-open.** `setSource` drops the render graph —
  the old one holds the old provider — but only `initialize` ever rebuilt it,
  and that runs once. Every re-open left the picture stuck on the last frame
  drawn. The graph is rebuilt whenever it is missing now. This would have hit
  relinking and opening a second project too; proxies are merely what reached it
  first.

The test fixture is a proxy that is inverted as well as smaller, so which file
was read is unmistakable rather than a matter of judging sharpness: 246.2 from
the original, 0.1 through the proxy.

---

#### Phase 5t — nesting ✅

A sequence used as a clip, on both render paths and in the audio mix.

**A nest is a clip, not a special case.** Its source range is a range of the
inner sequence's own timeline, so every trim, retime, reverse, grade, mask and
keyframe already works on it — and none of that code was told nesting exists. A
test grades a nested clip two stops down and gets a quarter of the light.

**Cycles are refused at the edit, not guarded against at the render.** A
sequence containing itself is a render that never finishes. A depth limit would
turn an impossible project into a merely wrong one and explain nothing to
whoever made it, so `nestingWouldCycle` walks the whole chain — A inside B
inside C is fine until somebody puts A inside C — and the operation refuses. The
same sequence used twice is a diamond, not a loop, and is allowed. A depth limit
does exist, as a backstop for a project that arrived from an OTIO file or a
hand-edited save.

**The GPU preview composites a nest on the CPU and uploads it**, the same
decision as the shapes. Doing it on the GPU means rendering into a texture and
sampling it back, which needs a second target to ping-pong against — real
plumbing for a case that is rare in a preview and never on the path that
delivers, since export is the CPU renderer anyway.

**Recursion had to stop clobbering the counters.** `compositeInto` resets the
clip tally, so a nested call wiped the one the level above was building. Saved
across the recursion: a nested sequence contributes one clip to the outer count
however many it contains.

**Two latent bugs, both about pointers into a growing vector.** Adding a
sequence reallocates the project's `std::vector<Sequence>`, and both the window
and the monitor held raw pointers into it. The window looks its sequence up by
id now; the monitor is re-seated through one place that the application and the
self-test both use. Neither was reachable before, because nothing created a
sequence at runtime — making one to nest is exactly that. It presented as a zero
denominator deep inside rational arithmetic, which is a long way from the cause.

---

#### Phase 5u — multicam ✅

Several angles on one clip, one of them live, switched with the number keys at
the playhead.

**A switch is a cut.** The clip splits and the part after takes the new angle.
Modelling it as anything else would mean a second kind of edit that trims,
transitions and ripples all had to learn about — when what somebody wants is
exactly the cut they would have made by hand. Switching at the very first frame
changes the clip instead, since splitting there leaves a piece of no length.

**Angles are synced by an offset, not by trimming.** Cameras rarely start
rolling together, so each angle carries how far into its own material the
group's zero point is. Trimming each to a common start would mean re-deriving
the sync on every switch, and a switch that lands a frame out *sometimes* is
unfindable.

**A multicam clip is still a clip.** Only which file it reads changes, so trims,
grades, transitions, masks and keyframes all go on working, and the three read
sites — CPU, GPU and audio — needed one change each: ask for the active angle
rather than the clip's source.

**An out-of-range angle falls back to the first, not the nearest.** The header
said "the first" and the code clamped, which answers "the last" — an arbitrary
camera to land on. The test caught the disagreement; the code now matches what
was documented, because the first angle is where a multicam clip starts and is
the one recovery nobody has to reason about.

The number keys act on the primary selection only: switching several clips at
once would be several cuts in several places, which is not what pressing a
number means.

**Not done: sync detection.** Working out the offsets from timecode, from
markers or from the audio is a separate problem — the first two are arithmetic
on data that may not be there, and the third is cross-correlation. The
operations take the offsets they are given and do not guess.

---

#### Phase 5v — adjustment layers ✅

A clip that carries no picture of its own and grades everything composited
beneath it, for the length it covers and only where it covers.

**An adjustment layer is a clip, not a track property.** It trims, ripples,
moves between tracks, takes a mask and takes keyframes, because it is the same
`Clip` every other operation already knows how to edit — one flag, `adjustment`,
changes what compositing does with it. A property of the track would have needed
its own duration, its own trimming and its own undo, all of it duplicating what
clips do.

**It grades what is already there, in place.** The compositor reaches the layer
with the frame beneath it fully assembled, so the grade runs over that buffer
rather than over a source that no longer exists separately. Pixels that are
still transparent are skipped: there is nothing under them to grade, and
grading nothing produces a coloured haze over the empty parts of the frame.

**Opacity and the mask limit it the same way they limit a picture.** The graded
result is blended back over the ungraded frame by opacity × mask coverage, which
makes a half-opacity adjustment layer mean *half the grade* — the reading anyone
would expect — and makes a masked one a power window over the whole stack for
free.

**The GPU path hands the whole frame to the CPU compositor.** Grading what has
already been drawn means reading back the render target and writing it again,
which on QRhi is a ping-pong between two textures and a restructure of the draw
loop. Rather than build that now and have preview diverge from export while it
settled, a sequence containing an adjustment layer composites entirely on the
CPU and uploads the result. It is slower, and it is the same code that exports,
so what is on screen is what is written to the file. The fast path is a
performance change, not a correctness one, and can land later without anything
above it moving.

**Identity is checked before any work.** A layer with no grade, no curves, no
qualifier and no LUT returns immediately, so an empty adjustment layer costs one
comparison rather than a pass over every pixel.

The self-test places one over the picture through the real preview and reads two
stops of difference off the actual frame; it also had to create the track it
sits on, which reallocated the sequence's track vector and dangled a reference
the later blocks were still using — the same shape of bug as Phase 5t, and the
reason the blocks after it now resolve ids through the project rather than hold
references across edits.

**Not done: the render cache, and sync detection from Phase 5u.** Caching
rendered ranges to disk so playback of a graded stack runs in real time is the
next phase; it is a storage and invalidation problem rather than a rendering
one, and adjustment layers are exactly the case that makes it worth having.

---

#### Phase 5w — the render cache ✅

Composited frames kept, and a range rendered ahead of being played.
[ADR-013](adr/0013-render-cache-invalidates-by-recipe.md).

**A cached frame carries the recipe it was made from.** Every alternative is a
form of bookkeeping — each operation declaring which frames it dirtied — that
has to be right in every operation ever written, and is silent when it is not.
Instead the frame stores a hash of everything that decides what the sequence
looks like at that instant, and a lookup that finds a different hash is a miss.
Nothing has to be told that anything changed.

**The hash is built from what the project file records, by the encoder that
writes it.** This is not a detail. A hand-written list of the fields a render
depends on fails the same way dependency tracking does: a field gets added,
serialized and rendered, the list is not updated, and from then on the cache
serves frames from before the change with nothing to point at. Running
`io::fingerprint` through the real encoder means a field that survives a save
is in the recipe the moment it is written.

**Only what is drawn at that instant counts.** A clip somewhere else on the
timeline is not in the recipe, so moving it invalidates nothing — which is the
difference between caching a timeline and caching a project. The self-test
checks both halves: a grade under the bar takes the bar away where it reaches,
and leaves the two hundred frames it does not reach alone.

**It caches the CPU compositor, which is the one that needs it.** The GPU path
is already real time. What is not is the whole-frame CPU composite that an
adjustment layer forces (Phase 5v), and that is exactly the case somebody
cannot play back. The cache is opt-in and export does not take it: an export
visits every frame once and would fill a gigabyte with frames nothing will ask
for again, evicting the ones the editor is using while it runs.

**Memoisation alone is not enough, so there is a pre-render.** Remembering a
frame helps the *second* pass over a range, and the first pass is the one
somebody is sitting through. "Render the visible range" fills the cache ahead
of time, with a progress dialog that cancels — and a cancelled render is a
partial one, not a wasted one; the frames it managed stay.

**The visible range, not the whole sequence.** What somebody wants rendered is
what they are about to watch, and rendering everything on a long timeline is a
decision to wait for frames nobody asked about. The range is chosen by
scrolling and zooming, which they are doing anyway.

**The bar is sampled, and it is one colour.** It is drawn a pixel at a time and
read at a glance, so checking more instants than it has pixels costs a hash per
frame over the whole timeline on every repaint and shows nothing extra. Green
and nothing else: a bar with three colours in it makes three claims, and only
one of them — "this will play" — is one that can be made honestly.

The end-to-end self-test pre-renders through the real window, plays twenty
frames through the real monitor and requires them to come back from the cache
(19 hits, 0 misses), then grades the layer and requires the bar to retreat
exactly as far as the grade reaches. With the cache lookup disabled it reports
0 hits and fails.

**Not done: a disk cache.** Surviving a restart means choosing an intermediate
codec, managing a directory and deciding when to collect it — none of which is
needed to make a graded stack play back, which was the problem in front of us.

---

#### Phase 5x — sync detection ✅

The offsets Phase 5u took as given, worked out from the material.

**Two methods, because a shoot is either jam-synced or it is not.** Timecode is
subtraction: the same clock written into every file, and the offsets fall out
with nothing to be confident about. What is left when it is absent is
correlating the sound, which is a measurement with an error bar. There is no
useful middle case to design for, and pretending there is would mean one code
path that is worse at both.

**A camera with no timecode is not a low-confidence answer, it is a camera this
method cannot do.** Reporting a plausible wrong offset is the failure that
shows up three cuts later as "something feels off"; reporting "cam C has no
source timecode" is a sentence somebody can act on. So the result is per angle,
with an offset or a reason, and the angles that could be synced are — a shoot
where one camera was not jammed is the normal case, and refusing the other
three because of it would be worse than saying which one is left.

**By ear is two passes, not one.** A correlation at sample resolution over a
thirty-second search range is hundreds of billions of operations. One over a
loudness envelope is millions — but its answer is only as good as its block,
and a tenth of a frame out is exactly the sync error nobody finds later,
because it looks right on most cuts. So the envelope finds roughly where, and a
short correlation of the raw samples around that answer finds exactly where.
The tests require the offset to be found *to the sample*, at four different
delays including a negative one.

**The refinement listens where there is something to hear.** Its window is
centred on the loudest part of the overlap rather than on the middle, because a
second of room tone correlates with any other second of room tone and would
confidently return the coarse answer unchanged.

**Normalised correlation, not a dot product.** Two cameras at different
distances from the same clap record the same shape at different levels, and it
is the shape that says where they line up. A test grades one angle down to a
twentieth and still requires the exact offset.

**Silence says so.** A signal with no variation correlates with nothing; the
answer is a reason, not a confident zero that reads as "they were already in
sync".

**Angles are mixed to mono before comparing.** A camera with a dead left input
and a camera with a dead right one recorded the same room, and comparing one
channel each would fail to notice.

**Applying the offsets is one command.** Undoing a sync halfway would leave a
clip where two cameras agree and two do not, which is worse than either state.

The self-test picks a multicam clip in the real window, asks for a timecode
sync, and requires the offset to land on the clip and to come back whole on
undo. Its second angle starts with a deliberately wrong offset, so a sync that
does nothing cannot pass — and with the subtraction removed it reports +0.00s
and fails.

**Not done: syncing by marker or by in point.** Both need somewhere to hang a
per-file marker, and nothing in the model has one yet; inventing it for a
feature nothing else uses would be a field that exists to be read by one
function.

---

#### Phase 5y — time remapping and freeze frames ✅

Speed that varies along a clip, and the freeze that falls out of it.
[ADR-014](adr/0014-time-remapping-stores-frames-not-speed.md).

**The curve stores which frame, not how fast.** Keyframing the speed is how the
control usually presents itself, and it means integrating to find a frame — an
integral that accumulates its own error along the clip, so a hold set to end on
frame 300 ends on 299 or 302 depending how far into the timeline it sits.
Storing the frame makes every keyframe exact by construction, and leaves speed
as the slope.

**It collapses three features into one.** A freeze is a curve that does not
change; reverse is a curve that falls; a ramp is a bezier segment. All of them
are edited in the keyframe lane that already exists, by the same operations,
with the same undo — and both render paths, the render cache and export got it
for free, because they already ask the clip which source time to read and that
is the one function that changed.

**The remap is keyed in the clip's un-remapped source time, and so is
everything else.** The first half is forced: reading the remap through the
remap it defines is circular. The second half is a choice — a fade drawn across
a frozen shot still fades, because stopping the picture is not a statement
about the graphics on top of it. A test requires exactly that: an opacity ramp
over a total freeze is halfway down at the halfway point.

**It is a picture operation; the sound runs at the clip's own speed.** Retiming
a signal is resampling it, and a remap changes rate continuously — a varispeed
resampler is real work, worth doing properly rather than badly in passing. So
audio reads through `Clip::baseSourceTimeAt`, and the name says which mapping
it is rather than leaving it to be discovered.

**A switch, not a stopwatch.** Every other animatable parameter has a value the
clip holds when nothing is animated. A remap that is not animated is the clip's
ordinary mapping — there is nothing for a stopwatch to turn off *to*, and
inventing one would put "frozen on the first frame" one click away from every
clip in the project. Turning it on seeds the identity: two keyframes holding
exactly what the clip already plays, because switching it on is a statement
about what can be edited next, not an edit.

**Frames are chosen to the nearest, and clamped at the front of the file.** A
continuous curve against discrete media otherwise shows every frame slightly
late and holds the last frame of a ramp for two; a curve dragged below zero
otherwise asks for frames that do not exist and the clip silently stops
drawing, which looks like a broken decode.

The self-test finds a lit frame and a black one in the real preview, freezes on
the lit one through the panel's own button, and requires the black frame to
show the lit picture — then unticks the box and requires it to go black again.
With the remap lookup disabled it reports 0.0 and fails.

**Not done: frame blending and optical flow.** A slowed clip repeats frames.
That is a rendering feature on top of this one, and this one has to be right
first.

---

#### Phase 5z — keying ✅

Making part of a clip transparent, so what is under it shows through.

**Distance from a colour, not a region of colour space.** This looks like the
HSL qualifier from Phase 5g and answers a different question. A qualifier
describes an area — these hues, this much saturation, these tones — because
grading a *kind* of thing is what secondaries are for. A key starts with an
eyedropper on one pixel of a screen, and "everything within this much of that"
is what somebody means by it. Expressing that as three windows turns one number
into six and gives a box where a ball is wanted, so a screen with a warm corner
keys too much or too little depending which face of the box it crosses.

**The distance is measured in chromaticity.** Brightness is divided out before
comparing, so a shadow on the screen is the same colour as the lit part of it.
A distance on the raw values instead keys the top of an unevenly lit screen and
leaves the bottom — which is every screen. There is a test for exactly that: the
same colour at a quarter of the brightness has to key identically.

**Black in front of the screen is a subject, not a hole.** Near zero there is no
reliable colour to measure, and dividing by it turns sensor noise into a hue.
Below an intensity floor the pixel is kept.

**Spill suppression, and openly a heuristic.** A key removes the screen; it does
not remove the green bouncing off it onto somebody's shoulder, and that
difference is what separates a composite from a cut-out. The dominant channel of
the key is clamped towards the mean of the other two, blended by an amount.
Separating the light that bounced from the light that belongs to the subject
properly would need to know what the subject would have looked like, which is
the one thing nobody has. What this does instead is the operation that removes
fringing on real footage, cheap enough to run per pixel on both paths — and it
never *adds* spill, because taking out more than there is tints the subject
magenta, which is the classic over-suppressed composite.

**The key runs before the grade, on the ungraded colour.** A key is a
measurement of what the camera saw. Running it afterwards would mean every
adjustment to the look silently moved the edges of the matte.

**And after the sample, not before it.** The shader keys what comes back from
the texture unit, so the CPU does too; keying first would mean a scaled clip was
keyed at a different resolution than it is drawn at, and the two paths would
disagree along every edge.

**A matte view, for the same reason the qualifier has one.** Judging a key by
looking at the composite is guesswork, and the holes that matter are the ones
too faint to see against whatever happens to be underneath. It is drawn opaque,
so a hole in the matte shows as grey rather than as a hole. Like the qualifier's
mask view it is deliberately not saved: it is how somebody is working right now,
not something about the cut.

This is the first effect that changes *alpha* rather than colour, so a
disagreement between the two paths shows up as an edge that is soft in preview
and hard in the export — or the other way round, which is worse, because the
export is the one nobody watches all the way through. The golden-frame test
covers five keys, including a blue screen and the matte view, and passed against
the CPU reference first time. The self-test puts a green rectangle over the
picture in the real preview, keys it out through the panel's own combo box, and
requires the picture underneath to come back — then switches the key off and
requires the green to return, so what was measured was the key and not the clip
vanishing for some other reason. With the shader's key uniform forced off it
reports the screen still covering the frame and fails.

**Not done: an eyedropper.** Picking the key colour off the picture is the
control this obviously wants, and it needs the monitor to hand back the pixel
under a click — a small piece of plumbing that does not exist yet. The numbers
are typed in the meantime.

**Not done: matte choking, blur and edge detail.** Those are operations on a
matte once it exists, and the matte has to be right first.

---

#### Phase 6a — the effect stack ✅

An ordered list of effects on a clip, and the first two to fill it.

**A list, because order is what a list has and a set of fields does not.**
Every effect so far has been a field on `Clip` — a grade, curves, a secondary, a
LUT, a mask, a key — and that works right up to the point where two of them
commute differently. Blurring and then sharpening is not the same picture as
sharpening and then blurring, and somebody has to be able to say which they
meant. §7.4 asks for a *library*; a library cannot be a field each.

**Parameters come from a table, not from code.** An effect kind names the
parameters it takes, with a range, a step and a default. The renderer reads that
table and so does the panel, which is what makes "adding an effect is data"
true rather than aspirational: the panel has a fixed pool of parameter rows it
relabels, and a new effect needs no new widget. It also means a default cannot
be one value in the model and another in the control.

**A freshly added effect changes nothing.** Both defaults are the identity — a
blur of no radius, a sharpen of no amount. Adding an effect and having the
picture jump makes it impossible to tell what the effect did from what it
happened to be set to.

**A parameter that belongs to another effect is refused, not ignored.** Stored,
it would be written to the file, read back, and never used: a setting somebody
made that quietly does nothing.

**The stack runs on the clip's image, before the key and the grade.** These are
the only stage that reads a pixel's *neighbours*; everything else in the
pipeline is per-pixel and runs at sample time. Putting the spatial stage first
is what keeps the rest a single pass. The consequence is real and worth stating:
a blur before a key softens the matte. Sometimes that is exactly what is wanted
and sometimes it is not, and letting the stack be ordered against the grade is a
later phase.

**The blur is separable, premultiplied, and in linear light.** Separable because
a radius that costs a thousand samples as a square costs sixty-four as two
lines — the difference between a slider somebody drags and one they set and wait
for. Premultiplied because blurring straight colours pulls the colour of
transparent pixels into visible ones, which is the black halo around anything
blurred over an alpha edge; there is a test that the soft edge keeps colour
equal to coverage. Linear because the average of two brightnesses is the
brightness of their average only in linear, and a blur is nothing but averages.
The kernel is normalised, so a blur redistributes light rather than changing how
much there is — an unnormalised one darkens in proportion to the radius, which
reads as an exposure bug rather than a blur one.

**Sharpening is unsharp masking, and leaves alpha alone.** The detail is
whatever the blur threw away. Sharpening coverage as well would put a bright rim
along every edge of a keyed subject, which is the artefact people blame the key
for.

**On the GPU, the whole frame goes to the CPU compositor.** An effect needs a
pixel's neighbours, and this compositor queues every draw into one sampling
pass. That is the same fallback adjustment layers established in Phase 5v, and
the render cache from 5w is what makes it play back — the two earlier phases
paying for this one. The fast path is a per-clip pre-pass into a texture,
exactly the shape the Y'CbCr conversion already uses; it is a performance change
and can land later without anything above it moving.

The self-test adds a blur through the panel's own buttons and counts bright
pixels rather than mean brightness — a blur redistributes light, so the mean
barely moves and a test of it would pass on a blur that did nothing. What a blur
actually does is turn hard edges into gradients, and 178188 bright pixels became
145456. Writing it found a real bug: rebuilding the effect list emits
`currentRowChanged`, which is wired back to the function doing the rebuilding,
so it re-entered itself having already thrown away the selection it was in the
middle of restoring — and every parameter edit was silently dropped.

**Not done: keyframing an effect's parameters.** The keyframing engine is keyed
by `model::Param`, which is a fixed enum of the clip's own fields; reaching a
parameter inside an ordered list needs a way to name it that survives the list
being reordered. That is a design question, not an oversight.

**Not done: the rest of the library.** Distort and stylize are more entries in
the same table now that there is a table to put them in.

---

#### Phase 6b — keyframed effect parameters ✅

The design question Phase 6a left open: how to name a parameter that lives
inside a list somebody can reorder.

**The curves live on the effect, not in the clip's animation.** That is the
whole answer. The obvious alternative is to give every effect an id and key the
clip's `ClipAnimation` by (id, parameter) — which means two structures that have
to be kept agreeing through every add, remove and reorder, and an id generator,
and a rule for what happens when a stack is copied between clips. Putting the
curves inside the effect makes all of it free: moving an effect carries its
animation, deleting one takes its curves, copying a clip copies both together,
and there is nothing to remember. The reason `ClipAnimation` is separate from
`Transform` does not apply here — a transform's fields are fixed and always
present, and an effect is an object with a lifetime of its own.

**Keyed in the clip's source time, like every other curve** (ADR-008), so an
effect keyframed against a moment in the footage stays on it through a trim.

**One command still writes the whole stack.** Phase 6a's argument was that three
composable commands are three ways to reach a stack no sequence of user actions
could produce; adding a keyframe did not change that, so there are no new
operations — the panel mutates a copy and pushes it, and breaks the merge run so
that a keyframe is its own undo step.

**Editing an animated parameter writes a keyframe, not a static value.**
Anything else means turning a knob and watching the number spring back the
moment the playhead moves. The spin box shows the value *at the playhead* for
the same reason: a control displaying the value a parameter had before it was
animated is a control that lies.

**The stopwatch and the diamond keep meaning different things.** Switching the
stopwatch on seeds a keyframe holding what is already showing, so the picture
does not move; switching it off keeps that value as the static one, so it does
not move then either. The diamond is inert until the stopwatch is on — a
keyframe on a parameter that is not animated has nowhere to go, and quietly
turning animation on would collapse the distinction between the two buttons.

**An animated effect counts as active even where its curve reads zero.**
Deciding otherwise would mean the renderer took one path on some frames of a
ramp and another on the rest.

**The curve codec is now shared.** Effect curves and clip curves are written by
one function and read by another, rather than two copies that would eventually
disagree about how a bezier handle is stored — the same consolidation that
Phase 5 needed after the transition-grade bug, done before the second copy
existed rather than after it drifted.

A test writing a ramp caught a wrong expectation rather than a wrong
implementation: a point just outside a blurred edge does not move one way as the
radius grows — it rises as the edge softens and falls again once the same light
is spread thin enough. The middle of the object does move one way, because a
blur conserves light, and that is what the test asserts now.

The self-test animates a blur through the same stopwatch every other parameter
has: 178188 bright pixels at the start of the ramp, 133708 at the end. With the
curve lookup removed it reports the same number twice and fails.

**Not done: an editor for effect curves.** The keyframes show in the timeline's
lane and can be dragged there, but the curve editor from Phase 5f is wired to
`model::Param` and does not know about effects yet.

---

#### Phase 6c — getting back to the source ✅

Three of §7.1's remaining gaps, and they are one subject: the relationship
between a clip on the timeline and the material behind it.

**A subclip is a named range of a media reference, not a new kind of media.**
It records where somebody said the good part is. Placing one makes an *ordinary*
clip whose source range starts inside it, and from there the renderer, the media
source, the render cache and every edit operation carry on knowing nothing about
subclips at all. The alternative — a media reference with an offset — would mean
every read had to be translated, in a layer that currently resolves a path and
nothing else.

**So a clip made from a subclip can be trimmed past its edges.** Premiere can
restrict those trims. Doing that here would need a second kind of clip that
every trim, ripple, roll, slip and slide had to learn about, to enforce a
boundary somebody chose as a note to themselves. The subclip stays in the bin as
that note, and the cut is not constrained by it. That is a trade, and it is
written down rather than discovered.

**Subclips are added directly, not through a command**, like media: the bin is
not the cut, and nothing about a subclip changes what any sequence renders.
They are dropped on load when their media is missing — a subclip that cannot be
opened is worse in the bin than absent from it.

**Match frame has no operation at all.** The answer to "which frame of the file
is this" is `Clip::activeSourceTimeAt` — the mapping the renderer already asks
for. A second implementation of it would be a second thing to keep in step with
trims, speed, reverse, a multicam angle and a time remap, and it would fall
behind the first time one of those changed. So the feature is a key binding, a
lookup and a scrub.

**Replacing footage keeps the cut, and everything on the clip.** The whole
reason to replace is that the edit is right and the material is wrong — a graded
take for the ungraded one, a delivery for a placeholder. The grade, the effects,
the keyframes and the timeline range all stay. Where the new file is too short
to reach the old in point, the in point slides back rather than the clip
shortening: the clip's length *is* the cut. Where it is shorter than the clip
itself the operation is refused, because at that point there is no honest
answer and silently shortening would ripple an edit nobody asked to change.

The self-test drives all three through the real widgets — the monitor's subclip
button, the F key, the bin's Replace — and each one fails when its feature is
removed: match frame lands on frame 0 instead of 70, and the replacement never
reaches the clip.

Writing it turned up a discipline the self-test now states: this block moves the
playhead a long way, the timeline scrolls to follow, and the blocks that aim
real mouse events at fixed coordinates have to run before it. Placed in the
middle it silently broke a rubber-band selection two blocks later.

**Not done: a bin that can rename or delete a subclip.** The model can, the list
cannot; the bin has no editing affordances at all yet, and giving one just to
subclips would be an odd place to start.

---

#### Phase 6d — saving, autosave and recovery ✅

Until this phase the application could open a project, export a master, export
OTIO and export subtitles — and could not save. Every phase since 4a has been
adding things somebody would lose.

**A save writes beside the file and renames over it.** A truncating write
destroys the old project the instant it opens the file, so a crash, a full disk
or a pulled cable partway through leaves neither the old version nor the new —
and the moment somebody is most likely to lose a day's work is the moment they
were saving it. Rename within a directory is atomic: either the new file is
there whole or the old one still is. The temporary goes in the same directory
for that reason, because a rename across filesystems is a copy and a delete,
which is the non-atomic operation this exists to avoid.

**Modified is a position in the history, not a flag.** Undoing back to the
state that was saved reports the project as unmodified again, because that is
what it is — and a "modified" marker that will not go away is one people stop
reading. Three cases had to be got right, and each has a test:

- A merged command does not move the position but does change the project.
  Without handling it, dragging a value after a save would leave the project
  reading as unmodified while it had changed.
- A new command discards the redo branch. If the saved state was on it, that
  state can no longer be returned to, so it can no longer be recognised.
- The history is bounded. When the saved state falls off the end it is
  forgotten, because it can no longer be undone back to.

In all three the answer is to admit the saved state is unknown, which reports
"modified". Guessing the other way is cheap to write and costs somebody their
work.

**Autosave writes a recovery file beside the project, never into it.**
Autosaving over the file somebody last chose to save is making a decision they
did not make. The recovery file is named after the project and sits next to it —
not in a temporary directory, which is somewhere nobody finds and the operating
system may clear out from under them. It is offered on the next open only when
it is *newer* than the project, because an older one describes work the last
real save already includes. An explicit save deletes it: what it described is
now in the project, and being asked about it next time would be alarming.

**Every thirty seconds, and only when something changed.** A timer that writes
regardless rewrites an untouched project all day; one that writes on every edit
stalls a drag on disk.

**Autosave failures are silent.** An autosave is something the program does on
its own, and a dialog interrupting somebody mid-edit to report it is worse than
the missing file — the next explicit save reports the same problem at a moment
they are expecting an answer.

**There is no dialog on the way out.** "You have unsaved changes — save?" is a
question whose answer is almost always yes, asked at the moment somebody has
already decided to leave. Closing writes the recovery file instead, so quitting
is always instant and never loses anything, and the file they last chose to save
stays as they left it. Recovered work saves back to the *project*, not to the
recovery file, or the real project would stay stale for ever.

The self-test writes to a scratch path rather than over the fixture — a
self-test that rewrites its own input is one whose second run tests something
else. It saves through the button, checks the title stops saying modified,
reloads the file and finds the edit in it, then makes another edit, autosaves,
and requires the project file's size to be unchanged. With autosave pointed at
the project it reports nothing to recover and fails; with `markSaved` gutted it
fails on the first check.

**Not done: a New Project, and opening one from inside the window.** The
application still takes its project on the command line. That is a shell around
what this phase built rather than more of it.

---

#### Phase 6e — New and Open ✅

The shell Phase 6d left off: until now the application took its project on the
command line and could not start one or switch to another.

**A new project has one sequence with one video track and one audio track.**
Not none: a window with nothing to show has to special-case every panel, and "no
sequence" is a state somebody can only leave by making one anyway. A timeline
with no tracks has nowhere to drop anything, and the first thing anybody does is
drop something.

**Its format comes from the first thing put on it, not from a dialog.** Asking
somebody to choose a rate and a frame size before they have opened any footage
is asking a question whose answer is in the footage. So a new sequence starts at
a placeholder shape and `edit::makeConformSequence` replaces it when the first
clip arrives.

**Conforming is refused the moment there is anything to retime.** Every clip's
timeline range is expressed at the sequence's rate, so changing it under a cut
would retime the whole thing — silently, and by a ratio nobody was thinking
about. An empty sequence has nothing to disagree with, which is the only moment
this is both safe and useful.

**The rule lives in the bin, not in the model.** It is a decision about what
somebody meant, and those belong where the interaction is; the alternative is
every edit operation carrying a rule about when a sequence may change shape. The
operation itself enforces the safety condition, so calling it later cannot do
harm.

**Opening loads everything before replacing anything.** A file that cannot be
read leaves the window on the project it already had. Half-swapping a window is
how a program ends up showing one project's timeline over another's media.

**Switching projects asks nothing**, the same bargain closing makes in 6d:
whatever is unsaved goes to its recovery file first. The command history and the
render cache go with the project that has left — a cached frame's recipe covers
what is in a sequence, not which project it came from, so keeping one would
serve the old project's picture for the new one's timeline.

**`openProject` reports rather than reporting and deciding how to say so.** The
button puts the message on screen; a caller with no screen gets the same answer
without a dialog it cannot dismiss. That separation was not tidiness — the
self-test aborted inside Qt's offscreen platform on a modal it could never
close, which is exactly the shape of a headless caller.

Writing the self-test also ran straight into this file's oldest hazard: the
block read the frame rate from the `sequence` reference the function has held
since the top, and by that point two earlier blocks have added a track and a
sequence, both of which reallocate. It aborted inside rational arithmetic on the
wreckage, the same way the Phase 5t bug presented. The block now looks the
sequence up when it needs it, and says why.

**Not done: recent projects, and more than one window.** Both are shells around
this shell, and neither changes what the program can do.

---

#### Fix — a pipeline built against a freed render pass descriptor ✅

Reported as "unexpected exits". Six crash reports on this machine, in two
signatures; one was a self-test of mine reading a dangling reference, and the
other was real and in the compositor.

`GpuCompositor` kept `intermediatePass` as a **raw pointer into whichever
staging slot happened to create the first one**. Two ordinary things then had to
happen together, and opening a second project does both at once:

- A change of **output** size recreates the output target and clears the
  conversion pipelines, so the next `drawSource` rebuilds one.
- A change of **source** size makes the staging slot destroy and recreate its
  own render pass descriptor — and the `if (intermediatePass == nullptr)` guard
  meant the cached pointer was never updated to the replacement.

The rebuilt pipeline was then handed freed memory. Metal read a colour
attachment with an invalid pixel format and aborted the process from inside an
ordinary repaint, with nothing in the stack between `paintEvent` and the
assertion to say why.

**Every staging surface now shares one descriptor, owned by the compositor.**
All of them have the same texture format, and a QRhi render pass descriptor is
compatible with any target of that format — which is what
`newCompatibleRenderPassDescriptor` means. So there is one, it is a
`unique_ptr` on the compositor's state, and it lives as long as the pipelines
that reference it. The per-slot descriptor is gone, which is what makes the bug
unreachable rather than merely unlikely.

The regression test is the sequence that reproduces it: draw at one output and
source size, then at another, then back. It aborted before the fix and passes
after. Phase 6e is what made it easy to reach — Open is the one action that
changes both sizes in a single step — but the defect predates it, and any
project mixing resolutions could have found it.

**Worth noting for next time:** asan found nothing here. The freed object was a
Qt allocation reached through Metal's own validation, so the process died before
any instrumented read. The crash reports on disk were what identified it, and
the stack named the exact function.

---

#### Phase 6f — scene edit detection ✅

Finding where one shot becomes the next, inside a clip that arrived as one
continuous file.

**The measurement is a histogram, not a difference of pixels.** A pan changes
every pixel and almost no bin; a cut changes the distribution. Comparing pixels
would report a camera move as an edit, which is the failure that makes a
detector something people switch off.

**Bins are indexed through the same warp the curve tables use.** Linear light is
mostly small numbers, so binning it directly puts nearly every pixel of an
ordinary picture in the bottom two bins and two quite different shots come out
identical. The renderer already has one answer to "where does this brightness
sit", and this uses it rather than inventing a second.

**A flash is not a cut, and that is the hard half.** A camera flash differs
enormously from the frame before it and then goes straight back to looking
exactly like it. So confirmation compares *the shot before with the shot after*,
a few frames either side, rather than the two frames on the boundary. Both
halves of that matter, and the second was found by a failing test: without
looking forward a flash is reported going in, and without looking *back* it is
reported coming out — because the frame before that boundary is the flash
itself, and the flash never returns.

**The first shot is a shot.** The minimum length applies from the start of the
material, not only between cuts. The real footage found this: it reported a cut
one frame in, which would split a frame off the head of the clip — not an edit
anybody can use, and there is not enough material before it to tell a cut from
the shot simply beginning.

**A dissolve passes unremarked**, because it moves the distribution a little at
a time and no pair of frames crosses the threshold. That is the honest outcome:
a dissolve is not a cut, and reporting one in the middle of it would split a
shot where nobody made an edit. Writing that test also caught a bad *fixture*
rather than bad code — flat colour frames have a single bin per channel, so the
smallest change moves all of it and reads as a total change. The test now
dissolves between two textured shots, which is what footage looks like.

**Cutting is one command.** Detecting the cuts in a shot is one decision, so
undoing it gives back the clip somebody had rather than peeling the cuts off one
at a time in an order they never chose. Points that land in a gap or on an
existing cut are skipped rather than refusing the whole list.

The end-to-end test is the fixture itself: a continuous take with nine white
flashes in it. The assertion is that detection finds **nothing** — and it fails
loudly if the guard goes, because with confirmation switched off the same take
comes back in ten pieces.

**Also fixed here: an intermittent self-test failure.** One run in ten or so
reported "no lit frame to fade" on footage that is plainly lit. A single
`processEvents` is not a guarantee of a repaint — the widget schedules one, and
whether it happens before the grab depends on what else the loop has to do, so
a scan could read the frame before the playhead moved. Grabs now wait for the
monitor's frame counter to move, bounded so a monitor that never draws reports
what it saw rather than hanging. Ten consecutive runs pass; the original failure
was rare enough that this is evidence and not proof.

**Not done: detecting cuts across a whole timeline, and markers instead of
cuts.** Both are shells around this; the analysis is the part that had to be
right.

---

#### Phase 6g — audio roles and auto-ducking ✅

What a piece of sound is for, and the one decision that reads it.

**A role belongs to the clip, not to the track.** Material moves; somebody who
drags a line of dialogue onto the music track has not made it music. The list is
four long on purpose — dialogue, music, effects, ambience — because those are
what a mix is built out of and what an automatic decision can act on. A longer
taxonomy is one nobody maintains, with most clips left on whatever the default
happened to be. `Unassigned` is that default and is honest about it: a clip
nobody has classified is not dialogue, and treating it as such would duck the
music under every stray sound in the timeline.

**Ducking follows what is heard, not what is on the timeline.** A dialogue clip
with ten seconds of room tone at its head would otherwise duck the music for ten
seconds before anybody said anything. So the dialogue is read and its loudness
envelope decides — which also means a pause long enough to matter lifts the
music without anyone marking it. The envelope is `media::envelope` from Phase
5x, written for aligning multicam audio and doing the same job here.

**The answer is keyframes, not a live sidechain.** A compressor listening to
another track would be fewer moving parts and completely opaque: nothing on
screen would say why the music dipped, and nothing could be nudged when it
dipped in the wrong place. Keyframes are the automation somebody would have
drawn, on the parameter they would have drawn it on, and they can be dragged
afterwards. They are written in the clip's source time like every other curve
(ADR-008), so the ducking stays glued to the music through a trim.

**Down quickly, back slowly, and hold through the gaps.** Coming down late is
audible as the first word being buried; going up early is audible as a pump
under the pause between sentences. The hold is what stops the music lifting
between every sentence, which is more distracting than never ducking at all.

**It is a change to the level somebody set, not a replacement of it.** A bed
already pulled down six ducks to eighteen, not to twelve.

A failing test found something the model does that the options could not: **the
curve's keyframe times are quantised to frames**, so a fade shorter than one
lands on the same instant as the dip it precedes — and `Curve::set` replaces
rather than appends, so the level before the dip vanished and the ramp ran all
the way from the start of the clip. The emit now guarantees distinct times, and
the defaults are stated in seconds rather than in samples, which is how the
nonsense got in.

`edit::makeSetCurve` writes a whole curve as one command, because these curves
come from an analysis: undoing an auto-duck gives back the level somebody had
rather than removing two hundred keyframes one at a time.

The self-test drives it through the panel in both directions: as dialogue the
button is disabled, because a clip cannot duck under itself; as music with a
dialogue clip over it, 39 keyframes appear between −12 and 0 dB. Making it duck
on clip presence instead of on level collapses that to a flat −12 and fails.

**Not done: ducking a whole timeline in one action, and roles driving submixes.**
Both want a mix structure that does not exist yet; the role is the part that had
to come first.

---

#### Phase 6h — log and HDR footage into the working space ✅

§7.3's input half: getting what a camera actually wrote into the linear space
everything downstream assumes (ADR-005). Until now the renderer knew five
display curves, and **refused** PQ and HLG outright.

**Five new curves, and each is checked against its own specification.** S-Log3,
V-Log, LogC3 (EI 800), PQ and HLG. The constants are written out rather than
factored, because the only useful check on them is reading them against the
document they came from — so the tests assert the published anchors: S-Log3 puts
18% grey at code 420 of 1023 and 90% white at 598, V-Log puts grey at 42.3 IRE,
LogC3 at 39.1%. All four landed first try, which is the outcome that makes the
constants believable.

**A log curve carries several stops above white**, and that headroom is what a
grade is made from. There is a test that says so directly: Rec.709 runs out at
1.0 and each log curve is past 4.0 there.

**PQ carries absolute light and the working space carries relative light**, so
the two need a reference to agree on. 100 cd/m² — SDR diffuse white — is the one
chosen, which puts graphic white at 1.0 and leaves specular highlights above it,
exactly what a scene-linear space is for. Any other choice makes a correctly
exposed HDR shot arrive a hundred times too dark or too bright, and the number
is in one named constant on the CPU with the shader's copy pointing at it.

**The refusal stayed, aimed at what actually cannot be done.** `Unknown` is
still refused, because guessing at a curve produces a picture that is wrong in a
way nobody can see is a *tag* rather than the footage. Two tests had encoded
"HDR is refused" as the desired behaviour and were rewritten rather than
deleted: the thing they were protecting is still protected, it just has a
narrower target.

**Camera log is almost never tagged**, so there is a per-media override. A
container has a number for BT.709 and none for S-Log3, so an S-Log3 file says
BT.709 and decodes to a washed-out picture that looks, at a glance, like
somebody underexposed. Nothing in the pixels can tell them apart — a flat shot
and a log shot are the same picture — so the only honest mechanism is a person
saying so, in the bin, where the list of files is. It sits on the *media*, not
the clip: it is a fact about the file, and every clip reading that file needs
the same answer. The bin shows it in the row, because a file being read as
something other than what it claims is exactly the setting somebody forgets they
made.

Both read paths go through one `applyOverride`, because the CPU converts to the
working space inside the media source and the GPU converts in a shader from the
frame's own tag — a correction applied to one and not the other would show up
only in the export.

The golden-frame test covers all five curves against the CPU reference and
passed first run. Its tolerance is looser than the display curves get, and
honestly so: these carry values far above 1.0, so the sampled table's
interpolation error scales with them; relative to the values involved it is the
same error.

**What the end-to-end test could not check, and why.** The fixture is pure black
and pure white with nothing in between — black stays black under any curve and
white clips under all of them, so the preview cannot tell two curves apart on
it. The self-test therefore checks that the override reaches *the decoder*, and
that the preview renders log footage rather than refusing it; the curves
themselves are the golden-frame test's job. Stated here rather than papered over
with a weaker assertion.

**Not done: OCIO, output transforms, and tone mapping.** Footage now arrives in
the working space correctly; choosing a *display* to send it back out to is the
other half of colour management, and HDR delivery needs a tone map that this
does not attempt. An HDR file will grade correctly and export through an SDR
curve by clipping, which is honest but is not a deliverable.

---

#### Phase 6i — delivery: the output curve and the highlight rolloff ✅

The counterpart to Phase 6h. That phase got footage *into* the working space
correctly; this one decides what it looks like coming out.

**Delivery is a property of the sequence, not of the export.** The curve editor
and the scopes are drawn against the display signal (ADR-010), so choosing the
curve at export time would mean grading against one and delivering through
another. Both render paths now read it from the sequence rather than assuming
Rec.709, and they read it from the same place — a display curve taken from two
sources is exactly the kind of disagreement that only shows up in an export
somebody has already signed off.

**The rolloff is exactly the identity below its knee.** Not an optimisation: a
tone map that touched the midtones would silently change every existing
deliverable, and the first anybody would know is a re-export not matching the
one that was signed off. The default knee is 1 — no rolloff, the encoder clips —
which is what this program did before there was a choice, and is why
`verify-frame-exact.sh` still reports 46 frames byte-identical to FFmpeg.

**A failing test changed the curve's shape.** The first version spent its
headroom exponentially, which is smooth and joins the identity cleanly — and
underflows to exactly 1 about four and a half stops above the knee. That sounds
far away until you remember Phase 6h: PQ arrives with values up to 100, so the
top two stops of an HDR signal would have come out as flat white. The rational
form stays distinct until the input is millions of times the headroom, which no
signal is. The test that caught it asks for strict monotonicity out to 100.

**Tone mapping stays outside the encoder**, which is a decision the encoder's
own documentation had already made: its job is to write what it is given and
clip what does not fit, and making sure nothing needs clipping is a separate
step. Keeping them apart is what lets a project with nothing above white go
through untouched.

**Curves with no formula are refused here too**, the same rule the input side
uses: nothing could encode through `Unknown`, so it is rejected when the
delivery is set rather than left to fail at the encoder.

The revert checks are worth recording. Removing the identity-below-the-knee
guard fails 19 assertions across three tests. Removing the encoder's call to the
rolloff failed *nothing* — the unit tests covered `toneMap` and
`toDisplayRgb24` separately but nothing checked that the encoder joined them up.
That gap is now a real export: two files written from the same graded sequence,
one clipping and one rolling off, decoded back and compared. It fails when the
encoder ignores the setting.

**Not done: the preview does not tone map.** The rolloff is applied on the way
out, so a graded highlight looks clipped on screen and rolled off in the file.
That is a preview/export divergence of exactly the kind this project has spent
several phases removing, and it is here because the present pass is a separate
shader path; it wants the knee as a uniform and one branch, and it wants doing
before anybody grades HDR in anger.

**Also not done: OCIO, and output primaries.** The curve is chosen; the gamut is
still Rec.709 throughout.

---

#### Phase 6j — the preview tone maps too, and the colour wheels ✅

Two things: closing the divergence Phase 6i opened, and the last Lumetri control
whose absence was an engine gap rather than a widget gap.

**The preview now rolls off its highlights the way the encoder does.** 6i added
the rolloff on the way out and left the screen clipping, which is exactly the
preview/export disagreement every parity test in this project exists to prevent
— and it was self-inflicted, which is the worst kind. The present pass takes the
knee as a uniform and applies the same curve on straight colour; the composite
draws write 1 into that slot on every draw, because a draw that left it alone
would inherit whatever the last present wrote and tone map a clip on its way
*into* the frame rather than the frame on its way to the screen. The golden-frame
test compares a presented ramp against `render::toneMap` on the CPU and fails
when the shader branch goes.

**The wheels are an ASC CDL, and that is the whole decision.** Every grading tool
has three wheels and almost none of them mean quite the same arithmetic by it.
The CDL is the one definition other programs agree on, so a grade set here can be
handed to somebody else and land the same way — and the numbers can go out to a
.cdl or an EDL later without being reinterpreted. Per channel,
`out = (in * slope + offset) ^ power`: slope scales so it moves highlights and
leaves black alone, offset adds so it lifts black off zero, power is a gamma so
it pins both ends and moves the middle. That is what makes three wheels *feel*
like shadows, midtones and highlights when every one of them touches the whole
picture, and there is a test for each of those three sentences.

**The wheels run before contrast.** Both shape the midtones, and doing the CDL
first means the contrast control pivots about middle grey of the picture
somebody is actually looking at — which is what they expect from the control
they reached for last.

**Nine numbers rather than three pucks.** The arithmetic is what makes a grade,
and a puck is a way of typing two of these at once; a circular control can be
put in front of exactly these values later without anything behind them moving.
Presentation, not engine, and said so rather than left as an implied gap.

**Negative light stops at zero rather than becoming a NaN**, on both paths — a
fractional power of a negative number has no value, and one NaN spreads through
everything it is averaged with, so a single bad pixel would take a whole blurred
region with it.

---

#### Phase 6k — vignette, and one argument for clip shading ✅

**The refactor came first, and not for tidiness.** Adding the vignette would
have made the eleventh positional parameter of `drawTransformed`, the seventh
nullable pointer in a row. That is the exact shape that once let a patch add
colour correction to two of three draw sites and miss the third — a transition
that went two phases with its outgoing half ungraded. `ClipShading` collects
them, so a call site reads as what it sets rather than as a column of nullptrs,
and adding a stage is one field instead of one more place to get the order
wrong. The golden-frame tests are what made the change safe to do mechanically:
they compare against a CPU reference pixel by pixel, so a swapped argument would
have shown up immediately. None did.

**A vignette darkens; a mask makes holes.** `Mask` already carried the
observation that a vignette and a spotlight are the same shape with the
inversion flipped, and that is still true of the *geometry* — this reuses the
same coordinates, the same smoothstep, and therefore falls off with the same
shape as a feathered mask and a qualifier. What differs is what the shape
multiplies: a mask scales coverage, so the clip underneath shows through; a
vignette scales brightness, so it does not. There is a test that stacks a blue
clip over a red one and requires no red in the corner.

**Roundness is the choice between the frame's shape and a lens's.** 1 gives a
widescreen frame a widescreen oval; 0 gives a circle in pixels, which is what a
lens actually casts. The test puts them on a 128×32 frame, where the two
disagree most.

**It is in the colour group even though the arithmetic is geometry**, because
that is where somebody reaches for it.

Four-case GPU golden-frame test, passing first run; six core tests; and a
self-test that pulls the corners down on a lit frame through the panel.

---

#### Phase 6l — baking a look out as a .cube ✅

**A look file carries exactly what `gradePixel` does** — the primary, the
wheels, the tone curves, a look LUT and the secondary — because that is
precisely the part of a clip's look that is a function of colour and nothing
else. The secondary belongs in that list and it is worth saying why: an HSL
qualifier selects *by colour*, so it bakes perfectly well. What cannot go is
anything that depends on where a pixel is, or on whether it is there at all.

**What cannot be carried is said before the file is written, not after.** A look
file that silently does not match the shot it came from is worse than no look
file, and the moment to find out is while somebody is still deciding whether to
write it. So the bake reports its omissions — the mask, the vignette, the key,
the effects — and the panel puts them in front of somebody as a question rather
than a notice.

**A grade that lifts past white is one of those omissions.** A .cube has a 0..1
domain, and there is nowhere in it for light above white. Writing the test found
something worth knowing: a *white balance shift alone* is enough to trigger it,
because warming a picture multiplies the red channel and pure white lands above
one. That is not a bug in the bake, it is the honest answer, and it is now the
first thing the self-test says about the difference between a look and a shot.

**The cube is display-referred, through the sequence's delivery curve.** That is
what every program reading a .cube expects, and it is the same curve the grade
was judged against on the scopes — so the file describes the look somebody
actually approved rather than a linear-light one nobody looked at.

The round trip is the test: bake a grade, read it back through the `CubeLut`
parser this project already had, and require the same colour out. A neutral clip
must bake to an identity cube, and a two-entry cube with only red touched must
come back with only red touched — which is what catches the channel order, the
one mistake that produces a file that loads without complaint and swaps two
channels of every look.

---

#### Phase 6m — comparison view ✅

Holding a frame and grading the next one against it.

**A way of looking, not part of the cut.** None of it is saved: not the
reference, not the split, not the arrangement. It is the same rule the
qualifier's mask view and the keyer's matte view follow — reopening a project
into a half-and-half screen would be baffling, and nothing here reaches an
export.

**Turning it on takes the frame that is showing.** That is the gesture:
somebody looks at a shot they like and says "against this". Asking them to
nominate a reference first would put a step between the thought and the thing.

**A split copies; it does not resample.** The whole point of putting two frames
either side of a line is judging a difference in *detail*, and resampling one
side would invent a difference of its own. There is a test that puts a
single-pixel feature on one side and requires it to survive exactly. Side by
side does scale — evenly, and centred, because a comparison that stretched one
shot would be worse than useless for judging a grade.

**The divide is drawn.** Without it, two similar grades read as one picture with
an odd seam and somebody spends a while working out which side they are looking
at.

**It goes down the CPU path**, the one Phase 5v built for adjustment layers, for
a related reason: the GPU graph composites one instant into one target, and
asking it for two would mean a second target and a restructure of the draw loop
for something nobody exports. Phase 5w's render cache is what makes that
affordable — the held frame is composited once and read thereafter, so grading
against a still costs nothing.

Building it produced a mistake worth recording: the first version called
`draw` without opening a frame on the command buffer, which the compositor
reported as "draw outside a frame" rather than drawing nothing or crashing. The
error said exactly what was wrong, which is the whole argument for the
compositor checking its own preconditions instead of trusting callers.

---

#### Phase 6n — shot matching ✅

Working out the correction that puts one shot where another sits.

**Three anchors per channel, not the average.** Matching means alone moves a
shot bodily and leaves its contrast wrong; matching means and spreads gets the
contrast and pins nothing in particular. A shadow, a midtone and a highlight are
what a colourist is actually looking at — and they land exactly on the three
terms of a CDL: slope and offset put the two ends where they belong, and the
power takes the middle. So the answer comes back as the same three wheels
somebody would have set by hand, and can be nudged afterwards rather than
accepted or thrown away.

**Percentiles, not extremes.** One clipped highlight or one dead pixel would
otherwise decide the whole correction.

**It reports rather than pronounces.** Both distances come back — how far apart
the two shots were, and how far apart they are now — because "how much did this
move" is the question somebody actually has. When the correction is one no
colourist would dial, the answer is marked unusable and *nothing is applied*.
The numbers are still there for anyone who wants them anyway: refusing to
answer is different from refusing to act.

**Writing the self-test found a real defect.** This fixture's lit frames are
flat white, and a match built from three anchors needs three distinguishable
anchors — but the first version measured no range in any channel, corrected
nothing, and reported itself *usable*. Two frames half a stop apart, a
correction that does nothing, and a confident green light: precisely the
confidently-wrong outcome the design exists to avoid. It now says "there is no
range in those frames to match on" and applies nothing.

**And then the fixture had to earn its keep.** With that fixed, nothing in this
project's test footage could exercise a real match — flat white and flat black
throughout. A vignette makes a gradient out of a flat frame, so the self-test
puts one on both clips and matches through the real window: two shots 0.2509
apart, 0.0000 after. Using a feature already here beat inventing a fixture, and
kept the measurement on the real pipeline.

**Not done: matching a whole timeline to one shot.** The analysis is per pair;
applying it across a sequence is a loop and a question about which shot is the
reference, and that question is better answered by somebody looking at the cut.

---

#### §7.3 — where colour stands

Done: the working space and its rationale (ADR-005), primary correction, tone
curves and the curve editor, HSL secondaries and the qualifier UI, look LUTs in,
scopes, log and HDR input transforms with a per-media override, the delivery
curve, the highlight rolloff on both the export and the preview, and the colour
wheels.

Not done, and worth being plain about which is small and which is not:

- **Comparison view** is moderate: two frames side by side or wiped, which the
  compositor can already do, plus a way to say which frame is the reference.
- **Shot matching** is large and wants comparison view first. It is an analysis
  that has to produce a grade, and the honest version reports what it matched
  and how confidently rather than silently applying something.
- **OCIO** is the largest by far: a dependency, a config format, and a rework of
  every place that currently assumes Rec.709 primaries. The transfer half of
  colour management is now done; the gamut half is untouched, and it is not
  worth starting until somebody actually needs a working space that is not
  Rec.709.

---

---

### Phase 6o — wipes and slides §7.4 ✅

Two more transition kinds, and a bug they uncovered.

**Neither needed a new drawing primitive.** A slide is a transform and a wipe is
a mask, both of which the compositor already applies per clip — so the whole
feature is one function, `transitionShapeFor`, that says what the incoming clip
looks like part way through, and both render paths call it. The alternative,
each path working out where a wipe's edge is, is two answers to one question,
and this project has already paid for that once.

**A wipe's mask multiplies the clip's own rather than replacing it.** A masked
clip in a wipe should show where its mask says *and* where the wipe has got to;
replacing one with the other would silently drop whichever came second. That
made `maskCoverage` in the shader take its box and edge as arguments instead of
reading globals — a change that also fixed a latent trap next door, where "no
shape" was being written to the slot beside the shape flag and worked only
because the uniform block is zeroed on every draw.

**Direction is one word for both kinds.** A wipe to the right uncovers from the
left and a slide to the right enters from the left, so somebody who has chosen a
direction for one already knows what it does for the other.

**The bug: transitions never worked on generated clips.** The transition branch
reached straight for the decoder, so a dissolve between two title cards drew
nothing — and drew nothing *silently*, because a clip whose media cannot be read
is treated as a gap rather than an error. That is exactly the failure mode the
gap-not-error rule is supposed to buy tolerance for, and here it bought silence
instead. Both paths now resolve a clip's picture through one function that knows
about generated clips, with a second scratch buffer so the two halves of a
transition do not overwrite each other. It was found by a self-test that could
not otherwise be written: this fixture is black except on its flash frames, and
a wipe between two black shots measures nothing.

The end-to-end test is the discriminating one: at the midpoint a dissolve blends
both halves of the frame the same way (63.3 and 63.3) while a wipe puts one shot
on each side of the line (7.6 and 119.6). Make the wipe fall back to opacity and
it reports 63.3 twice and fails. The golden-frame test covers all four
directions at five points each.

**Not done: nested sequences in transitions.** They have the same shape of
problem the generated clips had, and the same fix would work; the composite that
draws a nest does not yet take a modified transform, which is what a slide would
need from it.

---

### Phase 6p — bezier masks §7.4 ✅

An arbitrary closed path as a mask shape, alongside the rectangle and the
ellipse.

**A corner is both handles at zero, not a flag.** A segment leaving a point with
no outgoing handle is a straight line to the next one, which is exactly what a
cubic with coincident controls degenerates to — so there is one evaluation path
rather than two, and a polygon is a special case of a path rather than a
separate thing that has to behave the same.

**Handles are stored, not derived.** A path is edited by dragging them, and a
curve reconstructed from its neighbours would move when a *different* point
moved — which is not what dragging one handle means.

**Always closed.** A mask is a question about what is inside, and an open path
has no inside; the alternative is a stroke, which is a different tool.

**Nonzero winding, not even-odd.** A five-pointed star traced in one direction
winds twice around its own middle: under even-odd it comes out hollow. Writing
that test is where a first attempt went wrong — a bow tie *looks* like the case
that distinguishes the two rules and is not, because its halves only touch at a
point, so the pixel there is genuinely half covered under either rule. The test
is a star now, and it fails under even-odd.

**Flattening follows the curve.** Subdivision by how far a segment strays from
its chord, not by a fixed count: a fixed count is wasteful on a nearly straight
segment and visibly faceted on a tight one, and a mask's edge is exactly where
facets show. There is a depth limit as well, because a degenerate curve from a
corrupt file would otherwise subdivide until the stack ran out.

**Antialiased in the fill, not by blurring afterwards.** Four sample rows per
pixel vertically and the exact span horizontally. A hard fill softened with a
blur moves the edge by half the blur, which is visible on a mask somebody has
lined up against something. Feather *is* a blur — of the coverage, using the
same separable Gaussian the effect stack uses, so a feathered path and a
feathered rectangle soften at the same rate.

**Coverage is a buffer, and the GPU falls back.** A path's coverage cannot be
answered per pixel from a handful of uniforms the way a rectangle's can, so a
clip with one composites on the CPU — the same bargain adjustment layers (5v)
and comparison view (6m) make, and the same render cache (5w) keeps it
affordable. The buffer is kept between frames and re-rasterised only when the
mask or the frame size changes.

**Not done: an editor.** Points are placed by an operation, not by dragging them
on the monitor, and everything above is what makes that editor possible rather
than a substitute for it. Tone curves and HSL secondaries both shipped this way
and got their editors in the phase after.

**Not done: the coverage texture.** Handing the GPU a mask texture per clip per
frame is the fast path and is worth having; nothing above changes when it lands,
because the coverage it would upload is the coverage the CPU already computes.

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

### Phase 6q — the mask editor §7.4 ✅

Handles on the picture, so a path mask is something you can draw rather than
only something the engine can render.

**An overlay widget, not something the renderer draws.** The renderer's job is
the picture that gets delivered; putting handles into the composite would mean
a flag threaded through every path that must never be set during an export. A
transparent `QWidget` over the monitor draws them instead, and the widget that
draws them is the one that receives the clicks.

**One place decides where the picture is.** The monitor letterboxes, so the
overlay asks it — `ProgramMonitor::pictureRect()` — rather than repeating the
arithmetic. Two places deciding would put the handles a few pixels off the
thing they are supposed to be on, and only at some window sizes.

**Convert a shape rather than start from nothing.** Drawing a path from scratch
needs a pen tool with its own modes; converting the rectangle or ellipse you
already have is one button with a result you can see before you touch it. The
conversion has to be invisible: the tests check a converted rectangle matches
the analytic one within 0.02 everywhere, and a converted ellipse — four cubics
with the 0.5523 circle constant — has no pixel off by more than 0.25.

**A corner has no handle to grab.** Handles are hit-tested before points, so a
handle pulled in tight is still reachable under the point that owns it. But a
corner's handles sit exactly *on* its point, so that rule made every corner
ungrabbable: the press landed on a zero-length handle and the drag bent the
curve instead of moving the point. Coincident handles are skipped now, which
matches the paint — a handle that is not drawn should not be grabbable. The
self-test caught it, but only after the test's own bug was fixed: it held a
*reference* to the point it was about to move, so it compared the moved point
against itself and would have passed on any behaviour at all.

**A drag is one undo step.** The moves merge and the release breaks the run, so
undo puts the point back where it started rather than replaying the drag a
mouse-move at a time.

**Alt-click deletes, and refuses at three points.** Fewer than three enclose
nothing, and a mask that vanished would look like the delete having gone
wrong.

The overlay hides itself when the selected clip has no path mask, so it never
swallows a click meant for the picture, and ignores presses that hit nothing so
they reach whatever is underneath.

Not done: a pen tool for drawing a path from nothing, mask feather handles on
the picture, and mask tracking.

### Phase 6r — the Cutline chrome §7.1 ✅

The window, dressed. Every phase since 4a added a panel and hung its controls
off the transport row; by 6q that row held eighteen buttons under the picture
and the application looked like what it was, a test harness with a compositor
behind it. This phase takes a design — Cutline, on the Nocturne palette — and
builds the furniture it asks for: a menu bar, a tool bar with a tool palette
and workspaces, one viewer with two pages, a timeline header, and a status
line.

**One place decides what colour anything is.** `app/Theme` holds the design
system's tokens — a ground, a surface, a text colour and two nine-step ramps
generated on one shared lightness scale — and builds both the palette Qt hands
to its own style and the stylesheet every control is drawn from. The panels
that paint themselves (the timeline, the meters, the burn-in) ask Theme for the
same tokens rather than keeping a second palette that drifts from it. The
keyframe self-test used to hunt for a literal `226,226,236` in the grab; it
asks the theme for the diamond's colour now, because a test that pins a
literal is a test that has to be chased every time the palette moves.

**Eighteen buttons became nine menus.** A row of buttons is fine at four and
illegible at eighteen: it says nothing about which of them belong together, and
it grows every phase. The menus are where the structure lives, and the actions
carry the object names the self-tests were already using — so the test that
saved a project by clicking a button now triggers the same action from the File
menu. Only modified shortcuts live on actions. A single letter bound
window-wide would fire while somebody was typing a title into a text field,
which is why the tool keys stay in the timeline's own key handler.

**Tools, because some gestures have no obvious chord.** Select, Blade, Trim,
Slip, Hand and Zoom. Trimming and moving still work under Select — that is what
a pointer is for — but cutting, slipping and panning are each somebody's whole
session, and a tool that stays picked is what makes a run of them fluent. Slip
finally reaches the interface: `makeSlip` has been in the edit engine since
Phase 2 with nothing calling it. A slip drag is measured in time rather than in
pixels, so it means the same thing at every zoom, and the anchor only advances
once a whole frame has been consumed — otherwise a slow drag at a high zoom
rounds itself away one mouse-move at a time.

**Workspaces are which panels are up.** Four, because there are four things
people do with an editor and each wants a different half of the window: the
scopes are dead weight while somebody is assembling, and the bin is dead weight
while they are grading. Each workspace remembers its own splitter state — one
saved arrangement restored into a different set of visible panels is a
collapsed bin and a mixer four pixels tall.

**One viewer, two pages.** Source and program were side by side, each with half
the width, which is not enough width to judge either on. They are one stack with
a segmented control now, and opening a clip from the bin — or a match frame —
switches to the source, because that is what the gesture asked for. It has a
cost the self-test found immediately: a monitor that is not the current page
does not repaint, so the render-cache check read zero hits until it asked for
the program back. Grabbing a hidden monitor still works; leaving it hidden and
expecting it to redraw does not.

**The burn-in is a widget, not a render.** Clip name, timecode, format and safe
guides sit on a transparent overlay over the monitor — the same argument Phase
6q made for the mask handles. The picture that leaves this application is the
picture, and an overlay that could reach an export is a flag somebody
eventually forgets to clear. It takes no mouse events, so the mask editor above
it still gets every click.

**A Qt layout given less room than its children need does not clip them, it
overlaps them.** The mixer strip is taller than the panel usually gets, and the
result was a meter painted over the solo button — visible in a screenshot and
invisible to every test. The strips scroll now. The meters are narrow bars in
the accent, and keep amber and red for the two states that are a warning: a
level that is simply fine should look like the rest of the interface.

**Icons are drawn, not fetched.** The design's tool palette is a row of
Phosphor glyphs. Vendoring an icon font is a build dependency and a licence;
taking the glyphs from whatever font happens to be installed gives a colour
emoji on one platform and an empty box on another. `app/Icons` paints the six
as painter paths at Phosphor's regular weight instead — stroked rather than
filled, so a cursor beside a magnifier reads as one set — in three colours for
the states a toolbar button has. The tooltip carries the shortcut: a palette is
aimed at rather than read, and the letter is one hover away for as long as it
takes to learn the shape. The transport keeps to characters, which are shapes
every font has.

The first version of the arrowheads had the sign of their barbs backwards, so
Trim and Slip drew their heads a pixel outside the box and came out as blobs.
It is the kind of mistake a unit test cannot see and a screenshot shows
immediately, which is the argument for looking at the window as part of
building it.

Not done: the design's media browser with thumbnails, bins as a tree, and
filter chips; a floating panel system; and the Deliver workspace is the export
dialog's neighbours rather than a page of its own.

### Phase 6r — the pen tool §7.4 ✅

Drawing a mask path from nothing, rather than only bending one converted from
a shape.

**The points live in the overlay until the path closes.** A mask needs three
points to enclose anything, so a partial path written straight to the clip
would switch masking on halfway through drawing it — and undo would then replay
the drawing click by click instead of undoing "the mask I just drew". One
command when the path closes, and Escape abandons a half-drawn path with
nothing to undo because nothing was written.

**Click for a corner, drag for a curve.** The press places the point and the
button staying down pulls its outgoing handle out; the incoming handle mirrors
it, so the curve runs smoothly through the point. Breaking that symmetry is
what dragging a handle afterwards is for, which the editor already does.

**The gesture that ends the path is visible.** A ring appears on the first
point once there are three, and the chasing segment is dashed so it reads as
not-placed-yet. A pen whose only way out is a keystroke you have to know is a
pen people get stuck in — Escape and Return work too, but neither is the
advertised route.

**Switching clips abandons the drawing.** Clicking a different clip is not a
request to move a half-drawn path onto it.

The old outline is hidden while the pen is out: it is about to be replaced, and
two outlines on the picture would be two things that look equally editable when
one of them is not.

Not done: inserting a point into an existing path, feather handles on the
picture, and mask tracking.

### Phase 6s — mask tracking §7.4 ✅

A mask that follows what it was put on, instead of one somebody has to keyframe
by hand.

**The track is an offset, not the mask's position.** Two curves, `maskX` and
`maskY`, added to wherever the mask was drawn. A path has no centre to animate
— it carries its position in its points — so animating "the mask's position"
would mean animating every point of it, and clearing a track would have nothing
to restore. As an offset, clearing the curves puts the mask back where somebody
drew it with the drawing untouched, and the same two curves mean the same thing
for a rectangle, an ellipse and a path.

**Zero-mean normalised cross-correlation, not sum of differences.** A shot that
brightens between two frames — an exposure step, a lamp, a dissolve starting —
shifts every pixel of the patch by the same amount, and a difference metric
reads that as the patch having gone somewhere. Correlation subtracts the mean
and divides by the spread, so a gain or lift change scores identically to no
change. There is a test that does exactly that: a stop up and a lift, on a patch
that also moved four pixels, and the four pixels are what comes back.

**A box around the mask, not the mask's own outline.** What is being followed is
a piece of texture, and the box is the cheapest honest description of one. A
track that sampled only the pixels inside a bezier would spend its time on the
shape rather than the motion and would still have to fall back to a box the
moment the shape had no detail in it.

**Blur before subsampling.** The patch is sampled on a bounded grid — at most 64
by 64 samples however big the mask is — so one track costs the same on a mask
covering half the frame as on a small one. That sparse sampling is aliasing:
reading every nineteenth pixel of a sharp picture makes the samples land on
different parts of an edge at each candidate offset, and the correlation surface
grows a texture of its own that has nothing to do with where anything went. The
first version of this tracked a title moving six pixels a frame as moving two.
Blurring each frame once by the amount the sampling skips fixes it, and costs
one pass over the frame against the alternative's one pass per candidate.

**Frame to frame, not against the first frame.** A reference frame does not
drift, but it stops matching the moment the subject turns, moves under a
different light, or is partly covered — which is most shots worth tracking.
Frame to frame follows all of that and accumulates a little error instead. Over
eight frames of a known 6,-4 per frame the self-test lands within half a pixel,
and the tolerance in the test says how much drift is still a working tracker.

**Refusals are reported, and what was found is kept.** A flat patch and a patch
that left the frame are both refused by name rather than answered with a
confident number. A track that held for two seconds and then lost its subject
is worth two seconds of keyframes and a note saying where to look, not a
refusal — so the keyframes up to that point are written and the message says
where it stopped.

**Every offset in the search window, no pyramid.** A coarse pass on a
downsampled frame is the usual speed-up and it is also how a tracker learns to
prefer a wrong answer: the coarse level cannot see the detail that
distinguishes two similar places, and the fine level only refines what the
coarse one chose. The patch is already subsampled, so exhaustive is affordable.
The window is capped at 48 pixels, because the cost is its square and a window
wide enough for a whip pan is wide enough to find the wrong lamppost.

**The preview had to be taught too.** The GPU graph read `clip.mask` directly,
so the tracked mask moved in an export and sat still on screen. That is the
worst of both and the self-test caught it: the picture check fails with the GPU
path reverted even though every keyframe is correct.

Not done: rotation and scale (one patch cannot separate them from translation;
several patches solving a similarity together is a different feature),
tracking backwards from the playhead, and a tracker somebody can nudge
mid-track.

### Phase 6t — stabilisation §7.4 ✅

Holding a shaky shot still: the warp stabiliser's job, built on the tracker
Phase 6s already needed.

**A grid of patches, reduced by the median.** One patch follows whatever
happens to be under it, which on a real shot is as likely to be somebody
walking as the background. Nine patches and the median of what they say is the
camera: a subject can dominate a few of them without moving the answer, and a
median needs no threshold to tune, unlike discarding outliers.

**Integrate, smooth, subtract.** The measured motion is integrated into a
camera path, the path is smoothed, and the difference is the correction.
Smoothing the *motion* instead would leave the path free to wander, which is
exactly the low-frequency drift that makes stabilised footage look like it is
floating. The window is half a second by default — shorter leaves the shake in,
longer fights the pan, and a stabiliser that flattens a deliberate move has
taken the shot away from whoever framed it. There is a test for each failure:
shake on a static shot comes out three times smaller, and a steady two-pixel
pan is followed rather than fought.

**The window is clamped at the ends, not shortened.** A window that shrank
towards the ends would smooth the first and last frames less than the middle,
so a clip would start shaky, settle, and end shaky — which looks like the
stabiliser giving up rather than like a shot.

**Three curves, one command.** `stabiliseX`, `stabiliseY` and a held
`stabiliseZoom`, added on top of the clip's own transform rather than written
into its position and scale. A stabiliser that wrote into the position curve
would destroy any move somebody had animated and would have nothing to put back
when cleared. Three separate commands would let undo stop somewhere that holds
the picture still and shows its edges.

**One zoom for the clip, not a curve.** A zoom that changed while the
correction did would be a slow breathing that reads as a focus pull, and is far
more noticeable than a slightly tighter frame.

**Refusing beats answering zero.** Footage with nothing trackable in it — the
flat-field fixtures, a shot of fog — used to come back as "no correction
needed", which is indistinguishable from a shot that really was steady and much
more misleading. It is an error now, by name. A cut mid-clip stops the analysis
and says so, keeping the keyframes found before it; the test for that had to
make the cut a *different picture*, because a very large jump in the same
picture is something a tracker cannot tell from a nearby match on a repeating
pattern.

**The measurement runs on the clip's own frames.** The composite already has
this clip's transform applied to it — including the correction being computed,
which would make the analysis chase its own tail — and whatever is layered over
it, which moved for reasons of its own.

**A fixture that really shakes.** `testdata/generate.sh` grows
`shaky_texture.mov`: a detailed still pattern seen through a jittering crop, so
the shake is known exactly. The first attempt used a moving test pattern, whose
content drifts upwards on its own — the analysis was measuring the fixture's
motion as faithfully as the camera's, and there was no way to tell them apart.
The pattern is drawn from X and Y alone now. End to end, the self-test measures
how far the composited picture moves frame to frame before and after: 8.5
pixels becomes 1.5.

Not done: rolling shutter, rotation and scale (the tracker is translation
only), and a stabiliser that reframes rather than zooms.

### Phase 6u — lens distortion §7.4 ✅

Bending the picture radially, to straighten what a wide lens bulged — or to
bulge it deliberately.

**An effect, not a clip property.** The effect stack already answers the
questions this needs answered: where in the order it applies, whether it is
switched off, and whether its parameters are keyframed. Adding it there was two
table entries, a resample and the switch case that calls it — the panel needed
no new widgets, which is the claim Phase 5 made when it said adding an effect
was data. The self-test drives it through the same generic controls the blur
uses to prove that claim rather than restate it.

**One radial term, not a lens profile.** `r' = r * (1 + curvature * r²)`, with
r in half-diagonals so the corners sit at 1 and a curvature of 0.1 means the
same bend on a 4K frame as on a preview-sized one. Real lenses need more terms
and a decentring pair to match exactly; one term is what straightens a wide
shot, and a profile per lens is a database rather than an effect.

**Resampled from a copy, once.** Every output pixel takes one bilinear sample
of the original. In place, each pixel would be read through the ones already
moved, and the picture would smear along whichever direction the loop happened
to run.

**Outside the source is transparent, not clamped.** Straightening a barrel
makes the corners read from beyond the frame edge, where there is nothing. A
clamped read would streak the border pixel outwards, which looks like a
rendering fault; empty corners are honest, and the zoom control is how somebody
fills them. Zoom is its own parameter rather than derived from the curvature,
because how much of the frame to give up is a decision about the shot.

**A ramp, not a dot, for testing the mapping.** The first test put one lit
pixel in the frame and looked for it afterwards: resampling spreads a single
pixel across its neighbours, so the peak stopped meaning "where the picture
went" and the test failed on arithmetic that was correct. A linear ramp
survives bilinear sampling exactly, so every output pixel says precisely which
source pixel it read, and the formula can be checked to a twentieth of a pixel.

Not done: decentring, chromatic aberration, per-lens profiles, and a GPU path —
an effect already forces the CPU graph, which the render cache makes
affordable.

### Phase 6v — responsive timing §7.4 ✅

The Essential Graphics "responsive design — time" idea: a title's entrance and
exit stay glued to its ends, and the hold in the middle takes up the slack.

**Why it is needed at all.** Keyframes are glued to the picture (ADR-008),
which is right for a fade on a shot and wrong for a title: trimming a title
shorter does not compress its animation, it removes the part that no longer
fits — so the exit simply never happens and the card vanishes at full strength.
The first version of the test asserted that a trim *stretched* the animation,
which is what a retime does; the honest baseline is that the exit is cut off,
and that is now what the test says.

**Durations at each end, plus the length they were authored against.** The
authored duration is stored rather than derived, because it is what the stretch
is relative to. Without it a clip trimmed twice would stretch relative to its
already-stretched self and drift further each time.

**One mapping, in one place.** `animationSecondsAt` is `sourceSecondsAt` with
the intro and outro applied, and every curve on a clip now reads through it —
transform, colour, mask offset, audio gain, effect parameters. The single
exception is the time remap, which defines the mapping the others are read at
and so cannot be read through its own output.

**Too short for both ends squeezes them together.** A title trimmed to less
than its own animation is somebody asking for a faster animation; dropping the
exit to protect the entrance would be a missing animation rather than a quick
one, and which end wins would be a coin toss the user cannot see.

**Whole frames.** A protected stretch ending between two frames would be a
boundary nothing on the timeline could line up with.

Off is exactly off: with both durations at zero the clip behaves as if the
feature did not exist, which the tests check frame by frame rather than assume.

Not done: responsive design in *position* — pinning a graphic to another
layer's bounds — and motion graphics templates, which are the rest of §7.4's
Essential Graphics line.

### Phase 6w — motion graphics templates §7.4 ✅

A title saved on its own, to be dropped into another sequence or another
project: the last piece of the Essential Graphics line except pinning.

**The same encoder the project file uses.** A template is a clip, and the
clip encoder already exists. A second serialiser would be a second thing to
remember whenever a field is added — and the failure would be silent, because a
template written by a forgetful encoder loads perfectly and quietly lacks
whatever was left out. The round-trip test compares `io::fingerprint` of the
original against the loaded clip rather than listing fields, so a field added to
`Clip` is covered the moment it is written.

**Only graphics.** A template referring to media would carry a path that means
nothing in the project it lands in. Refusing to save one beats a template that
arrives empty, and the refusal leaves no file behind.

**What travels, and what does not.** The shape or text, the transform, every
curve, the responsive intro and outro, the effect stack, the mask and the
grade travel. The id and the place it sat do not: a fresh id, because two clips
sharing one are two clips the timeline cannot tell apart, and a fresh range,
because where it used to sit is the one thing about it that is certainly wrong.

**The responsive timing is not rescaled to the new length.** That is the whole
point of it, and it is what makes a template reusable rather than merely
copyable: a lower third designed at two seconds and dropped in at one still
animates on and off at the speed it was designed at. Phase 6v is what makes
this phase worth having.

**A length of its own choosing.** Placed with no duration, a template arrives
at the length it was designed at, because a template dropped in at some
arbitrary length is a template whose timing nobody chose.

While extracting this, the project save's atomic write-and-rename became a
helper both writers share, rather than the second writer getting a plain
truncating write nobody would notice was less careful.

Not done: responsive design in *position* — pinning a graphic to another
layer's bounds — which is the remaining Essential Graphics item, and a browser
for templates rather than a file dialog.

### Phase 6x — pin to clip §7.4 ✅

Responsive design in position, and the last of the Essential Graphics line: a
title pinned to the shot it is over moves and scales with it, so repositioning
the shot does not leave the title behind.

**Composed, not copied.** The host's transform is applied to the clip's own:
the clip's position is scaled *and rotated* by the host's, so a badge in the
corner of a shot stays in the corner when the shot is scaled or turned. The
rotation is what a lazier version would leave out, and it is exactly the case
where the mistake is obvious.

**Opacity is not inherited.** A title over a dissolve is usually meant to
survive it. Inheriting opacity would make the two decisions -- how the shot
fades and whether the title goes with it -- impossible to separate, and the
common case would be the one nobody could express.

**A pin applies only where the host is.** Outside the host's own range there is
no transform to follow, and extrapolating one would put the title somewhere
nothing on the timeline explains. Same for a deleted host: the pin is inert
rather than an error, because the alternative is a project that will not open.

**Cycles refused at the edit, bounded at the render.** A loop would be a
position defined by its own position. The command walks the chain the pin would
create and refuses; the renderer still caps the chain length, because a file
can arrive from somewhere else with a loop already in it.

**The render cache had to be taught.** A pinned clip's picture depends on
another clip's transform, and the frame recipe is built from the clips visible
at that moment -- so a host on a *hidden* track contributed nothing but the
word "hidden", and moving it would have left the pinned title cached where it
was. The recipe mixes the host in explicitly now. The first version of this
check did not test anything: the host was on a visible track, so its
fingerprint was already in the recipe by another route and the check passed
with the feature removed. Hiding the host's track is what makes it bite.

**Where the button pins to.** "Pin to clip below" takes the topmost clip on a
lower track at the playhead -- the one somebody can actually see. Pinning to
something hidden behind another picture would be pinning to a thing that is
not there as far as they are concerned.

Writing the self-test also re-taught an old lesson: adding a track invalidates
the long-held track references earlier blocks still use, so the block sits
after the point where those references stop being live.

§7.4 is complete apart from feather handles on the picture and rotation and
scale in the tracker, both noted where they belong.

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
