// Adobe Premiere interchange, as FCP7 XML.

#include <string>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/PremiereXml.h"

#include "ModelFixtures.h"

using namespace zaro;
using zaro::testing::Fixture;

namespace {

/// The first `<tag>value</tag>` after `from`, so a test can name what it means
/// without reaching for the XML reader the io layer keeps to itself.
std::string valueOf(const std::string& text, const std::string& tag, std::size_t from = 0) {
    const std::string open = "<" + tag + ">";
    const std::size_t begin = text.find(open, from);
    if (begin == std::string::npos) {
        return {};
    }
    const std::size_t end = text.find("</" + tag + ">", begin);
    return end == std::string::npos ? std::string{}
                                    : text.substr(begin + open.size(), end - begin - open.size());
}

}  // namespace

TEST_CASE("A timeline round trips through Premiere XML", "[io][premiere]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(100, 30, 800))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.a1), f.clip(0, 50, 500))));

    const auto text = io::writePremiereXml(f.project, f.sequenceId);
    REQUIRE(text);

    const auto back = io::readPremiereXml(*text);
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

    REQUIRE(sequence->audioTracks().front().clips().size() == 1);
    // One file, mentioned by three clips.
    REQUIRE(back->media().size() == 1);
    CHECK(back->media().front().path == "/media/long.mov");
}

TEST_CASE("A hole in a track needs no object to stand for it", "[io][premiere]") {
    // The opposite of OTIO, and the reason the two writers are not one. A
    // clipitem states its own position, so a gap is the absence of an item --
    // and a writer that emitted one anyway would put a clip on the timeline.
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(40, 10, 500))));

    const auto text = io::writePremiereXml(f.project, f.sequenceId);
    REQUIRE(text);
    CHECK(text->find("<clipitem") != std::string::npos);
    CHECK(valueOf(*text, "start") == "40");
    CHECK(valueOf(*text, "end") == "50");

    const auto back = io::readPremiereXml(*text);
    REQUIRE(back);
    const model::Track& video = back->sequences().front().videoTracks().front();
    REQUIRE(video.clips().size() == 1);
    CHECK(video.clips()[0].start().frames() == 40);
}

TEST_CASE("A rate is written as a timebase and an NTSC flag", "[io][premiere]") {
    // 23.976 is "24, pulled down", exactly. Writing 23.976 as a decimal is how
    // an edit comes back a frame per hour out.
    Fixture f;
    f.sequence().setFrameRate(time::rates::fps23_976);
    const auto text = io::writePremiereXml(f.project, f.sequenceId);
    REQUIRE(text);
    CHECK(valueOf(*text, "timebase") == "24");
    CHECK(valueOf(*text, "ntsc") == "TRUE");

    const auto back = io::readPremiereXml(*text);
    REQUIRE(back);
    CHECK(back->sequences().front().frameRate() == time::rates::fps23_976);
}

TEST_CASE("A start timecode survives", "[io][premiere]") {
    Fixture f;
    f.sequence().setStartTime(time::RationalTime{90000, time::rates::fps25});
    const auto text = io::writePremiereXml(f.project, f.sequenceId);
    REQUIRE(text);
    CHECK(valueOf(*text, "string") == "01:00:00:00");

    const auto back = io::readPremiereXml(*text);
    REQUIRE(back);
    CHECK(back->sequences().front().startTime().frames() == 90000);
}

TEST_CASE("Media is declared once and referred to after", "[io][premiere]") {
    // Two full <file> definitions of one path arrive in Premiere as two copies
    // of the same footage in the bin.
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 20, 0))));
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(30, 20, 100))));

    const auto text = io::writePremiereXml(f.project, f.sequenceId);
    REQUIRE(text);
    // The path appears once; the id appears on every clip that reads it.
    const std::size_t first = text->find("<pathurl>");
    REQUIRE(first != std::string::npos);
    CHECK(text->find("<pathurl>", first + 1) == std::string::npos);
    CHECK(text->find(R"(<file id="file-1"/>)") != std::string::npos);

    const auto back = io::readPremiereXml(*text);
    REQUIRE(back);
    REQUIRE(back->media().size() == 1);
    const model::Track& video = back->sequences().front().videoTracks().front();
    REQUIRE(video.clips().size() == 2);
    // Both clips resolved to it, including the one that only saw the reference.
    CHECK(video.clips()[0].source == video.clips()[1].source);
    CHECK(video.clips()[0].source.isValid());
}

TEST_CASE("A path with spaces and accents comes back as itself", "[io][premiere]") {
    Fixture f;
    f.project.mediaMutable().front().path = "/Volumes/Card A/Café Take 1 & 2.mov";
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 20, 0))));

    const auto text = io::writePremiereXml(f.project, f.sequenceId);
    REQUIRE(text);
    CHECK(text->find("%20") != std::string::npos);

    const auto back = io::readPremiereXml(*text);
    REQUIRE(back);
    REQUIRE(back->media().size() == 1);
    CHECK(back->media().front().path == "/Volumes/Card A/Café Take 1 & 2.mov");
}

