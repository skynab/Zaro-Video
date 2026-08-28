#include <cstdint>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/model/Clip.h"

using namespace zaro;
using Catch::Approx;

namespace {

const time::Rational k24{24, 1};
const time::Rational k60{60, 1};

time::RationalTime at(std::int64_t frames, time::Rational rate = k24) {
    return time::RationalTime{frames, rate};
}

model::Keyframe key(std::int64_t frames, double value,
                    model::Interpolation how = model::Interpolation::Linear) {
    model::Keyframe k;
    k.time = at(frames);
    k.value = value;
    k.interpolation = how;
    return k;
}

}  // namespace

TEST_CASE("a curve holds its value outside the keyframed range", "[animation]") {
    model::Curve curve;
    curve.set(key(24, 0.0));
    curve.set(key(48, 1.0));

    // Extrapolating the slope would give -1 and 2 here. Held values are the
    // only answer that cannot produce an opacity outside [0,1] from keyframes
    // that are both inside it.
    CHECK(curve.valueAtSeconds(0.0) == Approx(0.0));
    CHECK(curve.valueAtSeconds(-5.0) == Approx(0.0));
    CHECK(curve.valueAtSeconds(10.0) == Approx(1.0));

    CHECK(curve.valueAtSeconds(1.5) == Approx(0.5));
}

TEST_CASE("keyframes are kept sorted and deduplicated by time", "[animation]") {
    model::Curve curve;
    curve.set(key(48, 1.0));
    curve.set(key(0, 0.0));
    curve.set(key(24, 0.25));

    REQUIRE(curve.size() == 3);
    CHECK(curve.keyframes()[0].time == at(0));
    CHECK(curve.keyframes()[1].time == at(24));
    CHECK(curve.keyframes()[2].time == at(48));

    // Setting an existing time replaces rather than appends: two keyframes at
    // one instant would describe a zero-length segment, which has no value.
    curve.set(key(24, 0.9));
    REQUIRE(curve.size() == 3);
    CHECK(curve.at(at(24))->value == Approx(0.9));
    CHECK(curve.valueAtSeconds(1.0) == Approx(0.9));

    CHECK(curve.removeAt(at(24)));
    CHECK_FALSE(curve.removeAt(at(24)));
    CHECK(curve.size() == 2);
}

TEST_CASE("hold does not move until the next keyframe", "[animation]") {
    model::Curve curve;
    curve.set(key(0, 3.0, model::Interpolation::Hold));
    curve.set(key(24, 9.0));

    CHECK(curve.valueAtSeconds(0.0) == Approx(3.0));
    CHECK(curve.valueAtSeconds(0.99) == Approx(3.0));
    CHECK(curve.valueAtSeconds(1.0) == Approx(9.0));
}

TEST_CASE("a bezier eases and stays monotonic between its keyframes", "[animation]") {
    model::Curve curve;
    model::Keyframe start = key(0, 0.0, model::Interpolation::Bezier);
    model::Keyframe end = key(24, 1.0);
    curve.set(start);
    curve.set(end);

    // The default handles are the classic ease: it starts and ends slower than
    // linear but passes through the midpoint.
    CHECK(curve.valueAtSeconds(0.5) == Approx(0.5).margin(1e-6));
    CHECK(curve.valueAtSeconds(0.1) < 0.1);
    CHECK(curve.valueAtSeconds(0.9) > 0.9);

    double previous = -1.0;
    for (int i = 0; i <= 100; ++i) {
        const double value = curve.valueAtSeconds(i / 100.0);
        CHECK(value >= previous);
        previous = value;
    }
    CHECK(previous == Approx(1.0));
}

