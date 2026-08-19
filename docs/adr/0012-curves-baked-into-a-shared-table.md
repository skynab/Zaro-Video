# ADR-012: Tone curves are baked into a table both paths share

**Status:** accepted, implemented
**Date:** 2026-08-19

## Context

A tone curve is drawn against the picture as it is *shown*: its axes are code
values, black at one end and white at the other. The compositor works in
scene-linear light. Something has to bridge the two, on both the CPU and the
GPU, and the two have to agree.

The primary correction ([ADR-011](0011-grading-in-linear-light.md)) solved this
by keeping the arithmetic small enough to state twice — five operations, with
the constants computed once on the CPU. A spline through arbitrary control
points is not that.

## Decision

The curve is evaluated only on the CPU, and only when it changes. The result is
baked into a lookup table of 1024 linear-in, linear-out entries per channel, and
the shader looks the answer up.

The shader has no control points, no spline, and no transfer function. There is
nothing in it that *can* drift from the reference, because it does not implement
the reference — it reads its output. Where ADR-011 needed a test to keep two
implementations honest, here there is one implementation.

## The table is indexed by `sqrt(v / (1 + v))`

The index has to map all of `[0, ∞)` — linear light has no upper bound, and a
highlight may be many times white — onto `[0, 1]`, and it has to be identical on
both sides. This is three arithmetic operations, so there is nothing in it to
get subtly different.

It also spends its resolution where the picture is. A linear index would give
everything below a thousandth of white a single entry, and the shadows are
exactly where a tone curve is read most closely; this index puts middle grey
near entry 400 and a thousandth of white around entry 30.

The alternative — indexing by the encoded value — would have required the
transfer functions in the shader, which is the duplication this decision exists
to avoid.

## Interpolation is a monotonic cubic

Fritsch–Carlson, with the tangents limited so no segment can turn back on
itself. A natural cubic spline through the same control points overshoots near a
steep segment, and an overshooting tone curve is not a subtle error: it puts a
dark halo above a highlight and can invert a gradient.

A test walks a thousand points across a deliberately steep curve and requires
the result to be non-decreasing. It fails with the slope limiting removed.

## Values outside the control points are held

The tangent at the last control point says nothing about what is beyond it, and
following it produces values far outside the range being mapped. The same
reasoning as keyframes ([ADR-008](0008-keyframes-in-source-time.md)).

## The master curve is applied after the per-channel curves

A master curve is a statement about the picture's tones, and the picture is what
the per-channel curves have already made it. The two orders give different
answers, so the order is part of the contract rather than an accident of the
implementation.

## An identity curve is skipped, not sampled

Sampling an identity table would round every ungraded pixel through the table's
own resolution. An ungraded clip has to come out bit-identical — the frame-exact
harness depends on it — so the shader takes a flag and skips the lookup, and the
CPU never builds a table at all.

## Tables are cached against the curves themselves

Building one is three thousand spline evaluations and as many `pow`s: nothing
once, ruinous per frame. The cache compares the curves rather than trusting a
dirty flag somebody has to remember to set, which is the kind of flag that is
correct until the day an undo restores a snapshot behind its back.
