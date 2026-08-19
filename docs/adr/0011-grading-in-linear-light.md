# ADR-011: Primary correction is applied in scene-linear light

**Status:** accepted, implemented
**Date:** 2026-08-19

## Context

[ADR-005](0005-working-colour-space.md) put the working space in scene-linear
Rec.709. Primary correction is the first thing built on top of it, and it could
equally be applied to the encoded signal: most consumer tools do, because most
consumer tools never have linear values to hand.

## Decision

The grade is applied in the linear working space, before the clip is placed in
the frame.

This is the payoff of the working space rather than an extra cost of it:

- **Exposure is a multiply.** One stop is exactly a factor of two, and it is the
  same factor at every brightness. On an encoded signal it would be a curve
  whose meaning changes with the code value, and "+1 stop" would be an
  approximation rather than a statement.
- **White balance is a set of channel gains**, which is what a different
  illuminant actually does to a sensor, rather than a curve-shaped imitation of
  one.
- **Nothing has to be decoded first.** A correction applied to encoded values
  has to undo the transfer function, correct, and reapply it; every one of those
  steps is a place for the CPU and the GPU to disagree.

## Contrast pivots at 0.18, not 0.5

Half is the middle of an *encoded* signal. In linear light it is nearly two
stops above middle grey, and pivoting there would make every contrast adjustment
darken the picture as a side effect — a control that also does something else is
a control that has to be undone with a second one.

The same reasoning gives white balance its normalisation: the gains are divided
by their own luma so that white keeps its brightness, and the temperature slider
is not also an exposure slider.

The two ends of the contrast control are inverses of each other — `+100` doubles
the exponent, `-100` halves it — so it can be returned to neutral by eye. A test
takes a value through both and requires it back where it started.

## Non-positive values are left alone

A fractional power of a negative number is not a number, and one NaN spreads
through every pixel it is later averaged with. Scene-linear values can go
negative through a wide-gamut conversion, so this is not hypothetical. Both the
CPU and the shader leave anything at or below zero where it is.

## Order of operations: balance, exposure, contrast, saturation

The photographic one. Light is balanced and exposed before a tone curve is
applied to it, and saturation acts last, on the tones being delivered rather
than on the ones on the way in. The order is part of the contract between the
CPU and the shader, not an implementation detail of either.

## The constants are computed once, on the CPU

`gradeConstantsFor` turns the panel's units — stops, −100..100, saturation where
100 is neutral — into gains and exponents, and the shader receives those. The
shader never re-derives what a temperature of −20 means. Two implementations of
that question are two answers, and preview would disagree with export by an
amount too small to notice and too large to accept.

## Grading is un-premultiplied

Alpha is divided out and multiplied back, on both paths. Grading premultiplied
values would make a correction depend on how transparent the pixel is, so a clip
would grade differently in the middle of a dissolve than on either side of it.

## What holds the two implementations together

A test grades a 32×32 spread of colours — including values above 1, since a
scene-linear highlight is allowed to exceed white — through ten corrections on
both paths and requires them to agree to within the transfer table's
interpolation error. It was verified to fail, by more than two full code values,
when the shader's pivot is changed to 0.5 while the CPU keeps 0.18.
