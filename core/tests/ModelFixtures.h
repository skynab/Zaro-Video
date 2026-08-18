#pragma once

#include <string>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/model/Project.h"

namespace zaro::testing {

/// A project with one 25fps sequence, two video tracks and one audio track,
/// plus two media references: one long enough that trims never run out of
/// source, and one deliberately short so bounds checking can be exercised.
struct Fixture {
    model::Project project;
    model::SequenceId sequenceId;
    model::TrackId v1;
    model::TrackId v2;
    model::TrackId a1;
    model::MediaRefId longMedia;
    model::MediaRefId shortMedia;
    edit::CommandStack stack;

    static constexpr std::int64_t kLongMediaFrames = 10000;
    static constexpr std::int64_t kShortMediaFrames = 100;

    Fixture() {
        const time::Rational rate = time::rates::fps25;

        model::Sequence sequence{project.ids().next<model::SequenceTag>(), "Sequence 01", rate};
        sequence.setSize(1920, 1080);
        sequenceId = sequence.id();

        v1 = project.ids().next<model::TrackTag>();
        v2 = project.ids().next<model::TrackTag>();
        a1 = project.ids().next<model::TrackTag>();
        sequence.addTrack(v1, model::TrackKind::Video, "V1");
        sequence.addTrack(v2, model::TrackKind::Video, "V2");
        sequence.addTrack(a1, model::TrackKind::Audio, "A1");
        project.addSequence(std::move(sequence));

        longMedia = addMedia("long.mov", kLongMediaFrames);
        shortMedia = addMedia("short.mov", kShortMediaFrames);
    }

    model::MediaRefId addMedia(const std::string& name, std::int64_t frames) {
        model::MediaRef ref;
        ref.id = project.ids().next<model::MediaRefTag>();
        ref.path = "/media/" + name;
        ref.name = name;
        ref.info.duration = time::Rational{frames, 25};
        media::VideoStreamInfo video;
        video.width = 1920;
        video.height = 1080;
        video.frameRate = time::rates::fps25;
        video.duration = ref.info.duration;
        ref.info.videoStreams.push_back(video);
        return project.addMedia(std::move(ref));
    }

    model::Sequence& sequence() { return *project.findSequence(sequenceId); }
    const model::Sequence& sequence() const { return *project.findSequence(sequenceId); }
    model::Track& track(model::TrackId id) { return *sequence().findTrack(id); }
    const model::Track& track(model::TrackId id) const { return *sequence().findTrack(id); }

    [[nodiscard]] edit::EditTarget on(model::TrackId id) const { return {sequenceId, id}; }

    /// A clip with distinct source and timeline positions, so a test that
    /// confuses the two fails instead of coincidentally passing.
    model::Clip clip(std::int64_t timelineStart, std::int64_t duration,
                     std::int64_t sourceStart = 500, model::MediaRefId source = {}) {
        const time::Rational rate = time::rates::fps25;
        model::Clip out;
        out.id = project.ids().next<model::ClipTag>();
        out.source = source.isValid() ? source : longMedia;
        out.name = "clip" + std::to_string(out.id.value());
        out.sourceRange = time::TimeRange{time::RationalTime{sourceStart, rate},
                                          time::RationalTime{duration, rate}};
        out.timelineRange = time::TimeRange{time::RationalTime{timelineStart, rate},
                                            time::RationalTime{duration, rate}};
        return out;
    }

    time::RationalTime at(std::int64_t frames) const {
        return time::RationalTime{frames, time::rates::fps25};
    }
    time::TimeRange range(std::int64_t start, std::int64_t duration) const {
        return time::TimeRange{at(start), at(duration)};
    }

    /// Build, execute and report. Tests that expect success use REQUIRE on it.
    bool run(Result<edit::CommandPtr> built) {
        if (!built) {
            lastError = built.error().message();
            return false;
        }
        stack.execute(project, std::move(*built));
        return true;
    }

    std::string lastError;

    /// Compact rendering of a track for readable failures:
    /// "0-50@500 50-100@600" is start-end@sourceStart.
    [[nodiscard]] std::string layout(model::TrackId id) const {
        std::string out;
        for (const model::Clip& c : track(id).clips()) {
            if (!out.empty()) {
                out += " ";
            }
            out += std::to_string(c.start().frames()) + "-" +
                   std::to_string(c.endExclusive().frames()) + "@" +
                   std::to_string(c.sourceRange.start().frames());
        }
        return out;
    }
};

}  // namespace zaro::testing

/// Like REQUIRE, but reports why the edit was refused instead of just "false".
#define ZARO_REQUIRE_EDIT(fixture, expr)                   \
    do {                                                   \
        if (!(fixture).run(expr)) {                        \
            FAIL("edit refused: " << (fixture).lastError); \
        }                                                  \
    } while (false)
