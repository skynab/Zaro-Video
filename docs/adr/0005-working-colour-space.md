# ADR-005: Compositing happens in scene-linear float, with premultiplied alpha

**Status:** accepted
**Date:** 2026-08-18

## Context

Phase 1 established that every frame leaves the decoder fully tagged: primaries,
transfer, matrix, range. This decides what those tags are converted *into* before
anything is blended, and it has to be decided before the render graph is written
around it rather than after — §4 of the plan lists a colour retrofit as one of
the top risks for exactly this reason.

The choice is essentially between compositing on the numbers as they arrive
(display-referred, non-linear) or converting to linear light first.

Blending on non-linear numbers is wrong in a way that is easy to demonstrate: a
50% cross-dissolve between black and white in gamma space produces a value that
is about 22% of the light of white, not 50%. Dissolves go muddy, motion blur and
defocus darken, and edges of scaled footage pick up dark fringes. Every one of
those is a visible artefact that a colourist will notice and be unable to fix.

## Decision

The working space is:

- **Scene-linear light.** Transfer curves are removed on the way in and
  reapplied on the way out.
- **Rec.709 primaries**, as the default. Wider-gamut sources are converted in;
  this becomes a per-sequence setting when HDR work arrives (§7.3).
- **32-bit float RGBA.** Linear light in 8 bits would band severely in the
  shadows, which is where linear puts most of its precision problems.
- **Premultiplied alpha.** `over` is a multiply-add on premultiplied values and
  needs a divide on straight ones; more importantly, filtering straight alpha
  bleeds colour from fully transparent pixels into the edges of everything
  scaled or rotated, which is where the dark halo around composited graphics
  comes from.

Conversion is symmetric: whatever curve a frame is tagged with is inverted on
input and reapplied on output, so a clip that passes through untouched comes out
bit-identical to what went in. That property is asserted by a test, because it is
the one users notice immediately when it breaks.

## Consequences

- **Memory.** Float RGBA is 32 bytes a pixel: a 1080p frame is 8 MB, a 4K frame
  33 MB. This is the number that drives the frame cache budget, and it is why the
  cache has a hard limit rather than a heuristic.
- **Dissolves will not match legacy tools.** An editor coming from a
  gamma-space application will find a linear cross-dissolve looks different --
  correct, but different. When the working space becomes a per-sequence setting,
  a display-referred option should exist for exactly this reason. Making linear
  the *default* is the decision here, not making it the only choice.
- **HDR is deferred, not designed out.** PQ and HLG are recognised as tags and
  currently rejected with a clear error rather than silently mistreated as
  Rec.709 — which would look catastrophic rather than subtly wrong, and is
  better than pretending.
- BT.709's tagged curve is a camera OETF, while displays follow BT.1886's
  gamma 2.4. This pipeline inverts the tagged curve, which is what the tag
  literally means. Grading work will eventually need the display curve as a
  separate, explicit step; conflating the two now would bake an error into every
  node.
- The CPU implementation here is the reference. When the QRhi shader path lands
  ([ADR-002](0002-effects-on-qrhi.md)), this is the oracle its golden-frame tests
  compare against — a GPU renderer with no independent reference is a renderer
  nobody can prove anything about.