TEST_CASE("Timeline frames and source frames are counted at different rates", "[io][premiere]") {
    // A 24fps file on a 25fps sequence. Reading <in> at the sequence rate is
    // the mistake this format invites, and it retimes every such clip.
    Fixture f;
    const model::MediaRefId media = f.addMedia("film.mov", 5000);
    for (model::MediaRef& ref : f.project.mediaMutable()) {
        if (ref.id == media) {
            ref.info.videoStreams.front().frameRate = time::rates::fps24;
        }
    }
    model::Clip clip = f.clip(100, 50, 0, media);
    // 48 source frames at 24fps under 50 timeline frames at 25fps: the same two
    // seconds, counted twice.
    clip.sourceRange = time::TimeRange{time::RationalTime{240, time::rates::fps24},
                                       time::RationalTime{48, time::rates::fps24}};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), clip)));

    const auto text = io::writePremiereXml(f.project, f.sequenceId);
    REQUIRE(text);

    const auto back = io::readPremiereXml(*text);
    REQUIRE(back);
    const model::Clip& read = back->sequences().front().videoTracks().front().clips().front();
    CHECK(read.timelineRange.start().rate() == time::rates::fps25);
    CHECK(read.timelineRange.duration().frames() == 50);
    CHECK(read.sourceRange.start().rate() == time::rates::fps24);
    CHECK(read.sourceRange.start().frames() == 240);
    CHECK(read.sourceRange.duration().frames() == 48);
}

TEST_CASE("Muted and locked tracks stay that way", "[io][premiere]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 20, 0))));
    f.track(f.v1).setMuted(true);
    f.track(f.v2).setLocked(true);

    const auto text = io::writePremiereXml(f.project, f.sequenceId);
    REQUIRE(text);
    const auto back = io::readPremiereXml(*text);
    REQUIRE(back);
    const model::Sequence& sequence = back->sequences().front();
    CHECK(sequence.videoTracks()[0].isMuted());
    CHECK_FALSE(sequence.videoTracks()[0].isLocked());
    CHECK(sequence.videoTracks()[1].isLocked());
}

TEST_CASE("Markers survive, and a point marker keeps no false duration", "[io][premiere]") {
    Fixture f;
    model::Marker point;
    point.id = f.project.ids().next<model::MarkerTag>();
    point.range = f.range(30, 1);
    point.name = "look here";
    point.note = "the focus goes";
    model::Marker span;
    span.id = f.project.ids().next<model::MarkerTag>();
    span.range = f.range(100, 40);
    span.name = "section";
    f.sequence().setMarkers({point, span});

    const auto text = io::writePremiereXml(f.project, f.sequenceId);
    REQUIRE(text);
    const auto back = io::readPremiereXml(*text);
    REQUIRE(back);
    const auto& markers = back->sequences().front().markers();
    REQUIRE(markers.size() == 2);
    CHECK(markers[0].name == "look here");
    CHECK(markers[0].note == "the focus goes");
    CHECK(markers[0].isPoint());
    CHECK(markers[1].range.start().frames() == 100);
    CHECK(markers[1].range.duration().frames() == 40);
}

TEST_CASE("A sequence wrapped in a project element is still found", "[io][premiere]") {
    // Premiere's own exports nest the sequence under <project><children>.
    const std::string text = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE xmeml>
<xmeml version="4">
  <project>
    <name>Some job</name>
    <children>
      <bin><name>Footage</name></bin>
      <sequence id="sequence-1">
        <name>From Premiere</name>
        <rate><timebase>30</timebase><ntsc>TRUE</ntsc></rate>
        <media>
          <video>
            <format><samplecharacteristics><width>3840</width><height>2160</height></samplecharacteristics></format>
            <track>
              <clipitem id="clipitem-1">
                <name>A005_C012</name>
                <rate><timebase>30</timebase><ntsc>TRUE</ntsc></rate>
                <start>0</start><end>120</end><in>1000</in><out>1120</out>
                <file id="file-1">
                  <name>A005_C012.mov</name>
                  <pathurl>file://localhost/Volumes/Card/A005_C012.mov</pathurl>
                </file>
              </clipitem>
            </track>
          </video>
        </media>
      </sequence>
    </children>
  </project>
</xmeml>)";

    const auto back = io::readPremiereXml(text);
    REQUIRE(back);
    const model::Sequence& sequence = back->sequences().front();
    CHECK(sequence.name() == "From Premiere");
    CHECK(sequence.frameRate() == time::rates::fps29_97);
    CHECK(sequence.width() == 3840);
    CHECK(sequence.height() == 2160);
    REQUIRE(sequence.videoTracks().size() == 1);
    REQUIRE(sequence.videoTracks().front().clips().size() == 1);
    const model::Clip& clip = sequence.videoTracks().front().clips().front();
    CHECK(clip.name == "A005_C012");
    CHECK(clip.timelineRange.duration().frames() == 120);
    CHECK(clip.sourceRange.start().frames() == 1000);
    REQUIRE(back->media().size() == 1);
    CHECK(back->media().front().path == "/Volumes/Card/A005_C012.mov");
}

