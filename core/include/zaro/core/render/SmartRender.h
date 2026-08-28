#pragma once

#include <cstdint>
#include <string>

#include "zaro/core/model/Project.h"
#include "zaro/core/time/RationalTime.h"

namespace zaro::render {

/// What the output has to be, for a copy to produce it.
struct SmartRenderTarget {
    std::int32_t width{0};
    std::int32_t height{0};
    time::Rational frameRate;
    /// The codec the export was asked for. Empty means the caller has not
    /// decided, which is not something to guess about: a copy that changed the
    /// codec is not a copy.
    std::string videoCodec;
    bool includeAudio{true};
    time::Rational audioSampleRate;
    std::int32_t audioChannels{0};
};

/// Whether an export can be done by copying the source's packets, and if not,
/// why not.
struct SmartRenderPlan {
    bool possible{false};
    /// Always filled in. A render that quietly fell back to re-encoding would
    /// leave somebody wondering why the export took twenty minutes.
    std::string reason;

    model::MediaRefId media;
    /// Where in the source file to start, and how many frames to take.
    time::RationalTime sourceStart;
    std::int64_t frames{0};
    bool copyAudio{false};
};

/// Work out whether a range of a sequence is just a piece of a file.
///
/// **Everything has to be untouched.** One grade, one transform, one mask, one
/// opacity keyframe, and the exported picture is not the source's picture any
/// more. The check is deliberately a list of "is this at its default" rather
/// than a guess: a new field added to `Clip` and forgotten here would make a
/// copy that silently dropped whatever it does, so the test for this walks a
/// clip's fields and is the place that fails when one is added.
///
/// **One clip over the whole range, on one visible track.** Anything layered
/// over it, any transition, any gap, and there is compositing to do.
///
/// **Frame for frame, at the same size and rate, in the same codec.** A copy
/// that resized or changed rate is not a copy; if the export settings differ
/// from the source in any of those, this refuses and the ordinary path runs.
///
/// **Audio too, or not at all.** Copying video while re-encoding audio is a
/// third path with its own timestamp arithmetic; where the audio does not
/// match, the whole export is re-encoded and says so.
[[nodiscard]] SmartRenderPlan smartRenderPlan(const model::Project& project,
                                              const model::Sequence& sequence,
                                              std::int64_t startFrame, std::int64_t frameCount,
                                              const SmartRenderTarget& target);

}  // namespace zaro::render
