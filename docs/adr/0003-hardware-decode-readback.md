# ADR-003: Hardware decode is wired up but not the default, yet

**Status:** accepted, revisit in Phase 3
**Date:** 2026-08-17

## Context

VideoToolbox decode was built in Phase 1 on the assumption that hardware decode
is faster than software decode. Measured on Apple silicon, decoding through our
own pipeline, it is not:

| Clip | Software | VideoToolbox |
|---|---|---|
| 3840x2160 ProRes 422, 29.97 | 174.7 fps — **5.83x realtime** | 30.5 fps — 1.02x realtime |
| 1920x1080 H.264, 29.97 | 1474.4 fps — **49.2x realtime** | 171.6 fps — 5.73x realtime |

The decode itself is not the problem. Hardware decode delivers frames as GPU
surfaces, and Phase 1 has no GPU-side consumer, so every frame is copied back to
system memory through `av_hwframe_transfer_data`. At 4K that is roughly 17 MB per
frame of synchronous readback, and it costs far more than it saves. Meanwhile
FFmpeg's software ProRes decoder is frame- and slice-threaded and uses every core
on the machine.

## Decision

`DecodeMode::Auto` resolves to software decoding. The hardware path stays fully
implemented, tested, and reachable through `DecodeMode::ForceHardware`.

Keeping it built rather than deleting it is deliberate: Phase 3 needs a working
hardware path, and code that has been untested for a phase is not a working path.
A test asserts hardware and software produce the same picture for the same frame
index, so the path cannot rot silently.

## Consequences

- Phase 1 is faster than it would otherwise have been, on the strength of a
  measurement rather than an assumption.
- Battery life is worse than a hardware path would give. Irrelevant for now;
  relevant for a shipping editor, and another reason to expect this to flip.
- **This decision expires when the compositor can consume a GPU texture
  directly.** At that point the readback disappears, hardware decode becomes a
  zero-copy handoff into the render graph, and `Auto` should be measured again
  and very probably changed. The test
  `"Auto mode picks software while frames are consumed on the CPU"` exists to
  make that revisit deliberate rather than accidental.
- On platforms without a competitive software decoder, or for codecs where
  software decode is genuinely slow (8K, or AV1 on weak cores), `Auto` will need
  to become a real decision rather than a constant.

## Note on the readback itself

A frame that is decoded on the GPU, copied to the CPU, and then uploaded back to
the GPU for compositing crosses the bus twice for no benefit. `VideoFrame` is
documented as CPU-owning for exactly this phase, and the eventual GPU-resident
variant is what makes the hardware path worth having.
