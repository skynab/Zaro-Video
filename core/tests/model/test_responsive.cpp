#include <cstdint>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/model/Clip.h"

using namespace zaro;
using Catch::Approx;

namespace {

const time::Rational k24{24, 1};

time::RationalTime at(std::int64_t frames) {
    return time::RationalTime{frames, k24};
}

/// A title 48 frames long that fades up over its first 12 frames, sits, and
/// fades out over its last 12 -- the shape nearly every lower third has.
model::Clip title() {
    model::Clip clip;
    clip.graphic.kind = model::GraphicKind::Text;
    clip.graphic.text = "hello";
    clip.sourceRange = time::TimeRange{at(0), at(48)};
    clip.timelineRange = time::TimeRange{at(0), at(48)};

    model::Curve opacity;
    opacity.set(model::Keyframe{at(0), 0.0, model::Interpolation::Linear, {}, {}});
    opacity.set(model::Keyframe{at(12), 1.0, model::Interpolation::Linear, {}, {}});
    opacity.set(model::Keyframe{at(36), 1.0, model::Interpolation::Linear, {}, {}});
    opacity.set(model::Keyframe{at(48), 0.0, model::Interpolation::Linear, {}, {}});
    clip.animation.curve(model::Param::Opacity) = opacity;
    return clip;
}

/// Trim the tail, the way a ripple trim does: both ranges shorten together.
void shortenTo(model::Clip& clip, std::int64_t frames) {
    clip.sourceRange = time::TimeRange{clip.sourceRange.start(), at(frames)};
    clip.timelineRange = time::TimeRange{clip.timelineRange.start(), at(frames)};
}

double opacityAt(const model::Clip& clip, std::int64_t frame) {
    return clip.transformAt(at(frame)).opacity;
}

}  // namespace

TEST_CASE("without responsive timing a trim cuts the exit off", "[responsive]") {
    model::Clip clip = title();
    shortenTo(clip, 24);

    // Keyframes are glued to the picture, so a trim does not compress the
    // animation -- it removes the part that no longer fits. The fade up still
    // takes twelve frames, and the fade out never happens at all: the title
    // is at full strength on the last frame and then gone. That is the problem
    // responsive timing exists to solve.
    CHECK(opacityAt(clip, 6) == Approx(0.5).margin(0.05));
    CHECK(opacityAt(clip, 12) == Approx(1.0).margin(0.02));
    CHECK(opacityAt(clip, 24) == Approx(1.0).margin(0.02));
}

TEST_CASE("a protected intro runs at the speed it was drawn at", "[responsive]") {
    model::Clip clip = title();
    clip.responsive.intro = at(12);
    clip.responsive.outro = at(12);
    clip.responsive.authored = at(48);
    shortenTo(clip, 24);

    // The fade up still takes twelve frames, whatever the clip's length became.
    CHECK(opacityAt(clip, 0) == Approx(0.0).margin(0.02));
    CHECK(opacityAt(clip, 6) == Approx(0.5).margin(0.05));
    CHECK(opacityAt(clip, 12) == Approx(1.0).margin(0.02));
}

TEST_CASE("a protected outro stays glued to the end", "[responsive]") {
    model::Clip clip = title();
    clip.responsive.intro = at(12);
    clip.responsive.outro = at(12);
    clip.responsive.authored = at(48);
    shortenTo(clip, 24);

    // Twelve frames before the new end, the fade out is starting; at the end it
    // has finished. Without the protection the exit would have run and gone
    // long before this.
    CHECK(opacityAt(clip, 12) == Approx(1.0).margin(0.02));
    CHECK(opacityAt(clip, 18) == Approx(0.5).margin(0.05));
    CHECK(opacityAt(clip, 24) == Approx(0.0).margin(0.02));
}

TEST_CASE("the middle stretches when the clip grows", "[responsive]") {
    model::Clip clip = title();
    clip.responsive.intro = at(12);
    clip.responsive.outro = at(12);
    clip.responsive.authored = at(48);
    shortenTo(clip, 96);  // twice as long as it was authored

    CHECK(opacityAt(clip, 12) == Approx(1.0).margin(0.02));
    // The hold has stretched to fill the middle, so the picture is still up
    // three quarters of the way through, where an unprotected clip would be
    // fading.
    CHECK(opacityAt(clip, 72) == Approx(1.0).margin(0.02));
    CHECK(opacityAt(clip, 90) == Approx(0.5).margin(0.05));
    CHECK(opacityAt(clip, 96) == Approx(0.0).margin(0.02));
}

TEST_CASE("a clip shorter than its own animation squeezes both ends", "[responsive]") {
    model::Clip clip = title();
    clip.responsive.intro = at(12);
    clip.responsive.outro = at(12);
    clip.responsive.authored = at(48);
    shortenTo(clip, 12);  // less than the intro and outro together

    // Both ends still happen -- faster. Dropping the exit to protect the intro
    // would be a missing animation rather than a quick one.
    CHECK(opacityAt(clip, 0) == Approx(0.0).margin(0.02));
    CHECK(opacityAt(clip, 6) == Approx(1.0).margin(0.05));
    CHECK(opacityAt(clip, 12) == Approx(0.0).margin(0.02));
}

TEST_CASE("responsive timing off is the same as never setting it", "[responsive]") {
    model::Clip protectedClip = title();
    protectedClip.responsive.intro = at(0);
    protectedClip.responsive.outro = at(0);
    protectedClip.responsive.authored = at(48);
    model::Clip plain = title();
    shortenTo(protectedClip, 30);
    shortenTo(plain, 30);

    for (std::int64_t frame = 0; frame <= 30; frame += 3) {
        CHECK(opacityAt(protectedClip, frame) == Approx(opacityAt(plain, frame)).margin(0.001));
    }
}
