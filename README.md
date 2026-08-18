# Zaro Video

A non-linear video editor. C++20, Qt 6, FFmpeg, GPU compositing via Qt RHI.

Early development. See [docs/PLAN.md](docs/PLAN.md) for the architecture and the
phased roadmap, and [docs/adr/](docs/adr/) for the decisions that are already
locked in.

## Status

**Phase 2 — model and edit engine.** Media decodes frame-exactly, and a project
of sequences, tracks and clips can be edited, undone, saved and reloaded. All of
it is headless: there is no compositor and no window yet.

| Phase | | |
|---|---|---|
| 0 | Foundations, exact time arithmetic | **done** |
| 1 | Media I/O: probe, decode, hwaccel, seek | **done** |
| 2 | Project model and edit engine, headless | **done** |
| 3 | GPU compositor and realtime playback | next |
| 4 | Application shell, timeline UI, export | |

## Building

Requires CMake 3.24+, Ninja, a C++20 compiler, and FFmpeg 5.0+ development
libraries. Catch2 and nlohmann/json are found via `find_package` or fetched
automatically.

```
brew install ffmpeg ninja pkg-config nlohmann-json   # macOS
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
```

`scripts/verify-frame-exact.sh` compares `zaro-frame`'s raw output against
FFmpeg's own decoder, byte for byte, in each file's native pixel format.

## Layout

```
core/       no GUI dependency, no FFmpeg, headless-testable
  time/     Rational, RationalTime, TimeRange, Timecode
  media/    frame and buffer types, decoder interfaces
  model/    Project, Sequence, Track, Clip, MediaRef
  edit/     commands, undo stack, edit operations, snapping
  io/       versioned JSON project files
platform/
  ffmpeg/   the only place libav* headers are included
tools/      zaro-probe, zaro-frame
testdata/   fixture generator
cmake/      warning policy, find modules
docs/       plan and architecture decision records
```

`core/` deliberately links neither Qt nor FFmpeg. Keeping the edit engine
headless is what makes it testable in CI, scriptable, and renderable without a
window server — and it means a second decoder backend can be dropped in without
touching a caller.
