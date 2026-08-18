# Zaro Video

A non-linear video editor. C++20, Qt 6, FFmpeg, GPU compositing via Qt RHI.

Early development. See [docs/PLAN.md](docs/PLAN.md) for the architecture and the
phased roadmap, and [docs/adr/](docs/adr/) for the decisions that are already
locked in.

## Status

**Phase 3d — GPU compositing.** A project can be edited, saved, composited,
rendered to a file, and played back in sync. On a 1080p59.94 timeline the GPU
path holds the playhead to zero frames of offset with no audio underruns,
presenting about 47 of the 59.94 frames per second against the CPU path's 7 —
the remaining gap is the readback, which a preview window will not do. Still
headless: there is no window yet.

| Phase | | |
|---|---|---|
| 0 | Foundations, exact time arithmetic | **done** |
| 1 | Media I/O: probe, decode, hwaccel, seek | **done** |
| 2 | Project model and edit engine, headless | **done** |
| 3a | Render graph, audio mixer, export | **done** |
| 3b | Playback engine, audio clock, JKL | **done** |
| 3c | GPU compositor on QRhi, golden-tested | **done** |
| 3d | YUV texture path, GPU colour conversion | **done** |
| 4 | Preview window, app shell, timeline UI | next |

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

## Tools

```
zaro-probe <file>                        what we believe about a file
zaro-frame <file> <index> <out.png>      extract one frame, exactly
zaro-frame <file> --benchmark 150        decode throughput
zaro-cut out.zaro a.mov b.mov            build a project from media
zaro-render project.zaro out.mov         render it, headless
zaro-play project.zaro --seconds 10       play it on the GPU, and report sync
```

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
  io/       versioned JSON project files
platform/
  ffmpeg/   the only place libav* headers are included
  qrhi/     GPU compositor and its shaders
  sdl/      audio output device
tools/      zaro-probe, zaro-frame, zaro-cut, zaro-render, zaro-play
testdata/   fixture generator
cmake/      warning policy, find modules
docs/       plan and architecture decision records
```

`core/` deliberately links neither Qt nor FFmpeg. Keeping the edit engine
headless is what makes it testable in CI, scriptable, and renderable without a
window server — and it means a second decoder backend can be dropped in without
touching a caller.
