# ADR-002: Effects run as GPU shaders on Qt RHI, not as libavfilter graphs

**Status:** accepted
**Date:** 2026-08-17

## Context

We already depend on FFmpeg for demuxing and decoding, and libavfilter ships a
large, mature, well-tested effect library. Reusing it for the effect pipeline is
the obvious way to get a lot of features cheaply.

## Decision

Use it for nothing in the effect pipeline. Effects are shader nodes authored in
GLSL, compiled by Qt's `qsb`, and executed through `QRhi`.

Two reasons.

**libavfilter is CPU-bound.** A 4K RGBA frame is ~32 MB. Realtime playback with
three or four stacked effects means moving that through system memory and back
to the GPU every frame, several times over. The compositor has to be on the GPU
regardless -- track blending, transforms, transitions -- so a CPU effect chain
means round-tripping between the two for every clip on every frame.

**Its graph model fights per-frame keyframing.** libavfilter graphs are
configured once and then fed frames. An editor changes effect parameters
continuously along a timeline; every keyframed parameter on every clip is a new
value each frame. Reconfiguring or rebuilding graphs at frame rate is not what
that API is shaped for.

Qt RHI additionally gives Metal today and Vulkan/D3D12 later from one shader
source, which matters because the alternative is writing every effect two or
three times.

## Consequences

- Every effect we want, we write. There is no free library. This is the real cost
  and it is accepted deliberately.
- The transform-and-opacity node from Phase 3 is the foundation for all motion
  work, so it is worth over-investing in.
- `swscale`/`swresample` are still used for pixel-format and sample-rate
  conversion at the decode boundary. The decision here is about the *effect*
  pipeline, not about abandoning FFmpeg.
- A CPU fallback path will eventually be needed for headless render nodes without
  a GPU. Deferred until there is a reason.
