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

### Phase 2 — Model + edit engine (headless)
*Goal: the whole editing brain, with zero pixels.*

- `Project / Sequence / Track / Clip / MediaRef`, ids stable across save/load.
- Command stack: do/undo/redo, merging, History descriptions.
- Edit operations, each with tests: overwrite, insert (+ripple), razor, lift, extract,
  ripple delete, trim head/tail, ripple trim, roll, slip, slide, three/four-point edit,
  snapping, linked A/V selection, track targeting, sync locks.
- Serialization: versioned JSON, forward-compatible unknown-field preservation.

**Done when:** a headless test constructs a 20-edit sequence, undoes every step back to
empty, redoes to the end, saves, reloads, and asserts byte-identical model state.

### Phase 3 — Compositor + playback
*Goal: the hard part. Frames on screen, audio in sync, scrubbing that feels alive.*

- QRhi device, offscreen render target, texture pool.
- Clip transform node: position / scale / rotation / anchor point / opacity, plus track
  blending top-down. This one node is also the foundation of all motion later.
- `RenderGraph::composite(t)` — resolve active clips per track, pull frames, blend.
- Audio graph: per-clip gain → per-track gain/pan → master, sample-accurate mixing.
- **Playback engine:** audio clock as master, video frame queue with PTS, present on
  vsync, drop-frame policy under load, preroll on seek, JKL shuttle, scrub with
  frame-request coalescing.
- `zaro-render`: headless project → mp4/mov via libavcodec + VideoToolbox, with a real
  A/V muxing path (correct timestamps, no drift over 30 minutes).

**Done when:** a 3-clip sequence plays at 1080p59.94 with locked A/V sync for 10 minutes,
scrubbing stays responsive, and the exported file's audio drift is 0 samples end-to-end.

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

Phases 0 and 1 are done. Phase 2 — the project model and edit engine — starts here, and
it is entirely headless: no pixels, no FFmpeg, just the data model and the operations
that mutate it.

1. `Project / Sequence / Track / Clip / MediaRef` with ids stable across save and load.
   A `Clip` references a `MediaRef` and a source `TimeRange`; it never owns media.
2. The command stack before any edit operation is written. If undo is retrofitted, some
   operation will always have escaped it.
3. Edit operations one at a time, each with tests: overwrite and insert first, since
   ripple behaviour is where the model's invariants get decided.
4. Serialisation with a version field and unknown-field preservation, so a project saved
   by a later build does not lose data when opened by an earlier one.
5. The edit-engine fuzzer from §5 as soon as there are three operations to fuzz — random
   command sequences asserting no overlapping clips, no negative durations, and exact
   restoration on undo.

Carried forward as known work:

- Thumbnail and waveform generation with a content-hashed disk cache (deferred from
  Phase 1 to the head of Phase 4).
- Colour pipeline and working space, as ADR-004. Phase 1 established that every frame
  leaves the decoder fully tagged; what to *convert* those tags into needs the compositor
  to exist first.
- The frame-cache budget and eviction policy — 4K RGBA float is ~32 MB a frame, and this
  constrains the Phase 3 design more than anything else.
- Revisit `DecodeMode::Auto` when the compositor can consume a GPU texture
  ([ADR-003](adr/0003-hardware-decode-readback.md)).