TEST_CASE("bezier handles that reach past each other still give one value per time",
          "[animation]") {
    // Handles longer than the segment describe a curve that doubles back, and a
    // parameter cannot have two values at one instant. Scaling them down until
    // they meet keeps their ratio, so the drawn shape survives as nearly as a
    // function can.
    model::Curve curve;
    model::Keyframe start = key(0, 0.0, model::Interpolation::Bezier);
    start.out.dx = 3.0;
    model::Keyframe end = key(24, 1.0);
    end.in.dx = 1.0;
    curve.set(start);
    curve.set(end);

    double previous = -1e9;
    for (int i = 0; i <= 200; ++i) {
        const double value = curve.valueAtSeconds(i / 200.0);
        CHECK(std::isfinite(value));
        CHECK(value >= previous - 1e-9);
        previous = value;
    }
}

TEST_CASE("a vertical handle overshoots without becoming multivalued", "[animation]") {
    // Overshoot is a legitimate thing to ask for — it is how a move lands with
    // a bounce — so a value outside the keyframed range is not a bug here. Only
    // a curve that is not a function of time is.
    model::Curve curve;
    model::Keyframe start = key(0, 0.0, model::Interpolation::Bezier);
    start.out.dy = 2.0;
    model::Keyframe end = key(24, 1.0);
    curve.set(start);
    curve.set(end);

    bool overshot = false;
    for (int i = 0; i <= 100; ++i) {
        const double value = curve.valueAtSeconds(i / 100.0);
        REQUIRE(std::isfinite(value));
        overshot = overshot || value > 1.0;
    }
    CHECK(overshot);
    CHECK(curve.valueAtSeconds(0.0) == Approx(0.0));
    CHECK(curve.valueAtSeconds(1.0) == Approx(1.0));
}

TEST_CASE("an animated clip evaluates against source time, not sequence time", "[animation]") {
    model::Clip clip;
    clip.sourceRange = time::TimeRange{at(0), at(48)};
    clip.timelineRange = time::TimeRange{at(0), at(48)};
    clip.animation.curve(model::Param::Opacity).set(key(0, 0.0));
    clip.animation.curve(model::Param::Opacity).set(key(24, 1.0));

    CHECK(clip.transformAt(at(12)).opacity == Approx(0.5));

    // Move the clip down the timeline. The fade describes something happening
    // to the picture, so it has to happen at the same frame of the picture.
    clip.timelineRange = time::TimeRange{at(100), at(48)};
    CHECK(clip.transformAt(at(112)).opacity == Approx(0.5));

    // Trim the head. The frames now start half a second in, so the fade is
    // already half done at the clip's first frame.
    clip.sourceRange = time::TimeRange{at(12), at(36)};
    clip.timelineRange = time::TimeRange{at(100), at(36)};
    CHECK(clip.transformAt(at(100)).opacity == Approx(0.5));
}

TEST_CASE("animation is smooth when the source and sequence rates differ", "[animation]") {
    // A 24fps clip on a 60fps timeline. Evaluating in source frames would hold
    // each value for two or three output frames; every output frame should get
    // its own value.
    model::Clip clip;
    clip.sourceRange = time::TimeRange{at(0), at(24)};
    clip.timelineRange = time::TimeRange{at(0, k60), at(60, k60)};
    clip.animation.curve(model::Param::Opacity).set(key(0, 0.0));
    clip.animation.curve(model::Param::Opacity).set(key(24, 1.0));

    double previous = -1.0;
    int distinct = 0;
    for (std::int64_t frame = 0; frame < 60; ++frame) {
        const double value = clip.transformAt(at(frame, k60)).opacity;
        if (value > previous + 1e-9) {
            ++distinct;
        }
        previous = value;
    }
    CHECK(distinct == 60);
}

