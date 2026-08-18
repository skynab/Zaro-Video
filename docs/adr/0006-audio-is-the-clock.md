# ADR-006: Audio is the clock, and audio is never dropped

**Status:** accepted, implemented
**Date:** 2026-08-18

## Context

Playback needs a definition of "now". Three candidates: a wall clock, the
display's vsync, or the audio device's consumption of samples.

A wall clock drifts against the audio hardware. A sound card running 0.01% fast
is entirely normal and puts picture a frame out every three minutes — slowly,
invisibly, and in a way that looks like the file is wrong rather than the player.

Vsync is worse: it is quantised to the display's refresh, which has no
relationship to either the timeline's frame rate or the audio hardware, and it
stops entirely when the window is hidden.

## Decision

**The master clock is the count of samples the audio device has consumed.** The
ring buffer's delivered-frame counter is the only source of "now", and the
timeline position is an exact rational function of it. Video is presented
against that clock; it never drives it.

Two consequences follow, and both are policy rather than accident:

**Video is dropped; audio never is.** A dropped frame is invisible in a way a
gap in sound is not — the audience hears the audio. More fundamentally, an
audio gap is a *lie about how much time has passed*, and everything else is
timed against it.

**A frame already behind the playhead is not rendered at all.** A renderer that
falls behind and then works through its backlog in order never catches up; it
stays exactly as far behind as the moment it stumbled. The render head skips
forward past the backlog instead, which is what makes playback recover rather
than degrade.

## Consequences

- **Audio production must not share a thread with rendering.** This was measured,
  not assumed: with both on one thread, a 1080p59.94 timeline produced 203,776
  samples of underrun — four seconds of silence — purely because compositing was
  in the way. On its own thread, the same timeline produces zero underruns while
  still dropping most of its frames. That is the policy working.
- The device callback must not allocate, lock or block, so the ring is lock free
  and its only failure mode is reading silence.
- **The clock and the ring's read position are separate counters.** Advancing the
  read position for silence invented during an underrun runs it past the write
  position and corrupts the buffer. The clock counts everything handed to the
  device, including that silence, because the time really did pass — a clock
  that stalls exactly when playback is in trouble is worse than useless.
- Shuttle speeds other than 1x currently play silent. Playing audio at 4x needs
  pitch handling to sound like anything; the clock still advances because the
  device is still being fed.
- Scrubbing with audio, and looping, will both need the clock to be re-anchored
  rather than reset — `PlaybackScheduler::seek` already does this.

## Verification

The scheduler is sans-io: it takes a clock reading as an argument rather than
reading one. A ten-minute playback at 59.94, including a ten-second render
stall, is therefore simulated deterministically in milliseconds — including the
conditions that matter, which are the ones a real-time test cannot arrange on
demand. Sync holds to within one frame whenever the renderer keeps up, and a
stall is recovered from in a single tick.
