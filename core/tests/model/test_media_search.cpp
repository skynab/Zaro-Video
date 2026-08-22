#include <catch2/catch_test_macros.hpp>

#include "zaro/core/model/MediaSearch.h"

using namespace zaro;

namespace {

model::MediaRef shot() {
    model::MediaRef ref;
    ref.id = model::MediaRefId{1};
    ref.name = "A007_C012";
    ref.path = "/Volumes/CARD_A/DCIM/A007_C012.mov";

    media::VideoStreamInfo video;
    video.codecName = "prores";
    video.width = 1920;
    video.height = 1080;
    video.frameRate = time::rates::fps25;
    ref.info.videoStreams.push_back(video);

    media::AudioStreamInfo audio;
    audio.codecName = "pcm_s16le";
    audio.channelCount = 2;
    ref.info.audioStreams.push_back(audio);

    ref.info.duration = time::Rational{35, 1};
    return ref;
}

}  // namespace

TEST_CASE("a media reference is findable by its name and its folder", "[search]") {
    const model::MediaRef ref = shot();
    CHECK(model::matchesSearch(ref, "a007"));
    CHECK(model::matchesSearch(ref, "CARD_A"));
    CHECK(model::matchesSearch(ref, "c012.mov"));
    CHECK_FALSE(model::matchesSearch(ref, "b013"));
}

TEST_CASE("technical facts are searchable, not just the name", "[search]") {
    const model::MediaRef ref = shot();
    CHECK(model::matchesSearch(ref, "prores"));
    CHECK(model::matchesSearch(ref, "1920x1080"));
    CHECK(model::matchesSearch(ref, "1080"));
    CHECK(model::matchesSearch(ref, "25fps"));
    CHECK(model::matchesSearch(ref, "pcm_s16le"));
    CHECK(model::matchesSearch(ref, "2ch"));
    CHECK_FALSE(model::matchesSearch(ref, "h264"));
    CHECK_FALSE(model::matchesSearch(ref, "720"));
}

TEST_CASE("every word has to match, in any order", "[search]") {
    const model::MediaRef ref = shot();
    CHECK(model::matchesSearch(ref, "prores 1080"));
    CHECK(model::matchesSearch(ref, "1080 prores"));
    // The second word is the one that rules it out, which is the whole point
    // of typing two.
    CHECK_FALSE(model::matchesSearch(ref, "prores 720"));
}

TEST_CASE("notes are searchable", "[search]") {
    model::MediaRef ref = shot();
    ref.notes = "wide, take 3, boom in shot";
    CHECK(model::matchesSearch(ref, "boom"));
    CHECK(model::matchesSearch(ref, "take"));
    CHECK(model::matchesSearch(ref, "boom prores"));
    CHECK_FALSE(model::matchesSearch(ref, "closeup"));
}

TEST_CASE("an empty search matches everything", "[search]") {
    CHECK(model::matchesSearch(shot(), ""));
    CHECK(model::matchesSearch(shot(), "   "));
}

TEST_CASE("case does not matter", "[search]") {
    const model::MediaRef ref = shot();
    CHECK(model::matchesSearch(ref, "ProRes"));
    CHECK(model::matchesSearch(ref, "PRORES"));
}

TEST_CASE("having a proxy is a thing you can search for", "[search]") {
    model::MediaRef ref = shot();
    CHECK_FALSE(model::matchesSearch(ref, "proxy"));
    ref.proxyPath = "/Volumes/CARD_A/DCIM/A007_C012-proxy.mov";
    CHECK(model::matchesSearch(ref, "proxy"));
}
