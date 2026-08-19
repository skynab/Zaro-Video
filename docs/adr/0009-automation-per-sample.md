# ADR-009: Audio automation is evaluated per sample

**Status:** accepted, implemented
**Date:** 2026-08-19

## Context

`AudioGraph::mix` renders a block of samples at a time. The block size comes
from the audio device — SDL asks for whatever its buffer holds, and the
playback engine asks for whatever it needs to stay ahead. Clip gain and pan were
constants, so they were computed once per clip per block, which was free.

Keyframed gain is not a constant. The cheap change is to evaluate the curve once
per block and use that value for the whole block.

## Decision

Automated gain and pan are evaluated **per sample**.

A gain held constant across a block steps at the block boundary. Those steps are
discontinuities in the waveform, and a discontinuity is a click — at a 512
sample buffer, a one-second fade becomes a staircase of about ninety of them.

The more serious problem is where the steps come from. The block boundary is a
property of the audio hardware and the current buffer setting, not of the edit.
Evaluating per block means the same project sounds different on different
machines, and different again when the buffer size is changed to fix a dropout.
A mix is not allowed to do that.

The regression test renders the same automated clip in one call and in blocks of
64, 128, 512 and 1000, and requires the results to be **bit-identical**. Not
approximately equal: any difference at all is the block size leaking into the
output. It fails on all four block sizes if evaluation is moved back to
once-per-block.

## Cost

One curve evaluation per sample per clip — a binary search and, for a bezier
segment, a Newton solve with a bisection fallback, both with fixed iteration
counts so the cost cannot spike. It is paid only by clips that are actually
automated; an unautomated clip keeps the constant-gain path, which is unchanged.

The evaluation is done once per sample and shared across channels, into a
scratch buffer reused across clips and blocks, so an automated mix does not
allocate. The curve does not know which speaker it is feeding, so evaluating it
per channel would be the same work twice.

## Alternatives considered

**Control-rate evaluation** — sample the curve every 32 or 64 samples and ramp
linearly between. This is what most audio engines do, and it would be cheaper.
It was rejected for the same reason as per-block: the result depends on a
granularity that has nothing to do with the edit, so it is not reproducible
across a change in that granularity. If profiling ever shows per-sample
evaluation costing real time, control-rate is the fallback — but it should be
adopted deliberately, with a fixed granularity that is a property of the mixer
rather than of the device.
