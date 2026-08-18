# Zaro Video

A non-linear video editor. C++20, Qt 6, FFmpeg, GPU compositing via Qt RHI.

Early development. See [docs/PLAN.md](docs/PLAN.md) for the architecture and the
phased roadmap, and [docs/adr/](docs/adr/) for the decisions that are already
locked in.

## Status

**Phase 0 — foundations.** The build system, the test harness, and the time
library are in place. Nothing decodes media yet.

| Phase | | |
|---|---|---|
| 0 | Foundations, exact time arithmetic | **done** |
| 1 | Media I/O: demux, decode, hwaccel, thumbnails | next |
| 2 | Project model and edit engine, headless | |
| 3 | GPU compositor and realtime playback | |
| 4 | Application shell, timeline UI, export | |

## Building

Requires CMake 3.24+, Ninja, and a C++20 compiler. Catch2 is found via
`find_package` or fetched automatically.

```
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

Presets: `debug`, `release`, `asan` (ASan + UBSan).

## Layout

```
core/       no GUI dependency, headless-testable
  time/     Rational, RationalTime, TimeRange, Timecode
cmake/      warning policy, find modules
docs/       plan and architecture decision records
```

`core/` deliberately links neither Qt nor FFmpeg. Keeping the edit engine
headless is what makes it testable in CI, scriptable, and renderable without a
window server.
