# CutReel

A non-linear video editor. C++20, Qt 6, FFmpeg, GPU compositing via Qt RHI.

Early development. See [docs/PLAN.md](docs/PLAN.md) for the architecture and the
phased roadmap, and [docs/adr/](docs/adr/) for the decisions that are already
locked in.

## Status

**Phase 7x — the design's third pass.** Media can be imported, marked up in a
source monitor, cut into a timeline three-point with picture and sound linked,
dissolved, marked, graded, masked, mixed, adjusted, watched and exported. The
window is dressed as an editor: a menu bar, a timeline tool palette (select,
blade, trim, slip, hand, zoom), four workspaces that decide which panels are
up, one viewer with source and program pages, a burn-in over the picture with
safe-area guides, and a status line, all drawn from one set of design tokens in
`app/Theme`. The inspector is three tabs — how a clip looks, how it sounds, and
what it is — and every parameter on them is a slider and a value together, so a
scale can be found by sweeping it against the picture rather than guessed at in
decimals. What it shows depends on what the clip *is*: a title has words, a
typeface and a box, a shape has neither a keyer nor a secondary to pull a matte
that was authored rather than shot, an adjustment layer has an opacity and no
position, and a clip that reads a file has a speed and a direction. A multicam
clip's cameras can be listed, synced and cut between; a nested clip opens the
sequence it is made of; and a sound clip carries its own filtering and
compression, so one noisy take is repaired where it lies instead of on the track
that holds the rest of the scene. Select several clips and the panel keeps what
it can write to all of them — the parameters, the blend, the level, the repair —
marks the rows they disagree about, and makes the whole change one undo step.
Pressing a track's header picks the track instead, and the panel says what the
track is — its name, whether it plays, whether it can be edited, and what it
contributes to the mix. Edits align to the edit points around them and say so — a dashed
guide down the tracks, and a blade that draws its cut before it makes it, and
one that cuts picture and sound together where they are linked. Delivery has a
workspace of its own: presets, settings that all reach the encoder, and a queue
that renders them one at a time. On a 1080p59.94 timeline the GPU path holds
the playhead to zero frames of offset with no audio underruns, presenting about
47 of the 59.94 frames per second against the CPU path's 7. A cut can leave
for another program and come back: OpenTimelineIO for anything that reads it,
FCP7 XML — the format Premiere Pro imports and exports — for Premiere, and
FCPXML for Final Cut Pro, which reads neither of the other two.