TEST_CASE("a clip with no curves returns its static values untouched", "[animation]") {
    model::Clip clip;
    clip.sourceRange = time::TimeRange{at(0), at(48)};
    clip.timelineRange = time::TimeRange{at(0), at(48)};
    clip.transform.opacity = 0.25;
    clip.transform.scaleX = 2.0;
    clip.gainDb = -6.0;
    clip.pan = 0.5;

    CHECK(clip.transformAt(at(10)).opacity == Approx(0.25));
    CHECK(clip.transformAt(at(10)).scaleX == Approx(2.0));
    CHECK(clip.gainDbAt(at(10)) == Approx(-6.0));
    CHECK(clip.panAt(at(10)) == Approx(0.5));

    // One curve overrides only its own parameter. The rest keep their static
    // values rather than collapsing to a default.
    clip.animation.curve(model::Param::Opacity).set(key(0, 1.0));
    CHECK(clip.transformAt(at(10)).opacity == Approx(1.0));
    CHECK(clip.transformAt(at(10)).scaleX == Approx(2.0));
    CHECK(clip.gainDbAt(at(10)) == Approx(-6.0));
}

TEST_CASE("every parameter name round trips", "[animation]") {
    constexpr model::Param kAll[] = {model::Param::PositionX,       model::Param::PositionY,
                                     model::Param::ScaleX,          model::Param::ScaleY,
                                     model::Param::RotationDegrees, model::Param::AnchorX,
                                     model::Param::AnchorY,         model::Param::Opacity,
                                     model::Param::GainDb,          model::Param::Pan};
    for (model::Param param : kAll) {
        model::Param back{};
        REQUIRE(model::paramFromString(model::toString(param), back));
        CHECK(back == param);
    }

    model::Param unused{};
    CHECK_FALSE(model::paramFromString("somethingFromALaterVersion", unused));
    CHECK_FALSE(model::paramFromString(nullptr, unused));

    for (model::Interpolation how :
         {model::Interpolation::Hold, model::Interpolation::Linear, model::Interpolation::Bezier}) {
        CHECK(model::interpolationFromString(model::toString(how)) == how);
    }
}

TEST_CASE("pruning drops curves that have no keyframes left", "[animation]") {
    model::ClipAnimation animation;
    animation.curve(model::Param::Opacity).set(key(0, 1.0));
    animation.curve(model::Param::ScaleX);  // touched but never given a keyframe

    CHECK_FALSE(animation.empty());
    animation.pruneEmpty();
    CHECK(animation.find(model::Param::ScaleX) == nullptr);
    CHECK(animation.find(model::Param::Opacity) != nullptr);

    animation.curve(model::Param::Opacity).removeAt(at(0));
    animation.pruneEmpty();
    CHECK(animation.empty());
}

TEST_CASE("source and timeline time are inverses of each other", "[animation]") {
    // Keyframes are stored in source time and drawn on the timeline, so the two
    // mappings have to agree. If they drift, a keyframe is drawn somewhere its
    // own curve does not agree with.
    model::Clip clip;
    clip.sourceRange = time::TimeRange{at(500), at(50)};
    clip.timelineRange = time::TimeRange{at(100), at(50)};

    for (std::int64_t frame = 100; frame < 150; ++frame) {
        const auto source = clip.sourceTimeAt(at(frame));
        CHECK(clip.timelineTimeOf(source) == at(frame));
    }

    // And at a speed change, where the two ranges differ in length.
    clip.sourceRange = time::TimeRange{at(500), at(100)};
    clip.timelineRange = time::TimeRange{at(100), at(50)};
    for (std::int64_t frame = 100; frame < 150; ++frame) {
        const auto source = clip.sourceTimeAt(at(frame));
        CHECK(clip.timelineTimeOf(source) == at(frame));
    }

    // Across a rate difference, where a frame boundary in one is not a frame
    // boundary in the other, the round trip has to stay within a frame.
    clip.sourceRange = time::TimeRange{at(500, k24), at(48, k24)};
    clip.timelineRange = time::TimeRange{at(100, k60), at(120, k60)};
    for (std::int64_t frame = 100; frame < 220; ++frame) {
        const auto back = clip.timelineTimeOf(clip.sourceTimeAt(at(frame, k60)));
        CHECK(std::llabs(back.frames() - frame) <= 1);
    }
}
