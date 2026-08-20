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
