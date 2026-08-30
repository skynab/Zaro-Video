// Final Cut Pro interchange, as FCPXML.
//
// The other half of the FCP7 tests next door, and deliberately not a copy of
// them: the two formats agree on almost nothing but the vendor whose name is on
// them. What is pinned here is what is different -- seconds instead of frames,
// lanes instead of tracks, and a timeline counted from its own start timecode.

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/FinalCutXml.h"

#include "ModelFixtures.h"

using namespace zaro;
using zaro::testing::Fixture;

namespace {

/// The value of `attribute` on the first element named `element`, so a test can
/// name what it means without reaching for the XML reader the io layer keeps to
/// itself.
std::string attributeOf(const std::string& text, const std::string& element,
                        const std::string& attribute, std::size_t from = 0) {
    const std::size_t begin = text.find("<" + element, from);
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find('>', begin);
    const std::string open = attribute + "=\"";
    const std::size_t at = text.find(open, begin);
    if (at == std::string::npos || at > end) {
        return {};
    }
    const std::size_t stop = text.find('"', at + open.size());
    return text.substr(at + open.size(), stop - at - open.size());
}

}  // namespace

TEST_CASE("A timeline round trips through FCPXML", "[io][finalcut]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100, 30, 800))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v2), f.clip(20, 40, 200))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 50, 500))));

    const auto text = io::writeFcpXml(f.project, f.sequenceId);
    REQUIRE(text);

    const auto back = io::readFcpXml(*text);
    REQUIRE(back);
    const model::Sequence* sequence = &back->sequences().front();
    CHECK(sequence->name() == "Sequence 01");
    CHECK(sequence->frameRate() == f.sequence().frameRate());
    CHECK(sequence->width() == 1920);
    CHECK(sequence->height() == 1080);
    REQUIRE(sequence->videoTracks().size() == 2);
    REQUIRE(sequence->audioTracks().size() == 1);

    const model::Track& video = sequence->videoTracks().front();
    REQUIRE(video.clips().size() == 2);
    CHECK(video.clips()[0].start().frames() == 0);
    CHECK(video.clips()[0].duration().frames() == 50);
    CHECK(video.clips()[1].start().frames() == 100);
    CHECK(video.clips()[1].duration().frames() == 30);
    // The trims, which are the other half of a cut.
    CHECK(video.clips()[0].sourceRange.start().frames() == 500);
    CHECK(video.clips()[1].sourceRange.start().frames() == 800);
    CHECK(video.clips()[1].sourceRange.duration().frames() == 30);

    // V2 stacks above V1 and comes back above it, which is the whole content of
    // the lane mapping.
    REQUIRE(sequence->videoTracks()[1].clips().size() == 1);
    CHECK(sequence->videoTracks()[1].clips()[0].start().frames() == 20);
    REQUIRE(sequence->audioTracks().front().clips().size() == 1);

    // One file, mentioned by four clips.
    REQUIRE(back->media().size() == 1);
    CHECK(back->media().front().path == "/media/long.mov");
}

TEST_CASE("Tracks become numbered lanes, sound below the storyline", "[io][finalcut]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 10, 0))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v2), f.clip(0, 10, 0))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 10, 0))));

    const auto text = io::writeFcpXml(f.project, f.sequenceId);
    REQUIRE(text);
    CHECK(text->find("lane=\"1\"") != std::string::npos);
    CHECK(text->find("lane=\"2\"") != std::string::npos);
    CHECK(text->find("lane=\"-1\"") != std::string::npos);
    // The spine holds one gap and nothing else: every clip is anchored to it.
    CHECK(attributeOf(*text, "gap", "name") == "Timeline");
}

TEST_CASE("Time is seconds, and a pulled-down rate stays exact", "[io][finalcut]") {
    // 23.976 is 1001/24000 of a second per frame, written as that ratio and
    // nothing else. A decimal here is how an edit comes back a frame per hour
    // out, which is the mistake the OTIO reader needs a table of rates to undo.
    Fixture f;
    f.sequence().setFrameRate(time::rates::fps23_976);
    const auto text = io::writeFcpXml(f.project, f.sequenceId);
    REQUIRE(text);
    CHECK(attributeOf(*text, "format", "frameDuration") == "1001/24000s");

    const auto back = io::readFcpXml(*text);
    REQUIRE(back);
    CHECK(back->sequences().front().frameRate() == time::rates::fps23_976);
}

