# ADR-001: Time is rational, never floating point

**Status:** accepted, implemented
**Date:** 2026-08-17

## Context

Every temporal quantity in an editor -- a clip's in point, a sequence duration, a
keyframe position, an audio sample offset -- needs a representation. The obvious
choice is `double` seconds, and it is wrong.

Broadcast frame rates are exact fractions with a denominator of 1001: 29.97 is
30000/1001, 23.976 is 24000/1001. Neither is representable in binary floating
point. The error per frame is tiny, but editors accumulate: a timeline is a sum
of thousands of durations, playback advances a frame at a time for hours, and
audio is compared against picture at 48000 comparisons a second. The failure mode
is not a crash. It is a clip that will not snap flush against its neighbour, an
export one frame short, and audio that drifts far enough into a long programme to
be audible but not far enough to be obviously a bug.

## Decision

Three types, all exact integer arithmetic:

- `Rational` -- an exact fraction in lowest terms, `int64` numerator and
  denominator. Arithmetic reduces cross terms before multiplying so intermediates
  stay small; residual overflow trips `ZARO_CHECK` in every build, release
  included, because a truncated timestamp is worse than a stop.
- `RationalTime` -- a whole frame count plus the exact rate it is counted at.
  Timeline positions are discrete by nature, so the frame index is the value and
  the rate travels with it. A source frame at 23.976 can never be silently
  mistaken for a timeline frame at 59.94.
- `TimeRange` -- half-open `[start, start + duration)`. This is what makes
  butt-jointed clips tile a timeline exactly: a clip ending at frame 100 and one
  starting at frame 100 are adjacent, every frame belongs to exactly one of them,
  and there is no off-by-one at the boundary.

Rate conversion is explicit (`rescaledTo`) and rounds to nearest, so the lossy
step is always visible at the call site rather than implied by an assignment.

## Consequences

- Arithmetic is slower than `double`. Irrelevant: this is not in any inner loop
  that runs per pixel, and the frame-rate path is integer add on the fast path.
- Mixed-rate arithmetic needs a defined result rate. It resolves to the finer of
  the two operands so resolution is never lost silently.
- `__int128` is required for the widened intermediates. Available on Clang and
  GCC; a Windows/MSVC port needs a checked-64-bit fallback in `makeChecked`,
  guarded by `#error` today so the gap cannot be missed.

## Verification

The implementation is cross-checked against FFmpeg's `av_timecode_make_string`,
which is an independent implementation of the same SMPTE rules. All 2,589,408
drop-frame labels in a 24-hour day at 29.97 match exactly, as do 59.94 drop
frame, 29.97 non-drop, 25 and 23.976. The suite additionally asserts that labels
are unique and monotonic, and that an hour of drop-frame timecode is within one
second of a real hour while an hour of non-drop is more than three seconds off --
the property drop frame exists to provide.