TEST_CASE("An item positioned by a transition is left out rather than invented", "[io][premiere]") {
    // -1 means "the transition decides". There is no transition model to hang
    // it on, and a clip placed at a guessed position is worse than one absent
    // from a cut that is otherwise right. What follows it must not move.
    const std::string text = R"(<xmeml version="4"><sequence>
        <rate><timebase>25</timebase><ntsc>FALSE</ntsc></rate>
        <media><video><track>
          <clipitem><name>inside a transition</name><start>-1</start><end>-1</end></clipitem>
          <clipitem><name>after</name><start>100</start><end>150</end><in>0</in><out>50</out></clipitem>
        </track></video></media>
      </sequence></xmeml>)";

    const auto back = io::readPremiereXml(text);
    REQUIRE(back);
    const model::Track& video = back->sequences().front().videoTracks().front();
    REQUIRE(video.clips().size() == 1);
    CHECK(video.clips()[0].name == "after");
    CHECK(video.clips()[0].start().frames() == 100);
}

TEST_CASE("Overlapping items are skipped rather than asserted on", "[io][premiere]") {
    // A track in this model holds no overlaps, and nothing stops a file from
    // containing some. An import is the one place the input is somebody else's,
    // so this has to be a refusal of the item, not a crash.
    const std::string text = R"(<xmeml version="4"><sequence>
        <rate><timebase>25</timebase><ntsc>FALSE</ntsc></rate>
        <media><video><track>
          <clipitem><name>first</name><start>0</start><end>100</end><in>0</in><out>100</out></clipitem>
          <clipitem><name>on top of it</name><start>50</start><end>150</end><in>0</in><out>100</out></clipitem>
          <clipitem><name>clear of both</name><start>200</start><end>250</end><in>0</in><out>50</out></clipitem>
        </track></video></media>
      </sequence></xmeml>)";

    const auto back = io::readPremiereXml(text);
    REQUIRE(back);
    const model::Track& video = back->sequences().front().videoTracks().front();
    REQUIRE(video.clips().size() == 2);
    CHECK(video.clips()[0].name == "first");
    CHECK(video.clips()[1].name == "clear of both");
}

TEST_CASE("A clip with no media keeps its place", "[io][premiere]") {
    // A title or a colour matte generated in the other program: there is
    // nothing to point a media reference at, and dropping the clip would move
    // nothing but would leave a hole where somebody put something.
    const std::string text = R"(<xmeml version="4"><sequence>
        <rate><timebase>25</timebase><ntsc>FALSE</ntsc></rate>
        <media><video><track>
          <clipitem><name>Title 01</name><start>10</start><end>60</end><in>0</in><out>50</out>
            <file id="file-1"><name>Title 01</name></file>
          </clipitem>
        </track></video></media>
      </sequence></xmeml>)";

    const auto back = io::readPremiereXml(text);
    REQUIRE(back);
    CHECK(back->media().empty());
    const model::Track& video = back->sequences().front().videoTracks().front();
    REQUIRE(video.clips().size() == 1);
    CHECK(video.clips()[0].name == "Title 01");
    CHECK(video.clips()[0].start().frames() == 10);
    CHECK_FALSE(video.clips()[0].source.isValid());
}

TEST_CASE("Something that is not an xmeml document is refused", "[io][premiere]") {
    CHECK_FALSE(io::readPremiereXml("not xml at all"));
    CHECK_FALSE(io::readPremiereXml("<otherthing/>"));
    CHECK_FALSE(io::readPremiereXml(R"(<xmeml version="4"><project/></xmeml>)"));
    CHECK_FALSE(io::loadPremiereXml("/definitely/not/a/file.xml"));
    CHECK_FALSE(io::writePremiereXml(model::Project{}, model::SequenceId{}));
}

TEST_CASE("Saving and loading a file agrees with the strings", "[io][premiere]") {
    Fixture f;
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 50, 500))));
    const std::string path = std::string{ZARO_SCRATCH_DIR} + "/interchange.xml";
    REQUIRE(io::savePremiereXml(f.project, f.sequenceId, path));

    const auto loaded = io::loadPremiereXml(path);
    REQUIRE(loaded);
    const auto text = io::writePremiereXml(f.project, f.sequenceId);
    REQUIRE(text);
    const auto parsed = io::readPremiereXml(*text);
    REQUIRE(parsed);
    CHECK(loaded->sequences().front().videoTracks().front().clips().size() ==
          parsed->sequences().front().videoTracks().front().clips().size());
}