TEST_CASE("A cut that starts at an hour is offset from its own timecode", "[io][finalcut]") {
    // FCPXML places everything against the sequence's timecode, so a clip at
    // frame zero of a one-hour cut sits at 3600s. Writing zero-based offsets
    // under a non-zero tcStart puts the whole timeline before its own start.
    Fixture f;
    f.sequence().setStartTime(time::RationalTime{90000, time::rates::fps25});
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 25, 0))));

    const auto text = io::writeFcpXml(f.project, f.sequenceId);
    REQUIRE(text);
    CHECK(attributeOf(*text, "sequence", "tcStart") == "3600s");
    CHECK(attributeOf(*text, "asset-clip", "offset") == "3600s");

    const auto back = io::readFcpXml(*text);
    REQUIRE(back);
    const model::Sequence& sequence = back->sequences().front();
    CHECK(sequence.startTime().frames() == 90000);
    REQUIRE(sequence.videoTracks().front().clips().size() == 1);
    CHECK(sequence.videoTracks().front().clips()[0].start().frames() == 0);
}

TEST_CASE("Offsets that ignore the start timecode are left where they are", "[io][finalcut]") {
    // Not everything that writes this format counts from tcStart. Subtracting
    // it from a file whose offsets already begin at zero would push the whole
    // cut off the front of the timeline, so the shift is only taken when there
    // is room for it.
    const std::string text = R"(<?xml version="1.0" encoding="UTF-8"?>
<fcpxml version="1.9">
    <resources>
        <format id="r1" frameDuration="1/25s" width="1920" height="1080"/>
    </resources>
    <library><event><project name="Zero based">
        <sequence format="r1" duration="10s" tcStart="3600s" tcFormat="NDF">
            <spine>
                <gap name="Timeline" offset="0s" start="0s" duration="10s">
                    <gap lane="1" name="shot" offset="0s" duration="2s"/>
                </gap>
            </spine>
        </sequence>
    </project></event></library>
</fcpxml>)";

    const auto back = io::readFcpXml(text);
    REQUIRE(back);
    const model::Sequence& sequence = back->sequences().front();
    CHECK(sequence.startTime().frames() == 90000);
    REQUIRE(sequence.videoTracks().size() == 1);
    REQUIRE(sequence.videoTracks().front().clips().size() == 1);
    CHECK(sequence.videoTracks().front().clips()[0].start().frames() == 0);
}

TEST_CASE("A hole needs no object to stand for it", "[io][finalcut]") {
    // Every anchored item states its own offset, so the space between two clips
    // is an absence -- the same as FCP7 XML and the opposite of OTIO.
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(40, 10, 500))));

    const auto text = io::writeFcpXml(f.project, f.sequenceId);
    REQUIRE(text);
    CHECK(attributeOf(*text, "asset-clip", "offset") == "8/5s");

    const auto back = io::readFcpXml(*text);
    REQUIRE(back);
    const model::Track& video = back->sequences().front().videoTracks().front();
    REQUIRE(video.clips().size() == 1);
    CHECK(video.clips()[0].start().frames() == 40);
    CHECK(video.clips()[0].duration().frames() == 10);
}

TEST_CASE("Media is declared once in the resources and referred to after", "[io][finalcut]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 20, 0))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(30, 20, 100))));

    const auto text = io::writeFcpXml(f.project, f.sequenceId);
    REQUIRE(text);
    std::size_t assets = 0;
    for (std::size_t at = text->find("<asset "); at != std::string::npos;
         at = text->find("<asset ", at + 1)) {
        ++assets;
    }
    CHECK(assets == 1);

    const auto back = io::readFcpXml(*text);
    REQUIRE(back);
    CHECK(back->media().size() == 1);
}

TEST_CASE("A path with spaces and accents comes back as itself", "[io][finalcut]") {
    Fixture f;
    const model::MediaRefId media = f.addMedia("caf\xc3\xa9 take 2.mov", 500);
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 20, 0, media))));

    const auto text = io::writeFcpXml(f.project, f.sequenceId);
    REQUIRE(text);
    CHECK(text->find("%20") != std::string::npos);

    const auto back = io::readFcpXml(*text);
    REQUIRE(back);
    REQUIRE(back->media().size() == 1);
    CHECK(back->media().front().path == "/media/caf\xc3\xa9 take 2.mov");
}

