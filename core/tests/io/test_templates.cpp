#include <cstdint>
#include <filesystem>
#include <fstream>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"

#include "ModelFixtures.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::Fixture;

namespace {

const time::Rational k24{24, 1};

time::RationalTime at(std::int64_t frames) {
    return time::RationalTime{frames, k24};
}

/// A lower third with everything a template is supposed to carry on it.
model::Clip lowerThird() {
    model::Clip clip;
    clip.id = model::ClipId{7};
    clip.name = "lower third";
    clip.graphic.kind = model::GraphicKind::Text;
    clip.graphic.text = "NAME HERE";
    clip.graphic.pointSize = 64.0;
    clip.graphic.red = 0.9;
    clip.timelineRange = time::TimeRange{at(100), at(48)};
    clip.sourceRange = time::TimeRange{at(0), at(48)};
    clip.transform.positionY = -300.0;

    model::Curve opacity;
    opacity.set(model::Keyframe{at(0), 0.0, model::Interpolation::Linear, {}, {}});
    opacity.set(model::Keyframe{at(12), 1.0, model::Interpolation::Linear, {}, {}});
    opacity.set(model::Keyframe{at(36), 1.0, model::Interpolation::Linear, {}, {}});
    opacity.set(model::Keyframe{at(48), 0.0, model::Interpolation::Linear, {}, {}});
    clip.animation.curve(model::Param::Opacity) = opacity;

    clip.responsive.intro = at(12);
    clip.responsive.outro = at(12);
    clip.responsive.authored = at(48);

    model::Effect blur;
    blur.kind = model::EffectKind::Blur;
    blur.setValue(model::EffectParam::Radius, 3.0);
    clip.effects.push_back(blur);
    return clip;
}

std::string scratchPath(const char* name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove(path);
    return path.string();
}

}  // namespace

TEST_CASE("a graphic survives a round trip through a template", "[templates]") {
    const model::Clip original = lowerThird();
    const std::string path = scratchPath("zaro-template-round-trip.zarograph");
    REQUIRE(io::saveGraphicTemplate(original, path));

    auto loaded = io::loadGraphicTemplate(path);
    REQUIRE(loaded);
    CHECK(loaded->graphic.text == original.graphic.text);
    CHECK(loaded->graphic.pointSize == Approx(original.graphic.pointSize));
    CHECK(loaded->transform.positionY == Approx(original.transform.positionY));
    CHECK(loaded->responsive.intro == original.responsive.intro);
    CHECK(loaded->responsive.authored == original.responsive.authored);
    REQUIRE(loaded->effects.size() == 1);
    CHECK(loaded->effects.front().value(model::EffectParam::Radius) == Approx(3.0));

    const model::Curve* curve = loaded->animation.find(model::Param::Opacity);
    REQUIRE(curve != nullptr);
    CHECK(curve->size() == 4);

    // Everything the file records, in one comparison rather than a list of
    // fields somebody has to remember to extend: what a template is *for* is
    // producing the same picture, and that is exactly what a fingerprint says.
    model::Clip placed = *loaded;
    placed.id = original.id;
    placed.timelineRange = original.timelineRange;
    placed.sourceRange = original.sourceRange;
    CHECK(io::fingerprint(placed) == io::fingerprint(original));

    std::filesystem::remove(path);
}

TEST_CASE("only a graphic can be saved as a template", "[templates]") {
    model::Clip footage;
    footage.id = model::ClipId{3};
    footage.source = model::MediaRefId{1};
    footage.timelineRange = time::TimeRange{at(0), at(24)};
    footage.sourceRange = time::TimeRange{at(0), at(24)};

    const std::string path = scratchPath("zaro-template-refused.zarograph");
    CHECK_FALSE(io::saveGraphicTemplate(footage, path));
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("a file that is not a template is refused by name", "[templates]") {
    const std::string path = scratchPath("zaro-template-nonsense.zarograph");
    {
        std::ofstream file{path};
        file << "{\"hello\": 1}";
    }
    auto loaded = io::loadGraphicTemplate(path);
    CHECK_FALSE(loaded);
    CHECK(loaded.error().message().find("not a graphic template") != std::string::npos);
    std::filesystem::remove(path);
}

TEST_CASE("a placed template keeps its animation and takes the new length", "[templates]") {
    Fixture fixture;
    const auto sequenceId = fixture.project.activeSequence();
    const auto trackId = fixture.project.findSequence(sequenceId)->videoTracks().front().id();

    const std::string path = scratchPath("zaro-template-placed.zarograph");
    REQUIRE(io::saveGraphicTemplate(lowerThird(), path));
    auto loaded = io::loadGraphicTemplate(path);
    REQUIRE(loaded);

    // Placed at half the length it was designed at.
    edit::CommandStack commands;
    auto built = edit::makePlaceGraphicTemplate(fixture.project, {sequenceId, trackId}, *loaded,
                                                time::TimeRange{at(200), at(24)});
    REQUIRE(built);
    commands.execute(fixture.project, std::move(*built));

    const model::Track* track = fixture.project.findSequence(sequenceId)->findTrack(trackId);
    const model::Clip* placed = nullptr;
    for (const model::Clip& candidate : track->clips()) {
        if (candidate.graphic.text == "NAME HERE") {
            placed = &candidate;
        }
    }
    REQUIRE(placed != nullptr);
    // A fresh identity, whatever number that lands on: the template's own id
    // means nothing in the project it is dropped into, and two clips sharing
    // one would be two clips the timeline could not tell apart.
    CHECK(placed->id.isValid());
    for (const model::Clip& other : track->clips()) {
        if (&other != placed) {
            CHECK(other.id != placed->id);
        }
    }
    CHECK(placed->timelineRange.duration() == at(24));

    // The intro still takes twelve frames, because the responsive timing came
    // with the template: that is what makes one reusable at any length.
    CHECK(placed->transformAt(at(200)).opacity == Approx(0.0).margin(0.02));
    CHECK(placed->transformAt(at(212)).opacity == Approx(1.0).margin(0.02));
    CHECK(placed->transformAt(at(224)).opacity == Approx(0.0).margin(0.02));

    std::filesystem::remove(path);
}
