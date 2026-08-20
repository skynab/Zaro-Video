# ADR-013: The render cache invalidates by recipe, not by dependency

**Status:** accepted, implemented
**Date:** 2026-08-20

## Context

Compositing a sequence at an instant is expensive enough that it is worth not
doing twice. It became expensive enough that it is worth not doing *once* in
real time when adjustment layers arrived: a sequence containing one is
composited entirely on the CPU, deliberately, so that the preview and the
export run the same code.

Any cache of rendered frames has to answer one question well: when does a
stored frame stop being the frame? Every edit potentially changes some frames
and leaves others alone, and both kinds of mistake are bad in their own way.
Throwing away too much wastes the work the cache exists to save, and people
stop trusting a bar that goes empty every time they nudge a clip. Throwing away
too little shows a picture from before the change, and the person looking at it
has no reason to suspect the cache rather than their own edit — which is the
worst failure a renderer can have, because it is invisible and it is silent.

The obvious design is to track dependencies: each operation says which frames it
dirtied, and the cache drops those. It is also the design that has to be right
in every operation, including the ones written next year, and wrong in exactly
one of them is enough.

## Decision

A cached frame carries a **recipe**: a hash of everything that decides what the
sequence looks like at that instant. A lookup that finds a frame stored under a
different recipe is a miss, and the stale frame is dropped there and then.

Nothing has to be told that anything changed. There is no dirty-range
bookkeeping, no per-operation invalidation, and no way for a new operation to
forget to participate.

The recipe is built from the fields that are *written to the project file*, by
running the same encoder `io::saveProject` runs. That is the second half of the
decision and it matters as much as the first: the alternative — a hand-written
list of the fields a render depends on — is the same failure mode as dependency
tracking, moved one layer down. A field added to `Clip`, serialized, rendered,
and left out of the fingerprint gives stale frames with no symptom at the site
of the mistake.

Only what is drawn at that instant contributes. A clip elsewhere on the timeline
is not in the recipe, so moving it does not invalidate frames it does not
appear on — which is the difference between caching a timeline and caching a
project.

## Consequences

Invalidation is exact in both directions and costs nothing to maintain. The
tests can state the property directly: change one thing, and require that the
recipe moves if and only if the picture does.

The cost is a hash per frame per lookup: a JSON encode of the handful of clips
visible at that instant, a few microseconds against a composite measured in
milliseconds. It is real, and it is paid on the fast path. It buys the absence
of a class of bug that would otherwise be found by somebody exporting the wrong
picture.

It is also charged to anything that asks *whether* a frame is cached, which is
why the timeline's cache bar is sampled at one point per pixel rather than one
per frame: a bar cannot show more than it has pixels, and hashing a whole
timeline on every repaint to draw the same rectangle is not a trade worth
making.

A frame stored under a recipe nobody asks about again is never noticed as
stale; it ages out under the byte budget like anything else. That is the right
behaviour — the alternative is a sweep over the cache after every edit, which
is the bookkeeping this decision exists to avoid.