TEST_CASE("Timeline and source are both seconds, whatever the rates", "[io][finalcut]") {
    // 24fps footage on a 25fps timeline. Counting in seconds is what makes this
    // case unremarkable here and a trap in FCP7 XML, where the two ranges are
    // frames of different things.
    Fixture f;
    model::MediaRef ref;
    ref.id = f.project.ids().next<model::MediaRefTag>();
    ref.path = "/media/film.mov";
    ref.name = "film.mov";
    ref.info.duration = time::Rational{1000, 24};
    media::VideoStreamInfo stream;
    stream.width = 1920;
    stream.height = 1080;
    stream.frameRate = time::rates::fps24;
    stream.duration = ref.info.duration;
    ref.info.videoStreams.push_back(stream);
    const model::MediaRefId film = f.project.addMedia(std::move(ref));

    model::Clip clip = f.clip(0, 50, 0, film);
    // Twelve seconds into the file, counted in the file's own frames.
    clip.sourceRange = time::TimeRange{time::RationalTime{288, time::rates::fps24},
                                       time::RationalTime{48, time::rates::fps24}};
    clip.timelineRange = time::TimeRange{time::RationalTime{0, time::rates::fps25},
                                         time::RationalTime{50, time::rates::fps25}};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), clip)));

    const auto text = io::writeFcpXml(f.project, f.sequenceId);
    REQUIRE(text);
    CHECK(attributeOf(*text, "asset-clip", "start") == "12s");

    const auto back = io::readFcpXml(*text);
    REQUIRE(back);
    const model::Clip& read = back->sequences().front().videoTracks().front().clips().front();
    // Back at the file's rate, not the sequence's: 288 frames of 24, not of 25.
    CHECK(read.sourceRange.start().rate() == time::rates::fps24);
    CHECK(read.sourceRange.start().frames() == 288);
    CHECK(read.timelineRange.start().rate() == time::rates::fps25);
    CHECK(read.timelineRange.duration().frames() == 50);
}

TEST_CASE("Markers survive, and a point marker keeps no false duration", "[io][finalcut]") {
    Fixture f;
    std::vector<model::Marker> markers;
    model::Marker point;
    point.id = f.project.ids().next<model::MarkerTag>();
    point.name = "check this";
    point.range = f.range(30, 1);
    markers.push_back(point);
    model::Marker span;
    span.id = f.project.ids().next<model::MarkerTag>();
    span.name = "section";
    span.note = "needs a grade";
    span.range = f.range(100, 50);
    markers.push_back(span);
    f.sequence().setMarkers(std::move(markers));

    const auto text = io::writeFcpXml(f.project, f.sequenceId);
    REQUIRE(text);

    const auto back = io::readFcpXml(*text);
    REQUIRE(back);
    const std::vector<model::Marker>& read = back->sequences().front().markers();
    REQUIRE(read.size() == 2);
    CHECK(read[0].name == "check this");
    CHECK(read[0].range.start().frames() == 30);
    CHECK(read[0].isPoint());
    CHECK(read[1].name == "section");
    CHECK(read[1].note == "needs a grade");
    CHECK(read[1].range.start().frames() == 100);
    CHECK(read[1].range.duration().frames() == 50);
}

TEST_CASE("A marker Final Cut left on a clip arrives on the timeline", "[io][finalcut]") {
    // FCPXML markers hang off whatever they were dropped on, in that item's own
    // source time. This model's markers belong to the sequence, so the note is
    // moved to where it points rather than thrown away with its container.
    const std::string text = R"(<?xml version="1.0" encoding="UTF-8"?>
<fcpxml version="1.9">
    <resources>
        <format id="r1" frameDuration="1/25s" width="1920" height="1080"/>
        <asset id="r2" name="shot" start="0s" duration="100s" hasVideo="1" format="r1">
            <media-rep kind="original-media" src="file:///media/shot.mov"/>
        </asset>
    </resources>
    <library><event><project name="Notes">
        <sequence format="r1" duration="10s" tcStart="0s" tcFormat="NDF">
            <spine>
                <asset-clip ref="r2" offset="4s" name="shot" start="20s" duration="6s">
                    <marker start="21s" duration="1/25s" value="here"/>
                </asset-clip>
            </spine>
        </sequence>
    </project></event></library>
</fcpxml>)";

    const auto back = io::readFcpXml(text);
    REQUIRE(back);
    const model::Sequence& sequence = back->sequences().front();
    REQUIRE(sequence.markers().size() == 1);
    // One second into a clip that starts four seconds in: five seconds, 125.
    CHECK(sequence.markers()[0].range.start().frames() == 125);
    CHECK(sequence.markers()[0].name == "here");
}

