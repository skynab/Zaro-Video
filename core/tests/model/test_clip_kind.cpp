#include <string_view>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/model/ClipKind.h"

using namespace zaro;
using model::Clip;
using model::ClipKind;
using model::GraphicKind;
using model::SequenceId;
using model::TrackKind;

TEST_CASE("A clip with nothing special on it is ordinary media", "[model][clipkind]") {
    const Clip clip;
    CHECK(model::clipKindOf(clip, TrackKind::Video) == ClipKind::VideoMedia);
    CHECK(model::clipKindOf(clip, TrackKind::Audio) == ClipKind::AudioMedia);
}

TEST_CASE("A graphic is classified by what it draws", "[model][clipkind]") {
    Clip clip;

    clip.graphic.kind = GraphicKind::Rectangle;
    CHECK(model::clipKindOf(clip, TrackKind::Video) == ClipKind::Shape);
    clip.graphic.kind = GraphicKind::Ellipse;
    CHECK(model::clipKindOf(clip, TrackKind::Video) == ClipKind::Shape);

    // Text is its own kind rather than a shape that happens to have words in
    // it: what somebody changes about a title is not what they change about a
    // rectangle.
    clip.graphic.kind = GraphicKind::Text;
    CHECK(model::clipKindOf(clip, TrackKind::Video) == ClipKind::Text);
}

TEST_CASE("Adjustment, nested and multicam clips are recognised", "[model][clipkind]") {
    Clip adjustment;
    adjustment.adjustment = true;
    CHECK(model::clipKindOf(adjustment, TrackKind::Video) == ClipKind::Adjustment);

    Clip nested;
    nested.nested = SequenceId{7};
    CHECK(model::clipKindOf(nested, TrackKind::Video) == ClipKind::Nested);

    Clip multicam;
    multicam.angles.push_back({});
    multicam.angles.push_back({});
    CHECK(model::clipKindOf(multicam, TrackKind::Video) == ClipKind::Multicam);
}

TEST_CASE("A clip on an audio track is sound whatever else is set on it",
          "[model][clipkind]") {
    // Nothing else on the list is audible, so the track wins. Without this a
    // graphic dragged onto an audio track would offer a colour picker for
    // something nobody can see.
    Clip clip;
    clip.graphic.kind = GraphicKind::Text;
    clip.adjustment = true;
    CHECK(model::clipKindOf(clip, TrackKind::Audio) == ClipKind::AudioMedia);
}

TEST_CASE("An adjustment layer outranks a graphic on the same clip", "[model][clipkind]") {
    // The render graph checks `adjustment` before it looks for a picture to
    // draw, so the classification has to agree with it or the panel would
    // describe a clip the renderer treats differently.
    Clip clip;
    clip.adjustment = true;
    clip.graphic.kind = GraphicKind::Rectangle;
    CHECK(model::clipKindOf(clip, TrackKind::Video) == ClipKind::Adjustment);
}

TEST_CASE("Only the kinds that draw something of their own have a picture",
          "[model][clipkind]") {
    CHECK(model::hasPicture(ClipKind::VideoMedia));
    CHECK(model::hasPicture(ClipKind::Shape));
    CHECK(model::hasPicture(ClipKind::Text));
    CHECK(model::hasPicture(ClipKind::Multicam));
    CHECK(model::hasPicture(ClipKind::Nested));
    // Sound has no picture, and an adjustment layer has none of its own --
    // moving one would take the correction off what it was made for.
    CHECK_FALSE(model::hasPicture(ClipKind::AudioMedia));
    CHECK_FALSE(model::hasPicture(ClipKind::Adjustment));
}

TEST_CASE("Only the kinds that open a file read media", "[model][clipkind]") {
    CHECK(model::readsMedia(ClipKind::VideoMedia));
    CHECK(model::readsMedia(ClipKind::AudioMedia));
    CHECK(model::readsMedia(ClipKind::Multicam));
    CHECK_FALSE(model::readsMedia(ClipKind::Shape));
    CHECK_FALSE(model::readsMedia(ClipKind::Text));
    CHECK_FALSE(model::readsMedia(ClipKind::Adjustment));
    CHECK_FALSE(model::readsMedia(ClipKind::Nested));
}

TEST_CASE("Every kind names itself", "[model][clipkind]") {
    for (const ClipKind kind : model::allClipKinds()) {
        CHECK(std::string_view{model::toString(kind)}.size() > 0);
    }
}
