#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "zaro/core/Error.h"
#include "zaro/core/media/ColorInfo.h"
#include "zaro/core/model/Clip.h"

namespace zaro::render {

/// What a clip's look contains that a LUT cannot carry.
///
/// Reported rather than silently dropped. A .cube is a function from one colour
/// to another and nothing else, so anything that depends on *where* a pixel is,
/// or that changes whether it is there at all, has no representation in one --
/// and a LUT that quietly left those out would produce a file that does not
/// match the shot it was taken from, which is the one thing a look file must
/// not do.
struct LutOmissions {
    bool mask{false};
    bool vignette{false};
    bool keyer{false};
    bool effects{false};
    /// The grade reaches above white, and a 0..1 cube has nowhere to put it.
    bool aboveWhite{false};

    [[nodiscard]] bool any() const noexcept {
        return mask || vignette || keyer || effects || aboveWhite;
    }
    /// A sentence naming what was left out, for showing to somebody.
    [[nodiscard]] std::string describe() const;
};

/// Bake a clip's colour grade into a .cube.
///
/// **It carries exactly what `gradePixel` does** -- the primary, the wheels,
/// the tone curves, a look LUT and the secondary -- because that is precisely
/// the part of a clip's look that is a function of colour alone. Everything
/// else about the clip is spatial or changes coverage, and is reported through
/// `omissions` instead.
///
/// **The cube is display-referred**, sampled and written through `transfer`.
/// That is what every program that reads a .cube expects, and it is the same
/// curve the grade was judged against on the scopes -- so the file describes
/// the look somebody actually approved. The cost is stated in `omissions`: a
/// grade that lifts anything past white has no room left in a 0..1 cube, and
/// that part of it does not travel.
[[nodiscard]] Result<std::string> bakeCube(const model::Clip& clip,
                                           media::TransferFunction transfer, std::int32_t size,
                                           const std::string& title, LutOmissions* omissions);

}  // namespace zaro::render