| Phase | | |
|---|---|---|
| 0 | Foundations, exact time arithmetic | **done** |
| 1 | Media I/O: probe, decode, hwaccel, seek | **done** |
| 2 | Project model and edit engine, headless | **done** |
| 3a | Render graph, audio mixer, export | **done** |
| 3b | Playback engine, audio clock, JKL | **done** |
| 3c | GPU compositor on QRhi, golden-tested | **done** |
| 3d | YUV texture path, GPU colour conversion | **done** |
| 4a | Preview window, transport, scrubbing | **done** |
| 4b | Timeline panel: paint, scrub, drag, razor | **done** |
| 4c | Trim by dragging, waveforms, disk cache | **done** |
| 4d | Effect controls: motion, opacity, blend | **done** |
| 4e | Cross dissolves, in both render paths | **done** |
| 4f | Project bin, import, export dialog | **done** |
| 4g | Source monitor, three-point editing | **done** |
| 4h | Linked A/V, sync locks | **done** |
| 4i | Markers, workspaces | **done** |
| 4j | Multi-selection: shift-click, rubber band, set moves | **done** |
| 5a | Keyframing engine: curves, automation, both render paths | **done** |
| 5b | Stopwatches, keyframe lane, keyframe dragging | **done** |
| 5c | Scopes: waveform, parade, histogram, vectorscope | **done** |
| 5d | Primary correction: balance, exposure, contrast, saturation | **done** |
| 5e | Tone curves: monotonic splines, baked LUT, both paths | **done** |
| 5f | Curve editor: draggable points, per-channel | **done** |
| 5g | HSL secondaries: qualifier, mask view, both paths | **done** |
| 5h | Qualifier UI: hue band, windows, mask toggle | **done** |
| 5i | Look LUTs: .cube reader, baked cube, both paths | **done** |
| 5j | Audio track mixer: strips, solo, metering | **done** |
| 5k | Shape layers: generated rectangles and ellipses | **done** |
| 5l | Text layers: Qt font engine behind a core interface | **done** |
| 5m | Captions: SubRip and WebVTT, burn-in | **done** |
| 5n | OpenTimelineIO interchange, `zaro-otio` | **done** |
| 5o | Masks: shape mattes with feather and invert | **done** |
| 5p | Speed and reverse, with retimed audio | **done** |
| 5q | Track EQ and compression, before the fader | **done** |
| 5r | Loudness to EBU R128, with normalisation | **done** |
| 5s | Proxies: attach, toggle, and never export them | **done** |
| 5t | Nesting: a sequence as a clip, cycles refused | **done** |
| 5u | Multicam: angles, offsets, switching as a cut | **done** |
| 5v | Adjustment layers, grading a stack from above | **done** |
| 5w | Render cache: pre-render a range, play it back | **done** |
| 5x | Sync detection: by timecode and by ear | **done** |
| 5y | Time remapping and freeze frames | **done** |
| 5z | Chroma and luma keying, with spill suppression | **done** |
| 6a | The effect stack: blur and sharpen, in order | **done** |
| 6b | Keyframed effect parameters | **done** |
| 6c | Subclips, match frame, replace footage | **done** |
| 6d | Saving, autosave and recovery | **done** |
| 6e | New and Open: the project shell | **done** |
| 6f | Scene edit detection | **done** |
| 6g | Audio roles and auto-ducking | **done** |
| 6h | Log and HDR footage into the working space | **done** |
| 6i | Delivery: output curve and highlight rolloff | **done** |
| 6j | The preview tone maps too, and colour wheels | **done** |
| 6k | Vignette, and one argument for clip shading | **done** |
| 6l | Baking a look out as a .cube | **done** |
| 6m | Comparison view | **done** |
| 6n | Shot matching | **done** |
| 6o | Wipes and slides, and transitions on generated clips | **done** |
| 6p | Bezier masks: engine, not yet an editor | **done** |
| 6q | The mask editor: convert a shape, drag the points | **done** |
| 6r | The pen tool: draw a mask path from nothing | **done** |
| 6s | Mask tracking: follow what the mask is on | **done** |
| 6t | Stabilisation: hold a shaky shot still | **done** |
| 6u | Lens distortion, as an effect | **done** |
| 6v | Responsive timing: intros and outros that survive a trim | **done** |
| 6w | Motion graphics templates | **done** |
| 6x | Pin to clip: a title that follows its shot | **done** |
| 7a | Relinking media that moved | **done** |
| 7b | Consolidate: gather a project's media into one folder | **done** |
| 7c | Making proxies, not just attaching them | **done** |
| 7d | Metadata and search: find a file by what it is | **done** |
| 7e | Smart rendering: export by copying, where nothing was done | **done** |
| 7f | Project versions: save one, jump between them | **done** |
| 7g | Transcode on ingest | **done** |
| 7h | Shared projects: an advisory lock, and read-only | **done** |
| 7i | Review comments, and a list to send | **done** |
| 7j | The media browser: look through a card, take what you want | **done** |
| 7k | Locking off by default, and two read-only bugs fixed | **done** |
| 7l | Auto-reframe for vertical and square | **done** |
| 7m | Remix: fitting music to a length, on its beats | **done** |
| 7n | Text-based editing: delete words, the cut follows | **done** |
| 7o | Actions and the hotkey manager | **done** |
| 7p | Quiet mode: nothing modal in the way | **done** |
| 7q | The Cutline chrome: menus, tools, workspaces | **done** |
| 7r | The design's second pass: the timeline's controls, and Donate | **done** |
| 7s | Timeline alignment: snap guides, and a blade that shows its cut | **done** |
| 7t | Cutting a linked pair, and a clip that says it is linked | **done** |
| 7u | The Deliver workspace: presets, settings and a render queue | **done** |
| 7v | Premiere interchange: FCP7 XML in and out, `zaro-premiere` | **done** |
| 7w | Final Cut interchange: FCPXML in and out, `zaro-finalcut` | **done** |
| 7x | The design's third pass: the inspector's sliders and tabs | **done** |
| 7y | An inspector per kind of clip: titles, shapes, adjustments, speed | **done** |
| 7z | Angles, nested sequences, and per-clip filtering and compression | **done** |
| 8a | Several clips at once: grouped undo, and only what applies to all | **done** |
| 8b | A track page, and titles named by what they say | **done** |
| 8c | Wider gamuts converted into the working space, on both paths | **done** |
| 8d | Saying what a file's gamut really is, beside its curve | **done** |
| 8e | Saturation against hue, on both paths | **done** |
| 8f | Drawing a hue curve: the editor's fourth channel | **done** |
| 8g | Saturation against brightness, and the curves compounded | **done** |
| 8h | Hue against hue: the curve set complete | **done** |

