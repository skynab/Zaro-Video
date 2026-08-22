#pragma once

#include <QColor>
#include <QString>

class QApplication;

namespace zaro::app::theme {

/// Nocturne — the palette the interface is drawn from.
///
/// One place decides what colour anything is. The tokens below are the design
/// system's, transcribed rather than reinvented: a tonal ramp per role, all
/// steps generated on one shared lightness scale, so step 800 of the accent and
/// step 800 of the neutrals sit at the same visual value. Everything the app
/// paints -- the stylesheet Qt applies to its own controls, and the colours the
/// custom-painted panels reach for -- comes from here, so a retune is one edit
/// rather than a hunt through five widgets.
///
/// Steps run 100 (lightest) to 900 (darkest), as the source system numbers
/// them. `accent()` and `neutral()` clamp, so a caller asking for a step that
/// does not exist gets an end of the ramp rather than undefined behaviour.

/// The window ground, and the panel surface a shade above it.
[[nodiscard]] QColor bg();
[[nodiscard]] QColor surface();
/// Darker than the ground: the well a picture sits in, so the picture is the
/// brightest thing on screen even when it is a dark frame.
[[nodiscard]] QColor well();
[[nodiscard]] QColor text();
/// Text at a fraction of its opacity, composited over the ground. Qt has no
/// `color-mix`, so the blend happens here.
[[nodiscard]] QColor textAt(double fraction);
[[nodiscard]] QColor divider();

[[nodiscard]] QColor accent();
[[nodiscard]] QColor accent(int step);
[[nodiscard]] QColor neutral(int step);

/// Mix `over` into `under` by `amount` (0..1). The stylesheet needs literal
/// colours, so every blend the design expresses as `color-mix` is resolved
/// once, here, at the point the sheet is built.
[[nodiscard]] QColor mix(const QColor& under, const QColor& over, double amount);

/// The application-wide stylesheet. Built from the tokens above.
[[nodiscard]] QString styleSheet();

/// Palette, font and stylesheet, applied to the whole application.
void apply(QApplication& application);

}  // namespace zaro::app::theme
