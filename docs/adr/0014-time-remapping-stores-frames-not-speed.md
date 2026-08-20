# ADR-014: A time remap stores which frame, not how fast

**Status:** accepted, implemented
**Date:** 2026-08-20

## Context

Constant speed is already a property of a clip's two ranges: source duration
over timeline duration, with a `reversed` flag for direction. A *varying* speed
is not expressible that way, and it is the thing people actually want — a shot
that runs at full speed, ramps down over a second, holds, and ramps back.

There are two ways to describe that. Keyframe the speed, which is how the
control usually presents itself; or keyframe the source frame, and let speed be
the slope.

## Decision

The curve stores **which frame of the media is shown**, in seconds of source
time, keyed against the clip's un-remapped mapping. Speed is its derivative and
is not stored at all.

Keyframing the speed would mean integrating it to find a frame. An integral
accumulates its own error along the clip, so a hold that was set to end on
frame 300 ends on 299 or 302 depending how far into the timeline it sits, and
"the same ramp" behaves differently on a clip somebody trimmed. Storing the
frame makes every keyframe exact by construction: what the curve says at a
keyframe is the frame that is shown there, with no arithmetic between the two.

It also collapses three features into one. A freeze is a curve that does not
change. Reverse is a curve that falls. A ramp is a bezier segment. All of them
are edited in the keyframe lane that already exists, by the same operations,
with the same undo.

**The remap curve is keyed in un-remapped source time; every other curve is
read there too.** The first half is forced — reading the remap through the
remap it defines is circular. The second half is a choice: a fade drawn across
a frozen shot still fades, because stopping the picture is not a statement
about the graphics on top of it. Somebody dragging an opacity ramp over a
freeze means the ramp to run.

**Time remapping is a picture operation.** The sound of a remapped clip runs at
the clip's own speed, and reads through `Clip::baseSourceTimeAt`. Retiming a
signal is resampling it, and a remap changes rate continuously — a varispeed
resampler is a real piece of work, worth doing on its own rather than badly in
passing. Premiere draws the same line, and the honest version of that line is
one function name that says which mapping it is.

**Time remapping has no static value, so it has a switch rather than a
stopwatch.** Every other animatable parameter has a value the clip holds when
nothing is animated. A remap that is not animated is the clip's ordinary
mapping — there is nothing for a stopwatch to turn off *to*, and inventing one
would put "frozen on the first frame" a single click away from every clip in
the project.

## Consequences

Both render paths get it for free: they already ask the clip which source time
to read, and that is the one function that changed. So do the render cache
(the curve is serialized, so it is in the frame recipe) and export.

The frame is chosen to the nearest, not truncated — a continuous curve against
discrete media otherwise shows every frame slightly late and holds the last
frame of a ramp for two.

A curve dragged below zero clamps to the first frame rather than asking for
frames that do not exist. A clip that silently stopped drawing would be the
alternative, and it would look like a broken decode.

There is no frame blending or optical flow: a slowed clip repeats frames. That
is a rendering feature on top of this one, and this one has to be right first.