TEST_CASE("A clip on the primary storyline is read as V1", "[io][finalcut]") {
    // What Final Cut itself writes: the cut in the spine, sound connected below.
    const std::string text = R"(<?xml version="1.0" encoding="UTF-8"?>
<fcpxml version="1.9">
    <resources>
        <format id="r1" frameDuration="1/25s" width="1920" height="1080"/>
        <asset id="r2" name="shot" start="0s" duration="100s" hasVideo="1" hasAudio="1" format="r1">
            <media-rep kind="original-media" src="file:///media/shot.mov"/>
        </asset>
        <asset id="r3" name="music" start="0s" duration="100s" hasAudio="1" audioChannels="2">
            <media-rep kind="original-media" src="file:///media/music.wav"/>
        </asset>
    </resources>
    <library><event><project name="Cut">
        <sequence format="r1" duration="20s" tcStart="0s" tcFormat="NDF" audioRate="48k">
            <spine>
                <asset-clip ref="r2" offset="0s" name="one" start="0s" duration="4s"/>
                <asset-clip ref="r2" offset="4s" name="two" start="10s" duration="6s">
                    <asset-clip ref="r3" lane="-1" offset="10s" name="bed" start="0s" duration="8s"/>
                    <asset-clip ref="r2" lane="1" offset="10s" name="over" start="30s" duration="2s"/>
                </asset-clip>
            </spine>
        </sequence>
    </project></event></library>
</fcpxml>)";

    const auto back = io::readFcpXml(text);
    REQUIRE(back);
    const model::Sequence& sequence = back->sequences().front();
    CHECK(sequence.name() == "Cut");
    REQUIRE(sequence.videoTracks().size() == 2);
    REQUIRE(sequence.audioTracks().size() == 1);

    const model::Track& v1 = sequence.videoTracks()[0];
    REQUIRE(v1.clips().size() == 2);
    CHECK(v1.clips()[0].name == "one");
    CHECK(v1.clips()[1].start().frames() == 100);

    // Anchored at 10s into a clip whose own start is 10s and which sits at 4s:
    // the connected clip is at 4s on the timeline.
    const model::Track& v2 = sequence.videoTracks()[1];
    REQUIRE(v2.clips().size() == 1);
    CHECK(v2.clips()[0].name == "over");
    CHECK(v2.clips()[0].start().frames() == 100);
    REQUIRE(sequence.audioTracks()[0].clips().size() == 1);
    CHECK(sequence.audioTracks()[0].clips()[0].name == "bed");

    CHECK(back->media().size() == 2);
    CHECK(sequence.audioSampleRate() == time::rates::hz48000);
}

TEST_CASE("A compound clip in the resources is not the timeline", "[io][finalcut]") {
    // `<media><sequence>` is how a compound clip is stored, and it lives in the
    // resources. A reader that took the first sequence it found would import
    // somebody's compound clip and call it the cut.
    const std::string text = R"(<?xml version="1.0" encoding="UTF-8"?>
<fcpxml version="1.9">
    <resources>
        <format id="r1" frameDuration="1/25s" width="1920" height="1080"/>
        <media id="r9" name="Compound">
            <sequence format="r1" duration="5s" tcStart="0s">
                <spine/>
            </sequence>
        </media>
    </resources>
    <library><event><project name="The Cut">
        <sequence format="r1" duration="10s" tcStart="0s" tcFormat="NDF">
            <spine>
                <gap name="Timeline" offset="0s" start="0s" duration="10s"/>
            </spine>
        </sequence>
    </project></event></library>
</fcpxml>)";

    const auto back = io::readFcpXml(text);
    REQUIRE(back);
    CHECK(back->sequences().front().name() == "The Cut");
}

