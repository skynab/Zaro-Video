#include <cmath>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/render/EffectStack.h"
#include "zaro/core/render/RenderGraph.h"

#include "ModelFixtures.h"
#include "TestSources.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::Fixture;
using zaro::testing::SolidFrameSource;

namespace {

model::Effect blurOf(double radius) {
    model::Effect effect;
    effect.kind = model::EffectKind::Blur;
    effect.setValue(model::EffectParam::Radius, radius);
    return effect;
}

model::Effect sharpenOf(double radius, double amount) {
    model::Effect effect;
    effect.kind = model::EffectKind::Sharpen;
    effect.setValue(model::EffectParam::Radius, radius);
    effect.setValue(model::EffectParam::Amount, amount);
    return effect;
}

/// A single lit pixel in the middle of a dark frame: the impulse response of
/// whatever is applied to it.
render::RgbaImage impulse(std::int32_t size) {
    render::RgbaImage image{size, size};
    image.fill(render::Rgba{0.0F, 0.0F, 0.0F, 1.0F});
    image.at(size / 2, size / 2) = render::Rgba{1.0F, 1.0F, 1.0F, 1.0F};
    return image;
}

double totalLight(const render::RgbaImage& image) {
    double sum = 0.0;
    for (std::int32_t y = 0; y < image.height(); ++y) {
        const render::Rgba* row = image.row(y);
        for (std::int32_t x = 0; x < image.width(); ++x) {
            sum += static_cast<double>(row[x].r);
        }
    }
    return sum;
}

}  // namespace

TEST_CASE("A blur spreads light without creating or losing any", "[render][effects]") {
    render::RgbaImage image = impulse(33);
    render::RgbaImage scratch;
    const double before = totalLight(image);

    render::blur(image, scratch, 2.0F);

    // The kernel is normalised, so a blur is a redistribution. An unnormalised
    // one darkens or brightens in proportion to the radius, which reads as an
    // exposure bug rather than a blur one.
    CHECK(totalLight(image) == Approx(before).margin(0.01));
    CHECK(image.at(16, 16).r < 0.2F);
    CHECK(image.at(16, 16).r > 0.0F);
    CHECK(image.at(18, 16).r > 0.0F);
}

TEST_CASE("A blur is symmetric and falls away with distance", "[render][effects]") {
    render::RgbaImage image = impulse(33);
    render::RgbaImage scratch;
    render::blur(image, scratch, 3.0F);

    const float centre = image.at(16, 16).r;
    const float near = image.at(18, 16).r;
    const float far = image.at(22, 16).r;
    CHECK(centre > near);
    CHECK(near > far);
    // Separable means the horizontal and vertical passes must agree.
    CHECK(image.at(18, 16).r == Approx(image.at(16, 18).r));
    CHECK(image.at(14, 16).r == Approx(image.at(18, 16).r));
}

TEST_CASE("A blur of no radius is exactly the picture", "[render][effects]") {
    render::RgbaImage image = impulse(9);
    render::RgbaImage scratch;
    render::blur(image, scratch, 0.0F);
    CHECK(image.at(4, 4).r == 1.0F);
    CHECK(image.at(0, 0).r == 0.0F);
}

TEST_CASE("Blurring does not pull colour out of transparent pixels", "[render][effects]") {
    // The black-halo bug, which is what premultiplied alpha exists to prevent.
    // Half the frame is opaque white, half is transparent; blurring across the
    // boundary must fade the coverage without darkening the colour under it.
    render::RgbaImage image{32, 8};
    for (std::int32_t y = 0; y < 8; ++y) {
        render::Rgba* row = image.row(y);
        for (std::int32_t x = 0; x < 32; ++x) {
            row[x] = x < 16 ? render::Rgba{1.0F, 1.0F, 1.0F, 1.0F}
                            : render::Rgba{0.0F, 0.0F, 0.0F, 0.0F};
        }
    }
    render::RgbaImage scratch;
    render::blur(image, scratch, 2.0F);

    // Along the soft edge the values stay premultiplied white: colour equals
    // coverage. A straight-alpha blur would leave colour below coverage here,
    // which is the halo.
    for (std::int32_t x = 12; x < 20; ++x) {
        const render::Rgba pixel = image.at(x, 4);
        if (pixel.a > 0.01F && pixel.a < 0.99F) {
            CHECK(pixel.r == Approx(pixel.a).margin(1e-5));
        }
    }
}

