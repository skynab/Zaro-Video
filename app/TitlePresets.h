// The titles the pane offers, and what each one looks like.
//
// One catalogue, because two things make titles and they must make the same
// ones: the Add Title action, and a preset dragged out of the Titles tab. A
// second table would be two answers to "what is a lower third", and they would
// drift the first time either was touched.
//
// Nothing here is a file. A preset is a shape a graphic starts in; the moment
// it lands it is an ordinary clip, and everything that trims, moves, grades,
// keyframes or links a clip works on it -- which is the argument `Graphic.h`
// makes for graphics riding on `Clip` in the first place.
#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "zaro/core/model/Graphic.h"
#include "zaro/core/time/RationalTime.h"

namespace zaro::app {

/// One entry in the Titles tab.
struct TitlePreset {
    /// Stable, and what a drag carries. Never shown.
    std::string id;
    /// What the pane calls it, and what the clip is called until somebody
    /// types over the text.
    std::string name;
    /// The second line in the pane: where it sits, in a few words.
    std::string blurb;
};

/// Every preset, in the order the pane lists them.
[[nodiscard]] const std::vector<TitlePreset>& titlePresets();

/// The preset with this id, or null. An unknown id is a drag from a build that
/// knew about a preset this one does not, which is a refusal rather than a
/// guess.
[[nodiscard]] const TitlePreset* findTitlePreset(std::string_view id);

/// What the preset looks like in a frame of this size.
///
/// Everything is in output pixels from the centre of the frame -- the same
/// coordinates `Transform` and the shapes use -- and everything scales with the
/// frame, because the rasteriser reads `pointSize` as pixels. A default in
/// points would be illegible on a 4K cut and enormous on a thumbnail.
[[nodiscard]] model::Graphic graphicFor(const TitlePreset& preset, std::int32_t frameWidth,
                                        std::int32_t frameHeight);

/// How long one runs when nobody has said otherwise.
///
/// Long enough to read and short enough to trim. A title is the kind of clip
/// whose length is decided by the cut around it, so the value matters less than
/// it being the same wherever a title comes from.
[[nodiscard]] time::RationalTime defaultTitleLength(const time::Rational& rate);

}  // namespace zaro::app
