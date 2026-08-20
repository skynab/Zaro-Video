#pragma once

#include <cstdint>
#include <string>

namespace zaro::model {

/// A clip that generates its picture instead of reading one.
///
/// Carried on `Clip` rather than modelled as a separate kind of thing on the
/// track. A graphic is trimmed, moved, faded, graded, keyframed and linked
/// exactly like any other clip, and every one of those operations already
/// works; making it a second type would mean either duplicating all of them or
/// discovering, one at a time, which ones it had been left out of.
enum class GraphicKind : std::uint8_t {
    /// Not a graphic. The clip reads its media, as usual.
    None,
    Rectangle,
    Ellipse,
    Text,
};

[[nodiscard]] const char* toString(GraphicKind kind) noexcept;
[[nodiscard]] GraphicKind graphicKindFromString(const char* name) noexcept;

/// A generated shape.
///
/// Sized and positioned in output pixels from the centre of the frame, the same
/// coordinates `Transform` uses — so a shape and a clip mean the same thing by
/// "40 pixels right", and the motion controls work on a shape without anything
/// being special-cased.
struct Graphic {
    GraphicKind kind{GraphicKind::None};

    /// What a text layer says, and how.
    ///
    /// The family is a name, resolved when the text is drawn rather than
    /// stored as a resolved font: a project opened on a machine without that
    /// typeface should fall back to something readable, and it should fall back
    /// again — to the right thing — when it goes back to a machine that has it.
    std::string text;
    std::string family;
    double pointSize{72.0};
    bool bold{false};
    bool italic{false};
    /// -1 left, 0 centre, 1 right, about the shape's own box.
    int alignment{0};

    double width{400.0};
    double height{200.0};
    double centreX{0.0};
    double centreY{0.0};

    /// Corner radius in pixels, for a rectangle. Ignored by an ellipse, which
    /// is already all corner.
    double cornerRadius{0.0};

    /// How far the edge fades, in pixels. Zero is a hard edge — which is
    /// aliased, so the rasteriser antialiases it regardless; feather is
    /// something wider and deliberate.
    double feather{0.0};

    /// Scene-linear, premultiplied on the way out. Alpha is coverage: a shape
    /// at alpha 0.5 is a half-transparent shape rather than a darker one.
    double red{1.0};
    double green{1.0};
    double blue{1.0};
    double alpha{1.0};

    [[nodiscard]] bool isSet() const noexcept { return kind != GraphicKind::None; }

    friend bool operator==(const Graphic&, const Graphic&) = default;
};

}  // namespace zaro::model