TEST_CASE("Sharpening puts back what a blur took away", "[render][effects]") {
    render::RgbaImage image = impulse(33);
    render::RgbaImage scratch;
    render::RgbaImage scratchB;
    const float before = image.at(16, 16).r;

    render::applyEffects({sharpenOf(1.5, 1.0)}, image, scratch, scratchB);

    // The centre gains, because the detail added back is the difference
    // between it and its own blur.
    CHECK(image.at(16, 16).r > before);
    // And nothing goes negative: an overshoot into the shadows clamps at black
    // rather than inverting.
    CHECK(image.at(20, 16).r >= 0.0F);
}

TEST_CASE("Order is what a list has and a set of fields does not", "[render][effects]") {
    // The whole reason effects are a list. Blur-then-sharpen and
    // sharpen-then-blur are different pictures, and somebody has to be able to
    // say which they meant.
    render::RgbaImage scratch;
    render::RgbaImage scratchB;

    render::RgbaImage blurThenSharpen = impulse(33);
    render::applyEffects({blurOf(2.0), sharpenOf(2.0, 1.0)}, blurThenSharpen, scratch, scratchB);

    render::RgbaImage sharpenThenBlur = impulse(33);
    render::applyEffects({sharpenOf(2.0, 1.0), blurOf(2.0)}, sharpenThenBlur, scratch, scratchB);

    CHECK(blurThenSharpen.at(16, 16).r != Approx(sharpenThenBlur.at(16, 16).r));
}

TEST_CASE("A disabled effect is kept but does nothing", "[render][effects]") {
    render::RgbaImage image = impulse(17);
    render::RgbaImage scratch;
    render::RgbaImage scratchB;

    model::Effect off = blurOf(3.0);
    off.enabled = false;
    render::applyEffects({off}, image, scratch, scratchB);
    CHECK(image.at(8, 8).r == 1.0F);
    CHECK_FALSE(model::anyActive({off}));
}

TEST_CASE("A freshly added effect changes nothing", "[render][effects]") {
    // Adding an effect and having the picture jump makes it impossible to tell
    // what the effect did from what it happened to be set to.
    model::Effect fresh;
    fresh.kind = model::EffectKind::Blur;
    CHECK(fresh.value(model::EffectParam::Radius) == 0.0);
    CHECK_FALSE(model::anyActive({fresh}));

    model::Effect sharp;
    sharp.kind = model::EffectKind::Sharpen;
    CHECK(sharp.value(model::EffectParam::Amount) == 0.0);
    CHECK_FALSE(model::anyActive({sharp}));
}

