# ADR-007: The GPU compositor, and why it is not yet faster

**Status:** accepted, implemented
**Date:** 2026-08-18

## Context

[ADR-002](0002-effects-on-qrhi.md) committed to effects as GPU shader nodes on
QRhi. Phase 3b measured the CPU pipeline at roughly 9 fps on a 1080p timeline
while the decoder managed 1840, so the pixel pipeline was clearly the thing
standing between the playback engine and realtime HD.

## Decision

The compositor is implemented on QRhi, with the CPU version kept as the
reference the golden-frame tests compare against.

**QRhi is private API in Qt 6.11.** It lives under `QtGui/6.11.1/QtGui/rhi/` and
requires linking `Qt6::GuiPrivate`. This is a real cost — it carries no
source-compatibility promise across Qt minor versions, so a Qt upgrade can
require changes here. It is accepted because Qt Quick's own renderer is built on
the same interface, which makes it the best-supported private API in Qt, and
because the alternatives are worse: writing Metal, Vulkan and D3D backends by
hand, or using OpenGL, which is deprecated on the platform we are developing on.
The blast radius is one file.

Shaders are written once in GLSL and compiled by `qsb` into bundles for Metal,
Vulkan and D3D, so the effect library does not have to be written three times.

## The measurement, and what it says

At 1080p, per frame:

| Stage | Throughput |
|---|---|
| Decode (software) | 1840 fps |
| YUV → linear working space | 103 fps |
| Compositing, CPU | 63 fps |
| Compositing, GPU (upload + draw + readback) | **54 fps** |
| Whole pipeline, measured | ~9 fps |

**The GPU compositor is slower than the CPU one.** Not because the GPU is slow —
because each frame is uploaded as 8 MB of float RGBA and read back as another
8 MB, and that round trip costs more than the compositing it replaces. This is
the same shape as the hardware-decode finding in
[ADR-003](0003-hardware-decode-readback.md), and it has the same cause: moving
a frame across the bus to do one operation on it.

The individual stages also do not add up to the whole. 1840, 103 and 63 in
series predict about 38 fps, and the pipeline measures 9. The gap is the rest of
the per-frame traffic — clearing, cloning into the queue, caching — each of which
reads or writes another 8 MB. **The pipeline is memory-bandwidth bound on
float RGBA frames**, not compute bound, and no amount of optimising individual
loops changes that.

## The YUV path, and what it changed

Acting on the above: the decoder's planes are uploaded as they are, and the
colour conversion happens on the GPU. Measured at 1080p, converting and
compositing together:

| Path | Throughput |
|---|---|
| CPU: convert to working space, then composite | ~40 fps |
| GPU: planes up, result stays on the GPU | **520–590 fps** |
| GPU: planes up, result read back to the CPU | 93 fps |

In real playback of a 1080p59.94 timeline, that is the difference between 80
frames presented with picture lagging the clock by three, and 469 presented with
an offset of zero.

**The conversion needs its own pass, and this was not obvious.** The first
version folded it into the sampler — one pass, convert per fragment. It matched
the reference exactly at 1:1 and diverged as soon as a clip was scaled, by more
than chroma subsampling could explain. The cause is that a bilinear filter
applied to encoded Y'CbCr interpolates in gamma space: the midpoint of two
encoded values is not the encoding of the midpoint. That is precisely the error
[ADR-005](0005-working-colour-space.md) rejects for blending, reappearing in the
sampler. Converting into a linear-light surface first, and transforming from
that, costs one extra GPU pass and no bus traffic — and makes the two paths
agree.

Chroma is sampled with nearest filtering rather than linear, to match what the
CPU reference does. Proper chroma siting is a real improvement and should change
both paths together rather than letting them drift apart.

## Consequences

- The GPU path is the default in `zaro-play`, with `--cpu` to force the
  reference. The CPU path stays: it is the oracle the GPU is checked against,
  and it is what a headless render node without a GPU will use.
- **Preview still reads back**, because the playback scheduler's queue holds CPU
  images. Removing that needs a GPU-resident frame in the queue, which is what a
  preview window wants anyway. The 550-versus-95 fps figures are the size of
  that remaining prize.
- **Now** the `DecodeMode::Auto` revisit in ADR-003 makes sense: hardware decode
  becomes worthwhile when its output can stay on the GPU, and the compositor now
  accepts planes rather than converted pixels.

## Verification

Golden-frame tests render the same transform through both paths and compare.
They do not assert pixel equality, because that would be false: the CPU maps
destination pixels back into the source and treats outside as transparent, while
the rasteriser decides coverage at the quad's edge. The two disagree along a
one-pixel outline by under half a pixel of geometry. So the tests assert that
the *interior* matches to well under an 8-bit quantisation step, and that the
pixels which differ form a thin outline rather than an area — which is checkable
by counting them, and is what makes a genuine misalignment impossible to hide
behind a loose tolerance.
