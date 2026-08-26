#pragma once

#include <QIcon>
#include <QPixmap>

class QColor;

namespace zaro::app::icons {

/// The interface's icons, drawn rather than fetched.
///
/// The design calls for a Phosphor set. Nothing is vendored here and a glyph
/// taken from whatever font happens to be installed renders as a colour emoji
/// on one platform and an empty box on another — so these are painter paths at
/// Phosphor's regular weight, which is the one dependency-free way to have the
/// icon the design asks for and have it look the same everywhere.
enum class Glyph {
    Cursor,      ///< Select
    Scissors,    ///< Blade
    TrimEdges,   ///< Trim: a span between two hard edges
    SlipArrows,  ///< Slip: the same span with nothing pinning it
    Hand,        ///< Hand
    Magnifier,   ///< Zoom
    Magnet,      ///< Snapping
    Bookmark,    ///< Add marker
    Split,       ///< Razor at the playhead
    CrossFade,   ///< Add dissolve
    Link,        ///< Picture and sound edited as one
    Plus,        ///< Zoom in
    Minus,       ///< Zoom out
    Heart,       ///< Support: the one filled glyph, as the design draws it
    FilmStrip,   ///< A picture file, in the bin
    Waveform,    ///< A sound file, in the bin
    Image,       ///< A still, in the bin
    CaretDown,   ///< An open group heading
    CaretRight,  ///< A closed group heading
    DotsThree,   ///< The overflow menu: the second filled glyph
    Rows,        ///< The list/thumbnail view toggle
    Circle,      ///< A monitor toggle that is off
    CheckCircle, ///< A monitor toggle that is on
};

/// One glyph, in one colour, at a size in logical pixels.
[[nodiscard]] QPixmap pixmap(Glyph glyph, int size, const QColor& ink);

/// A toolbar icon: muted while the button is off, in the accent while it is on,
/// which is how a checked tool says so.
[[nodiscard]] QIcon toolIcon(Glyph glyph, int size = 17);

}  // namespace zaro::app::icons
