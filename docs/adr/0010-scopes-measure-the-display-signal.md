# ADR-010: Scopes measure the display signal, not the working space

**Status:** accepted, implemented
**Date:** 2026-08-19

## Context

The compositor works in scene-linear light ([ADR-005](0005-working-colour-space.md)).
Scopes measure a frame. The obvious implementation measures the frame in the
form it already has, which is linear.

## Decision

Every measurement encodes through the transfer function first.

A waveform of linear values is not the instrument anyone is asking for. Middle
grey sits at 18 rather than near 46, and every reference a colourist works
against — legal range at 16 and 235, skin tones around 60–70, the vectorscope's
colour targets — is defined on the encoded signal. A scope that reads in linear
light is not a stricter instrument; it is a different one, calibrated against
nothing anybody has.

The honest consequence is that a scope reading depends on which transfer curve
the frame is being *shown* through. That is correct: the instrument measures
what is going out, not what is being kept in memory. `ScopeOptions::transfer`
makes it explicit rather than implied.

## Pixels are un-premultiplied before measuring

The working space is premultiplied, so a half-transparent white pixel is stored
as 0.5. Measured directly it reads as mid grey, and a dissolve would show on the
scope as a change in exposure — sending a colourist after a problem that is
really an opacity.

## Measurement is on demand, not per frame

Measuring means compositing a frame on the CPU. Doing that on every frame of
playback would cost more than the picture does, and would reintroduce through
the back door exactly the per-frame readback the GPU path exists to avoid
([ADR-007](0007-gpu-compositor-on-qrhi.md)). So the frame is measured when the
playhead settles, when an edit changes the picture under it, and when the panel
becomes visible — never during playback, and never while the panel is hidden.

It composites through `render::RenderGraph` rather than reading back the
preview. The scope then reports what will be *delivered*, which is the number a
grade is judged against, and playback does not pay for a readback it otherwise
does not need.

## The measurement is counts, not a picture

`measure` returns histograms and cell counts. Drawing is a UI decision — trace
colour, scaling, graticule — and a panel can be resized, restyled or switched
between instruments without the frame being composited again. All four
instruments come from one pass, because the expensive part is the transfer
encode and four separate functions would pay it four times.

## What this cost to get right

The first version of the panel drew nothing at all for a fully lit frame. Level
255 mapped to `bottom - height`, one pixel above the plot area, where it was
clipped: the span between the first and last row is one less than the number of
rows. A black frame looked perfect throughout, because level 0 lands on the
bottom row either way.

The test that found it asserts *where* the trace sits rather than how much of it
there is — a bright frame reads at the top of the scope, a black frame at the
bottom — because the measurement is in signal order and the screen is upside
down relative to it. Getting that backwards produces an instrument that looks
entirely plausible and reports the opposite of the truth, and it is verified to
fail when the drawing is flipped.