## Building

Requires CMake 3.24+, Ninja, a C++20 compiler, FFmpeg 5.0+, SDL2, and Qt 6.6+
(for QRhi and the shader compiler). Catch2 and nlohmann/json are found via
`find_package` or fetched automatically.

```
brew install ffmpeg sdl2 qt ninja pkg-config nlohmann-json   # macOS
./testdata/generate.sh                   # media fixtures for the tests
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Presets: `debug`, `release`, `asan` (ASan + UBSan).

Media fixtures are generated rather than committed — see `testdata/generate.sh`.
Without them the media tests skip rather than fail.

`ctest` runs three suites. `zaro_core_tests` and `zaro_media_tests` are headless
and quick. `zaro_app_tests` drives the real window through sixty-two gestures —
a trim with the mouse, a grade through the panel, a render out of Deliver — and
measures the picture each one produced, so it takes about a minute and needs a
display with a working QRhi. `QT_QPA_PLATFORM=offscreen` is not one: that plugin
has no QRhi, and a `QRhiWidget` on it never draws. On a headless Linux box run
it under `xvfb-run`, which is what CI does.

## Tools

```
zaro-probe <file>                        what we believe about a file
zaro-frame <file> <index> <out.png>      extract one frame, exactly
zaro-frame <file> --benchmark 150        decode throughput
zaro-cut out.zaro a.mov b.mov            build a project from media
zaro-render project.zaro out.mov         render it, headless
zaro-play project.zaro --seconds 10       play it on the GPU, and report sync
zaro-otio export project.zaro out.otio   hand the cut to any other NLE
zaro-premiere import cut.xml out.zaro    take one back from Premiere
zaro-finalcut export p.zaro out.fcpxml   hand it to Final Cut Pro
```

`zaro-premiere` speaks FCP7 XML (`xmeml`), which is what Premiere Pro's
File ▸ Import and File ▸ Export ▸ Final Cut Pro XML read and write.
`zaro-finalcut` speaks FCPXML, which is what Final Cut Pro's File ▸ Import ▸ XML
and File ▸ Export XML read and write. Despite the names, these are two unrelated
formats that share a vendor: Final Cut has not read `xmeml` since version 10,
and Premiere has never read `.fcpxml`, so both tools exist. Neither `.prproj`
nor a Final Cut library is an interchange format in either direction — both are
an application's own memory in an undocumented schema that moves with its
version.

Two verification scripts back the claims the tests cannot make on their own:

- `scripts/verify-frame-exact.sh` compares `zaro-frame`'s raw output against
  FFmpeg's own decoder, byte for byte, in each file's native pixel format.
- `scripts/verify-av-sync.sh` renders the flash-and-click fixture, then pulls
  picture and sound back out of the *output file* independently and reports the
  offset between them in samples.

## Layout

```
core/       no GUI dependency, no FFmpeg, headless-testable
  time/     Rational, RationalTime, TimeRange, Timecode
  media/    frame and buffer types, decoder interfaces
  model/    Project, Sequence, Track, Clip, MediaRef
  edit/     commands, undo stack, edit operations, snapping
  render/   colour pipeline, compositor, render graph, mixer, cache
  playback/ scheduler, transport (JKL), audio ring buffer
  io/       versioned JSON project files, OTIO, FCP7 and FCPXML interchange
ui-core/    presentation logic with no toolkit: timeline geometry
app/        the Qt shell: chrome and theme, monitors, timeline, panels
platform/
  ffmpeg/   the only place libav* headers are included
  qrhi/     GPU compositor and its shaders
  sdl/      audio output device
tools/      zaro-probe, zaro-frame, zaro-cut, zaro-render, zaro-play,
            zaro-otio, zaro-premiere, zaro-finalcut
testdata/   fixture generator
cmake/      warning policy, find modules
docs/       plan and architecture decision records
```

`core/` deliberately links neither Qt nor FFmpeg. Keeping the edit engine
headless is what makes it testable in CI, scriptable, and renderable without a
window server — and it means a second decoder backend can be dropped in without
touching a caller.