TEST_CASE("An effect reaches the composited frame", "[render][effects]") {
    Fixture f;
    f.sequence().setSize(32, 32);
    SolidFrameSource source{32, 32};
    render::RenderGraph graph{source};

    // A generated rectangle, so the picture has an edge inside it for a blur to
    // soften. Blurring a clip whose image fills the frame edge to edge would
    // change nothing visible, and a test of that would pass on a blur that did
    // nothing at all.
    model::Graphic shape;
    shape.kind = model::GraphicKind::Rectangle;
    shape.width = 12.0;
    shape.height = 12.0;
    shape.red = 1.0;
    shape.green = 1.0;
    shape.blue = 1.0;
    REQUIRE(f.run(edit::makeAddGraphic(f.project, f.on(f.v1), shape, f.range(0, 50))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;

    auto sharp = graph.composite(f.sequence(), f.at(10));
    REQUIRE(sharp);
    // Just outside the rectangle: nothing there yet.
    const float outsideBefore = sharp->at(23, 16).a;
    const float insideBefore = sharp->at(16, 16).a;
    REQUIRE(insideBefore == Approx(1.0F));
    REQUIRE(outsideBefore == Approx(0.0F));

    REQUIRE(f.run(edit::makeSetEffects(f.project, f.on(f.v1), clipId, {blurOf(3.0)})));
    auto soft = graph.composite(f.sequence(), f.at(10));
    REQUIRE(soft);

    // The edge has spread outwards, and the middle has given some up.
    CHECK(soft->at(23, 16).a > 0.01F);
    CHECK(soft->at(16, 16).a < 1.0F);
}

TEST_CASE("An effect parameter that belongs to another effect is refused", "[render][effects]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;

    model::Effect wrong;
    wrong.kind = model::EffectKind::Blur;
    // A blur has no amount. Stored, it would be written to the file, read back
    // and never used -- a setting somebody made that quietly does nothing.
    wrong.setValue(model::EffectParam::Amount, 0.5);
    CHECK_FALSE(edit::makeSetEffects(f.project, f.on(f.v1), clipId, {wrong}));

    CHECK(f.run(edit::makeSetEffects(f.project, f.on(f.v1), clipId, {blurOf(2.0)})));
}

TEST_CASE("An effect stack survives a round trip through a project file", "[render][effects][io]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;

    model::Effect disabled = blurOf(4.0);
    disabled.enabled = false;
    REQUIRE(f.run(edit::makeSetEffects(f.project, f.on(f.v1), clipId,
                                       {blurOf(2.5), sharpenOf(1.0, 0.75), disabled})));

    auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    auto reloaded = io::loadProjectFromString(*text);
    REQUIRE(reloaded);

    const std::vector<model::Effect>& back =
        reloaded->project.findSequence(f.sequenceId)->findTrack(f.v1)->clips().front().effects;
    REQUIRE(back.size() == 3);
    // Order is part of the value, so it is part of the round trip.
    CHECK(back[0].kind == model::EffectKind::Blur);
    CHECK(back[0].value(model::EffectParam::Radius) == Approx(2.5));
    CHECK(back[1].kind == model::EffectKind::Sharpen);
    CHECK(back[1].value(model::EffectParam::Amount) == Approx(0.75));
    CHECK_FALSE(back[2].enabled);
}

// --- Keyframed parameters ---------------------------------------------------

namespace {

model::Effect rampedBlur(const Fixture& f, std::int64_t fromFrame, double fromRadius,
                         std::int64_t toFrame, double toRadius) {
    model::Effect effect;
    effect.kind = model::EffectKind::Blur;
    model::Curve curve;
    curve.set(model::Keyframe{f.at(fromFrame), fromRadius, model::Interpolation::Linear, {}, {}});
    curve.set(model::Keyframe{f.at(toFrame), toRadius, model::Interpolation::Linear, {}, {}});
    effect.animation[model::EffectParam::Radius] = std::move(curve);
    return effect;
}

}  // namespace

TEST_CASE("A curve overrides the static value where it exists", "[render][effects]") {
    Fixture f;
    model::Effect effect = rampedBlur(f, 0, 0.0, 100, 10.0);
    effect.setValue(model::EffectParam::Radius, 99.0);

    // The static value is ignored outright rather than blended with: an
    // animated parameter has one answer at every instant, and it is the curve's.
    CHECK(effect.valueAt(model::EffectParam::Radius, 0.0) == Approx(0.0));
    CHECK(effect.valueAt(model::EffectParam::Radius, 2.0) == Approx(5.0));
    CHECK(effect.valueAt(model::EffectParam::Radius, 4.0) == Approx(10.0));
    // And it is still there for when the stopwatch goes off again.
    CHECK(effect.value(model::EffectParam::Radius) == Approx(99.0));
}

TEST_CASE("An effect ramps over the clip", "[render][effects]") {
    Fixture f;
    f.sequence().setSize(32, 32);
    SolidFrameSource source{32, 32};
    render::RenderGraph graph{source};

    model::Graphic shape;
    shape.kind = model::GraphicKind::Rectangle;
    shape.width = 12.0;
    shape.height = 12.0;
    shape.red = 1.0;
    shape.green = 1.0;
    shape.blue = 1.0;
    REQUIRE(f.run(edit::makeAddGraphic(f.project, f.on(f.v1), shape, f.range(0, 100))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;
    // Source starts at zero for a generated clip, so source seconds and
    // timeline seconds agree and the expectations below stay readable.
    REQUIRE(f.track(f.v1).clips().front().sourceRange.start().frames() == 0);

    REQUIRE(f.run(
        edit::makeSetEffects(f.project, f.on(f.v1), clipId, {rampedBlur(f, 0, 0.0, 100, 8.0)})));

    auto atStart = graph.composite(f.sequence(), f.at(0));
    auto middle = graph.composite(f.sequence(), f.at(50));
    auto atEnd = graph.composite(f.sequence(), f.at(99));
    REQUIRE(atStart);
    REQUIRE(middle);
    REQUIRE(atEnd);

    // The middle of the rectangle is the measure that moves one way: a blur
    // conserves light, so a wider one always lowers the peak. A point just
    // outside the edge does *not* move one way -- it rises as the edge softens
    // and falls again once the same light is spread thin enough -- and an
    // assertion on it would have been a statement about the radius that
    // happened to be picked.
    CHECK(atStart->at(16, 16).a == Approx(1.0F));
    CHECK(middle->at(16, 16).a < atStart->at(16, 16).a);
    CHECK(atEnd->at(16, 16).a < middle->at(16, 16).a);
    // And the edge has moved outwards at all, which is what says the ramp ran.
    CHECK(atStart->at(23, 16).a == Approx(0.0F).margin(0.001));
    CHECK(middle->at(23, 16).a > 0.01F);
}

TEST_CASE("An effect's curves move with it", "[render][effects]") {
    // The reason the curves live on the effect rather than in the clip's
    // animation: reordering is a swap, and a swap has to carry the keyframes.
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;

    REQUIRE(f.run(edit::makeSetEffects(f.project, f.on(f.v1), clipId,
                                       {sharpenOf(1.0, 0.5), rampedBlur(f, 0, 1.0, 50, 6.0)})));
    std::vector<model::Effect> stack = f.track(f.v1).clips().front().effects;
    std::swap(stack[0], stack[1]);
    REQUIRE(f.run(edit::makeSetEffects(f.project, f.on(f.v1), clipId, stack)));

    const std::vector<model::Effect>& after = f.track(f.v1).clips().front().effects;
    REQUIRE(after.size() == 2);
    CHECK(after[0].kind == model::EffectKind::Blur);
    CHECK(after[0].isAnimated(model::EffectParam::Radius));
    CHECK(after[0].valueAt(model::EffectParam::Radius, 2.0) == Approx(6.0));
    CHECK_FALSE(after[1].isAnimated(model::EffectParam::Radius));
}

TEST_CASE("A keyframe on a parameter the effect does not take is refused", "[render][effects]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;

    model::Effect wrong = blurOf(2.0);
    model::Curve curve;
    curve.set(model::Keyframe{f.at(0), 0.5, model::Interpolation::Linear, {}, {}});
    wrong.animation[model::EffectParam::Amount] = std::move(curve);
    CHECK_FALSE(edit::makeSetEffects(f.project, f.on(f.v1), clipId, {wrong}));
}

TEST_CASE("An animated effect counts as active even where its curve reads zero",
          "[render][effects]") {
    // Conservative on purpose: deciding otherwise would mean the renderer took
    // one path on some frames of a ramp and another on the rest.
    Fixture f;
    model::Effect flat = rampedBlur(f, 0, 0.0, 50, 0.0);
    CHECK(model::anyActive({flat}));
}

TEST_CASE("An effect's keyframes survive a round trip", "[render][effects][io]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50))));
    const model::ClipId clipId = f.track(f.v1).clips().front().id;

    model::Effect ramped = rampedBlur(f, 0, 1.0, 50, 7.0);
    ramped.animation[model::EffectParam::Radius].set(
        model::Keyframe{f.at(25), 3.0, model::Interpolation::Hold, {}, {}});
    REQUIRE(f.run(edit::makeSetEffects(f.project, f.on(f.v1), clipId, {ramped})));

    auto text = io::saveProjectToString(f.project);
    REQUIRE(text);
    auto reloaded = io::loadProjectFromString(*text);
    REQUIRE(reloaded);

    const std::vector<model::Effect>& back =
        reloaded->project.findSequence(f.sequenceId)->findTrack(f.v1)->clips().front().effects;
    REQUIRE(back.size() == 1);
    const model::Curve* curve = back[0].curve(model::EffectParam::Radius);
    REQUIRE(curve != nullptr);
    REQUIRE(curve->size() == 3);
    CHECK(curve->keyframes()[1].interpolation == model::Interpolation::Hold);
    CHECK(back[0].valueAt(model::EffectParam::Radius, 1.5) == Approx(3.0));
}

namespace {

/// A single lit pixel, so where the picture went can be read off directly
/// rather than inferred from an average.
render::RgbaImage dotAt(std::int32_t width, std::int32_t height, std::int32_t x, std::int32_t y) {
    render::RgbaImage image{width, height};
    image.fill(render::Rgba{0.0F, 0.0F, 0.0F, 1.0F});
    image.at(x, y) = render::Rgba{1.0F, 1.0F, 1.0F, 1.0F};
    return image;
}

/// Where the brightest pixel ended up, and how bright it is.
std::tuple<std::int32_t, std::int32_t, float> brightest(const render::RgbaImage& image) {
    std::int32_t bestX = 0;
    std::int32_t bestY = 0;
    float best = -1.0F;
    for (std::int32_t y = 0; y < image.height(); ++y) {
        for (std::int32_t x = 0; x < image.width(); ++x) {
            if (image.at(x, y).r > best) {
                best = image.at(x, y).r;
                bestX = x;
                bestY = y;
            }
        }
    }
    return {bestX, bestY, best};
}

}  // namespace

TEST_CASE("no curvature and no zoom leaves the picture alone", "[effects][distort]") {
    render::RgbaImage image = dotAt(81, 61, 60, 20);
    const render::RgbaImage before = image.clone();
    render::RgbaImage scratch;

    render::distort(image, scratch, 0.0F, 1.0F);

    for (std::int32_t y = 0; y < image.height(); ++y) {
        for (std::int32_t x = 0; x < image.width(); ++x) {
            REQUIRE(image.at(x, y) == before.at(x, y));
        }
    }
}

TEST_CASE("curvature bends by the radial formula", "[effects][distort]") {
    // A ramp rather than a dot: resampling a single lit pixel spreads it over
    // its neighbours and the peak stops meaning "where the picture went". A
    // ramp is linear, so bilinear sampling reproduces it exactly and each
    // output pixel says precisely which source pixel it read.
    constexpr std::int32_t kCentreX = 40;
    constexpr std::int32_t kCentreY = 30;
    const double unit = std::hypot(static_cast<double>(kCentreX), static_cast<double>(kCentreY));

    auto ramp = [] {
        render::RgbaImage image{81, 61};
        for (std::int32_t y = 0; y < image.height(); ++y) {
            for (std::int32_t x = 0; x < image.width(); ++x) {
                const auto value = static_cast<float>(x) / 100.0F;
                image.at(x, y) = render::Rgba{value, value, value, 1.0F};
            }
        }
        return image;
    };

    for (const float curvature : {-0.3F, 0.4F}) {
        render::RgbaImage image = ramp();
        render::RgbaImage scratch;
        render::distort(image, scratch, curvature, 1.0F);

        for (const std::int32_t probe : {50, 60, 70}) {
            const double dx = probe - kCentreX;
            const double r = dx / unit;
            const double read = kCentreX + (dx * (1.0 + (static_cast<double>(curvature) * r * r)));
            const double got = static_cast<double>(image.at(probe, kCentreY).r) * 100.0;
            CHECK(got == Approx(read).margin(0.05));
        }
    }
}

TEST_CASE("the sign of the curvature says which way the corners go", "[effects][distort]") {
    // A dot on the mid-line, read as the brightest pixel: which side of where
    // it started it lands is all this asks, and that survives the smearing a
    // resample gives a single pixel.
    for (const auto& [curvature, expectFurther] :
         {std::pair{-0.3F, true}, std::pair{0.4F, false}}) {
        render::RgbaImage image = dotAt(81, 61, 73, 30);
        render::RgbaImage scratch;
        render::distort(image, scratch, curvature, 1.0F);

        const auto [outX, outY, level] = brightest(image);
        CHECK(outY == 30);
        CHECK(level > 0.2F);
        CHECK((outX > 73) == expectFurther);
    }
}

TEST_CASE("zoom scales about the centre", "[effects][distort]") {
    render::RgbaImage image = dotAt(81, 61, 60, 30);  // 20 pixels out
    render::RgbaImage scratch;
    render::distort(image, scratch, 0.0F, 2.0F);

    const auto [outX, outY, level] = brightest(image);
    CHECK(outY == 30);
    CHECK(level > 0.5F);
    // Twice as far out: the picture is twice the size.
    CHECK(outX == 80);
}

TEST_CASE("what the bend pulls in from outside is transparent, not smeared", "[effects][distort]") {
    render::RgbaImage image{81, 61};
    image.fill(render::Rgba{0.5F, 0.5F, 0.5F, 1.0F});
    render::RgbaImage scratch;

    // Straightening the picture means the corners read from beyond the source.
    render::distort(image, scratch, 0.6F, 1.0F);

    CHECK(image.at(0, 0).a < 0.5F);
    // And the middle, which reads from inside, is untouched.
    CHECK(image.at(40, 30).a == Approx(1.0F).margin(0.001));
}