TEST_CASE("Only the live take of an audition arrives", "[io][finalcut]") {
    const std::string text = R"(<?xml version="1.0" encoding="UTF-8"?>
<fcpxml version="1.9">
    <resources>
        <format id="r1" frameDuration="1/25s" width="1920" height="1080"/>
        <asset id="r2" name="shot" start="0s" duration="100s" hasVideo="1" format="r1">
            <media-rep kind="original-media" src="file:///media/shot.mov"/>
        </asset>
    </resources>
    <library><event><project name="Auditioned">
        <sequence format="r1" duration="10s" tcStart="0s" tcFormat="NDF">
            <spine>
                <audition offset="0s" lane="0">
                    <asset-clip ref="r2" offset="0s" name="picked" start="0s" duration="4s"/>
                    <asset-clip ref="r2" offset="0s" name="rejected" start="8s" duration="4s"/>
                </audition>
            </spine>
        </sequence>
    </project></event></library>
</fcpxml>)";

    const auto back = io::readFcpXml(text);
    REQUIRE(back);
    const model::Track& video = back->sequences().front().videoTracks().front();
    REQUIRE(video.clips().size() == 1);
    CHECK(video.clips()[0].name == "picked");
}

TEST_CASE("A clip with no media keeps its place", "[io][finalcut]") {
    // A title, a shape, a nested sequence and an adjustment layer all write as
    // a positioned gap with a name: the shape of the cut survives and what
    // filled the slot does not.
    Fixture f;
    model::Clip clip = f.clip(10, 30);
    clip.source = {};
    clip.name = "Lower third";
    clip.graphic.kind = model::GraphicKind::Rectangle;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), clip)));

    const auto text = io::writeFcpXml(f.project, f.sequenceId);
    REQUIRE(text);

    const auto back = io::readFcpXml(*text);
    REQUIRE(back);
    const model::Track& video = back->sequences().front().videoTracks().front();
    REQUIRE(video.clips().size() == 1);
    CHECK(video.clips()[0].name == "Lower third");
    CHECK(video.clips()[0].start().frames() == 10);
    CHECK(video.clips()[0].duration().frames() == 30);
    CHECK_FALSE(video.clips()[0].source.isValid());
    CHECK(back->media().empty());
}

TEST_CASE("A disabled clip and an audio role survive", "[io][finalcut]") {
    Fixture f;
    model::Clip clip = f.clip(0, 20);
    clip.enabled = false;
    clip.role = model::AudioRole::Music;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), clip)));

    const auto text = io::writeFcpXml(f.project, f.sequenceId);
    REQUIRE(text);
    CHECK(text->find("audioRole=\"music\"") != std::string::npos);

    const auto back = io::readFcpXml(*text);
    REQUIRE(back);
    const model::Clip& read = back->sequences().front().audioTracks().front().clips().front();
    CHECK_FALSE(read.enabled);
    CHECK(read.role == model::AudioRole::Music);
}

TEST_CASE("Something that is not an FCPXML document is refused", "[io][finalcut]") {
    CHECK_FALSE(io::readFcpXml("<xmeml version=\"4\"><sequence/></xmeml>"));
    CHECK_FALSE(io::readFcpXml("not xml at all"));
    // Well formed, and empty of anything to import.
    CHECK_FALSE(io::readFcpXml("<fcpxml version=\"1.9\"><resources/></fcpxml>"));
}

TEST_CASE("Saving and loading a file agrees with the strings", "[io][finalcut]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 25, 100))));

    const std::string path = std::string{ZARO_SCRATCH_DIR} + "/finalcut.fcpxml";
    REQUIRE(io::saveFcpXml(f.project, f.sequenceId, path));

    const auto loaded = io::loadFcpXml(path);
    REQUIRE(loaded);
    const model::Track& video = loaded->sequences().front().videoTracks().front();
    REQUIRE(video.clips().size() == 1);
    CHECK(video.clips()[0].sourceRange.start().frames() == 100);

    CHECK_FALSE(io::loadFcpXml(std::string{ZARO_SCRATCH_DIR} + "/nothing-here.fcpxml"));
}
