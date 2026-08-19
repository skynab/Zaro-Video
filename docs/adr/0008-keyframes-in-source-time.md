# ADR-008: Keyframes are stored in source time

**Status:** accepted, implemented
**Date:** 2026-08-19

## Context

The keyframing engine is the root of §6's dependency graph: motion, effects,
masks and audio automation all hang off it. Before any of that can be built, a
keyframe has to have a time, and there are three candidates for what that time
is measured against:

1. **Sequence time** — where the keyframe sits on the timeline.
2. **Clip-relative time** — an offset from the clip's start.
3. **Source time** — a position in the media the clip references.

The choice is invisible while a clip stays where it was first placed, and
decides the behaviour of every edit afterwards.

## Decision

Keyframes carry a `RationalTime` in the clip's **source** time.

Sequence time fails the first ripple: inserting a clip upstream shifts
everything after it, and animation stored in sequence time stays behind. A fade
set on the last shot would end up over the shot before it.

Clip-relative time survives moves but not trims. Trimming a head moves the
clip's start, so every keyframe measured from it slides against the frames it
was set on — a flash timed to an action would drift off it by exactly the amount
trimmed, which is the kind of error that is only noticed after the edit is
locked.

Source time survives both. A keyframe describes something happening to the
picture, so it is anchored to the picture: the clip can be moved, trimmed,
razored, and have its halves rejoined, and the fade still lands on the frame it
was set on. Splitting a clip is the clearest case — both halves keep the same
source-time curve, and each evaluates the part of it that its own frames cover,
with no keyframes to redistribute.

The cost is that a keyframe's position on screen has to be computed rather than
read, and that the timeline UI must map both ways. That is arithmetic, and it is
the same arithmetic `sourceTimeAt` already does for decoding.

## Evaluation is in seconds, not frames

`Clip::sourceSecondsAt` maps timeline time to source time as a `double`, without
quantising to a source frame. Rounding first would make a 24fps clip on a 60fps
timeline hold each animated value for two or three output frames, turning a
smooth move into a stutter that no keyframe accounts for. Time itself stays
rational ([ADR-001](0001-rational-time.md)); this is a *value* being sampled at
an instant, not an instant being computed, and the distinction is the same one
that makes `Transform` a set of doubles.

A test renders a 24fps clip on a 60fps sequence and requires all sixty output
frames to carry distinct values. It fails, with 24 distinct values, if
evaluation is moved back to quantised source frames.

## Values outside the keyframed range are held, never extrapolated

The slope leaving the last keyframe describes the segment before it, and
continuing it produces opacities of nine and scales of minus two a few seconds
later. Holding is also what makes a single keyframe meaningful: it pins a
constant.

## Consequences

- A curve exists only for a parameter that is animated, and the static value in
  `Transform` stays authoritative until one does. Nothing is paid — in storage,
  in serialization or in evaluation — for the overwhelming majority of clips
  that are not animated.
- Speed changes and time remapping get the behaviour they should have for free:
  `sourceSecondsAt` already accounts for a clip whose source and timeline
  durations differ, so animation retimes with the picture.
- Two keyframes cannot share an instant. `Curve::set` replaces rather than
  appends, because a zero-length segment has no value.
