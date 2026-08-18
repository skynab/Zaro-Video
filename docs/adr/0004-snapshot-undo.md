# ADR-004: Undo restores a snapshot rather than computing an inverse

**Status:** accepted
**Date:** 2026-08-17

## Context

Every edit has to be undoable, and undo has to be exact. The textbook approach
is for each command to compute its own inverse: ripple delete remembers the
clips it removed and the distance it shifted, roll remembers the frames it moved
the cut, and undo replays that in reverse.

The problem is that some of these inverses are genuinely hard. Ripple trim on
one track with sync ripple on three others, where a clip was split by the same
operation, has an inverse that is easy to write and easy to get subtly wrong.
And a wrong inverse does not announce itself. It corrupts the project by a frame
at a time, over an afternoon, until someone notices their cut no longer matches
what they remember making — by which point every autosave is also wrong.

## Decision

`SequenceCommand` snapshots the sequence before the edit and after it. Undo
restores the before state; redo restores the after state.

Correctness stops being something each of the fifteen operations has to earn
individually and becomes a property of the base class. Redo restores rather than
re-executes, so a command whose result depends on state it did not capture
cannot drift between the first application and the second.

Merging falls out of the same structure: a drag that emits three hundred move
commands collapses by keeping the first command's before state and the last
one's after state, which is exactly one undo step covering the whole gesture.

## Consequences

- **Memory.** A `Clip` is about a hundred bytes and holds no media, so a
  thousand-clip sequence snapshots to roughly 100 KB, and both states are kept.
  History depth is bounded at 200 entries and `CommandStack::snapshotBytes()`
  reports the real figure, with a test asserting it stays in kilobytes for a
  timeline of ordinary size.
- **This does not scale to every conceivable project.** A single sequence with
  tens of thousands of clips would make each entry megabytes. The answer then is
  to give a real inverse to the few operations that touch many clips, and leave
  the ninety percent that touch two alone — not to abandon the approach.
- Commands must not capture references to anything outside the sequence they
  snapshot, or undo will restore the sequence and leave that other thing
  changed. Operations that need to allocate ids do so at build time, before the
  snapshot is taken.
- The obvious test — "undo returns the project to its prior state" — is close to
  tautological under this design, so it is not where the value lies. The fuzzer
  therefore concentrates on the *forward* direction: after every accepted
  operation it checks that no clips overlap, no duration is negative, no clip
  reads past the end of its source, and no id is duplicated. That is where the
  bugs actually were, and it found two of them on its first run.
