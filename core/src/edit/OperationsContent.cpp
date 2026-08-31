// What a clip is made of, and how it is timed.
//
// Blend and captions, adjustment layers, multicam angles, nesting and
// graphics; then time remapping, freeze frames and speed; then the look --
// masks, effects, wheels, keyer, curves and LUTs.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "zaro/core/Check.h"
#include "zaro/core/edit/Operations.h"
#include "zaro/core/media/Waveform.h"

#include "OperationsCommon.h"

namespace zaro::edit {
namespace {

using model::Clip;
using model::ClipId;
using model::Project;
using model::Sequence;
using model::Track;
using model::TrackId;
using time::RationalTime;
using time::TimeRange;

// The helpers more than one operation file needs; see OperationsCommon.h.
using detail::idText;
using detail::lookupClip;
using detail::makeCommand;
using detail::modifyClip;

}  // namespace

Result<CommandPtr> makeSetBlendMode(Project& project, const EditTarget& target, ClipId clipId,
                                    model::BlendMode blend) {
    return modifyClip(project, target, clipId,
                      std::string{"Set blend mode to "} + model::toString(blend),
                      "blend:" + idText(clipId), [blend](Clip& clip) { clip.blend = blend; });
}

Result<CommandPtr> makeSetCaptions(Project& project, model::SequenceId sequenceId,
                                   const model::CaptionTrack& captions) {
    if (project.findSequence(sequenceId) == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }
    return makeCommand(sequenceId, "Set captions", {},
                       [captions](Sequence& sequence) { sequence.captions() = captions; });
}

Result<CommandPtr> makeAddAdjustment(Project& project, const EditTarget& target,
                                     const time::TimeRange& range) {
    if (range.isEmpty()) {
        return Error{ErrorCode::InvalidData, "an adjustment layer needs a duration"};
    }
    Clip clip;
    clip.id = project.ids().next<model::ClipTag>();
    clip.adjustment = true;
    clip.name = "adjustment";
    clip.timelineRange = range;
    // Its own length, so trims have something to work against, the same as a
    // generated shape.
    clip.sourceRange =
        time::TimeRange{time::RationalTime{0, range.start().rate()}, range.duration()};
    return makeOverwrite(project, target, clip);
}

Result<CommandPtr> makeMulticam(Project& project, const EditTarget& target,
                                const std::vector<Clip::Angle>& angles,
                                const time::TimeRange& range) {
    if (angles.empty()) {
        return Error{ErrorCode::InvalidData, "a multicam clip needs at least one angle"};
    }
    if (range.isEmpty()) {
        return Error{ErrorCode::InvalidData, "a multicam clip needs a duration"};
    }
    for (const Clip::Angle& angle : angles) {
        if (project.findMedia(angle.media) == nullptr) {
            return Error{ErrorCode::NotFound, "an angle names media that is not in the project"};
        }
    }

    Clip clip;
    clip.id = project.ids().next<model::ClipTag>();
    clip.angles = angles;
    clip.activeAngle = 0;
    clip.name = angles.front().name.empty() ? "multicam" : angles.front().name;
    clip.source = angles.front().media;
    clip.timelineRange = range;
    // The group's own time, from zero: each angle's offset says where that is
    // in its own material, so the clip's source range is the group's.
    clip.sourceRange =
        time::TimeRange{time::RationalTime{0, range.start().rate()}, range.duration()};
    return makeOverwrite(project, target, clip);
}

Result<CommandPtr> makeSetAngleOffsets(
    Project& project, const EditTarget& target, ClipId clipId,
    const std::vector<std::pair<std::int32_t, time::RationalTime>>& offsets) {
    auto found = lookupClip(project, target, clipId);
    if (!found) {
        return found.error();
    }
    const Clip& existing = **found;
    if (!existing.isMulticam()) {
        return Error{ErrorCode::InvalidData, "that clip has no angles"};
    }
    if (offsets.empty()) {
        return Error{ErrorCode::InvalidData, "there are no offsets to set"};
    }
    for (const auto& [angle, offset] : offsets) {
        if (angle < 0 || angle >= static_cast<std::int32_t>(existing.angles.size())) {
            return Error{ErrorCode::InvalidData, "there is no such angle"};
        }
        if (offset.rate().den() == 0 || offset.rate().num() <= 0) {
            return Error{ErrorCode::InvalidData, "an offset has to be a time"};
        }
    }

    return modifyClip(project, target, clipId, "Sync angles", "angles:" + idText(clipId),
                      [offsets](Clip& clip) {
                          for (const auto& [angle, offset] : offsets) {
                              clip.angles[static_cast<std::size_t>(angle)].offset = offset;
                          }
                      });
}

Result<CommandPtr> makeSwitchAngle(Project& project, const EditTarget& target, ClipId clipId,
                                   std::int32_t angle, const time::RationalTime& at) {
    auto found = lookupClip(project, target, clipId);
    if (!found) {
        return found.error();
    }
    const Clip& existing = **found;
    if (!existing.isMulticam()) {
        return Error{ErrorCode::InvalidData, "that clip has no angles"};
    }
    if (angle < 0 || angle >= static_cast<std::int32_t>(existing.angles.size())) {
        return Error{ErrorCode::InvalidData, "there is no such angle"};
    }
    if (angle == existing.activeAngle) {
        return Error{ErrorCode::InvalidData, "that angle is already live"};
    }

    const time::RationalTime when = at.rescaledTo(existing.start().rate());
    if (!existing.timelineRange.contains(when)) {
        return Error{ErrorCode::InvalidData, "that moment is not inside the clip"};
    }

    // Switching at the very first frame is not a cut, it is a choice about the
    // whole clip -- and splitting there would leave a piece of no length.
    const bool atStart = when == existing.start();
    Clip before = existing;
    Clip after = existing;
    after.id = project.ids().next<model::ClipTag>();
    after.activeAngle = angle;
    before.timelineRange = time::TimeRange::fromStartEnd(existing.start(), when);
    before.sourceRange =
        time::TimeRange::fromStartEnd(existing.sourceRange.start(), existing.sourceTimeAt(when));
    after.timelineRange = time::TimeRange::fromStartEnd(when, existing.endExclusive());
    after.sourceRange = time::TimeRange::fromStartEnd(existing.sourceTimeAt(when),
                                                      existing.sourceRange.endExclusive());

    const TrackId trackId = target.track;
    return makeCommand(target.sequence, "Switch angle", {},
                       [clipId, before, after, atStart, trackId](Sequence& sequence) {
                           Track* track = sequence.findTrack(trackId);
                           if (track == nullptr) {
                               return;
                           }
                           std::vector<Clip> clips = track->clips();
                           std::vector<Clip> rebuilt;
                           rebuilt.reserve(clips.size() + 1);
                           for (Clip& clip : clips) {
                               if (clip.id != clipId) {
                                   rebuilt.push_back(std::move(clip));
                                   continue;
                               }
                               if (atStart) {
                                   Clip whole = after;
                                   whole.id = clipId;
                                   whole.timelineRange = clip.timelineRange;
                                   whole.sourceRange = clip.sourceRange;
                                   rebuilt.push_back(std::move(whole));
                                   continue;
                               }
                               rebuilt.push_back(before);
                               rebuilt.push_back(after);
                           }
                           track->setClips(std::move(rebuilt));
                       });
}

Result<CommandPtr> makeNestSequence(Project& project, const EditTarget& target,
                                    model::SequenceId nestedId, const time::RationalTime& at) {
    const Sequence* inner = project.findSequence(nestedId);
    if (inner == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }
    if (project.nestingWouldCycle(target.sequence, nestedId)) {
        return Error{ErrorCode::InvalidData, "that would put a sequence inside itself"};
    }
    if (inner->duration().frames() <= 0) {
        return Error{ErrorCode::InvalidData, "that sequence is empty"};
    }

    const Sequence* outer = project.findSequence(target.sequence);
    if (outer == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }

    Clip clip;
    clip.id = project.ids().next<model::ClipTag>();
    clip.nested = nestedId;
    clip.name = inner->name();
    // The whole of it, from its own start: a nested clip's source range is a
    // range of the inner sequence's timeline, so this is the same shape of
    // thing as a clip covering the whole of a file.
    const time::RationalTime duration = inner->duration().rescaledTo(outer->frameRate());
    clip.sourceRange =
        time::TimeRange{time::RationalTime{0, inner->frameRate()}, inner->duration()};
    clip.timelineRange = time::TimeRange{at.rescaledTo(outer->frameRate()), duration};
    return makeOverwrite(project, target, clip);
}

Result<CommandPtr> makeAddGraphic(Project& project, const EditTarget& target,
                                  const model::Graphic& graphic, const time::TimeRange& range) {
    if (range.isEmpty()) {
        return Error{ErrorCode::InvalidData, "a graphic needs a duration"};
    }
    if (!graphic.isSet()) {
        return Error{ErrorCode::InvalidData, "that is not a graphic"};
    }
    Clip clip;
    clip.id = project.ids().next<model::ClipTag>();
    clip.graphic = graphic;
    clip.name = model::autoNameFor(graphic);
    clip.timelineRange = range;
    // Its own length, so a trim has something to trim against.
    clip.sourceRange =
        time::TimeRange{time::RationalTime{0, range.start().rate()}, range.duration()};
    return makeOverwrite(project, target, clip);
}

Result<CommandPtr> makePinTo(Project& project, const EditTarget& target, ClipId clipId,
                             ClipId hostId) {
    const model::Sequence* sequence = project.findSequence(target.sequence);
    if (sequence == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }
    if (hostId.isValid()) {
        if (hostId == clipId) {
            return Error{ErrorCode::InvalidData, "a clip cannot be pinned to itself"};
        }
        const model::Clip* host = model::findClip(*sequence, hostId);
        if (host == nullptr) {
            return Error{ErrorCode::NotFound, "there is no such clip to pin to"};
        }
        // Walk the chain the pin would create. Bounded by the number of clips
        // that could be in it, so a file that already contains a cycle cannot
        // make this loop either.
        const model::Clip* step = host;
        for (std::size_t guard = 0; step != nullptr && guard < 64; ++guard) {
            if (step->id == clipId) {
                return Error{ErrorCode::InvalidData,
                             "that would pin these clips to each other in a loop"};
            }
            step = model::findClip(*sequence, step->pinnedTo);
        }
    }
    return modifyClip(project, target, clipId, hostId.isValid() ? "Pin to clip" : "Unpin",
                      "pin:" + idText(clipId), [hostId](Clip& clip) { clip.pinnedTo = hostId; });
}

Result<CommandPtr> makePlaceGraphicTemplate(Project& project, const EditTarget& target,
                                            const Clip& templateClip,
                                            const time::TimeRange& range) {
    if (range.isEmpty()) {
        return Error{ErrorCode::InvalidData, "a graphic needs a duration"};
    }
    if (!templateClip.graphic.isSet()) {
        return Error{ErrorCode::InvalidData, "that template has no graphic in it"};
    }
    Clip clip = templateClip;
    // A new identity and a new place. Everything else -- the animation, the
    // responsive timing, the effects, the mask -- is what was saved, because
    // that is what somebody saved a template for.
    clip.id = project.ids().next<model::ClipTag>();
    clip.timelineRange = range;
    clip.sourceRange =
        time::TimeRange{time::RationalTime{0, range.start().rate()}, range.duration()};
    return makeOverwrite(project, target, clip);
}

Result<CommandPtr> makeSetTimeRemapped(Project& project, const EditTarget& target, ClipId clipId,
                                       bool remapped) {
    auto found = lookupClip(project, target, clipId);
    if (!found) {
        return found.error();
    }
    const Clip& existing = **found;
    if (existing.isTimeRemapped() == remapped) {
        return Error{ErrorCode::InvalidData,
                     remapped ? "that clip is already time remapped" : "that clip is not remapped"};
    }
    if (!remapped) {
        return modifyClip(project, target, clipId, "Remove time remapping",
                          "remap:" + idText(clipId),
                          [](Clip& clip) { clip.animation.erase(model::Param::TimeRemap); });
    }

    // The identity: what the clip already plays, said as a curve. Measured at
    // the first and last frames rather than at the range's ends, because the
    // out point is exclusive and a keyframe there would describe a frame the
    // clip does not show.
    const time::Rational& rate = existing.timelineRange.start().rate();
    const time::RationalTime lastFrame = existing.endExclusive() - time::RationalTime{1, rate};

    model::Keyframe first;
    first.time = existing.baseSourceTimeAt(existing.start());
    first.value = first.time.toSecondsDouble();
    model::Keyframe last;
    last.time = existing.baseSourceTimeAt(lastFrame);
    last.value = last.time.toSecondsDouble();
    if (last.time == first.time) {
        return Error{ErrorCode::InvalidData, "that clip is too short to remap"};
    }

    return modifyClip(project, target, clipId, "Time remap", "remap:" + idText(clipId),
                      [first, last](Clip& clip) {
                          model::Curve& curve = clip.animation.curve(model::Param::TimeRemap);
                          curve.set(first);
                          curve.set(last);
                      });
}

Result<CommandPtr> makeFreezeFrame(Project& project, const EditTarget& target, ClipId clipId,
                                   const time::RationalTime& at) {
    auto found = lookupClip(project, target, clipId);
    if (!found) {
        return found.error();
    }
    const Clip& existing = **found;
    const time::Rational& rate = existing.timelineRange.start().rate();
    const time::RationalTime when = at.rescaledTo(rate);
    if (!existing.timelineRange.contains(when)) {
        return Error{ErrorCode::InvalidData, "that moment is not inside the clip"};
    }

    // The frame showing now -- through any remap already on the clip, because
    // what somebody means by "freeze this" is the picture in front of them.
    const double frozen = existing.sourceTimeAt(when).toSecondsDouble();
    const time::RationalTime lastFrame = existing.endExclusive() - time::RationalTime{1, rate};

    model::Keyframe first;
    first.time = existing.baseSourceTimeAt(existing.start());
    first.value = frozen;
    // Held rather than linear: two keyframes of equal value would freeze
    // correctly today and thaw the moment somebody dragged either of them.
    first.interpolation = model::Interpolation::Hold;
    model::Keyframe last;
    last.time = existing.baseSourceTimeAt(lastFrame);
    last.value = frozen;
    last.interpolation = model::Interpolation::Hold;
    if (last.time == first.time) {
        return Error{ErrorCode::InvalidData, "that clip is too short to freeze"};
    }

    return modifyClip(project, target, clipId, "Freeze frame", "freeze:" + idText(clipId),
                      [first, last](Clip& clip) {
                          model::Curve& curve = clip.animation.curve(model::Param::TimeRemap);
                          curve = model::Curve{};
                          curve.set(first);
                          curve.set(last);
                      });
}

Result<CommandPtr> makeSetSpeed(Project& project, const EditTarget& target, ClipId clipId,
                                double speed, bool reversed, bool ripple) {
    auto found = lookupClip(project, target, clipId);
    if (!found) {
        return found.error();
    }
    const Clip& existing = **found;
    if (!std::isfinite(speed) || speed <= 0.0) {
        // Direction is the `reversed` flag, not a negative number: a speed of
        // -2 and a reversed speed of 2 would be two ways to say one thing, and
        // the pair would eventually disagree.
        return Error{ErrorCode::InvalidData, "speed has to be a positive number"};
    }

    const time::Rational& rate = existing.timelineRange.start().rate();
    const time::Rational seconds =
        existing.sourceRange.duration().toSeconds() / time::Rational::approximate(speed);
    const time::RationalTime duration = time::RationalTime::fromSeconds(seconds, rate);
    if (duration.frames() <= 0) {
        return Error{ErrorCode::InvalidData, "that speed leaves the clip with no length"};
    }

    Clip retimed = existing;
    retimed.reversed = reversed;
    retimed.timelineRange = time::TimeRange{existing.timelineRange.start(), duration};

    const time::RationalTime shift = duration - existing.timelineRange.duration();

    // Without the ripple, a clip that got longer has to have somewhere to get
    // longer into. Checked here rather than discovered in `Track::setClips`,
    // whose non-overlap invariant is an assertion -- so slowing a clip into its
    // neighbour was a crash rather than a refusal.
    if (!ripple && shift > time::RationalTime{0, rate}) {
        auto where = detail::locate(project, target);
        if (!where) {
            return where.error();
        }
        for (const Clip& other : where->track->clips()) {
            if (other.id == clipId) {
                continue;
            }
            if (other.start() < retimed.timelineRange.endExclusive() &&
                retimed.timelineRange.start() < other.endExclusive()) {
                return Error{ErrorCode::InvalidData,
                             "there is not room for that speed without moving what follows"};
            }
        }
    }
    return makeCommand(
        target.sequence, "Change speed", "speed:" + idText(clipId),
        [clipId, retimed, shift, ripple, trackId = target.track](Sequence& sequence) {
            Track* track = sequence.findTrack(trackId);
            if (track == nullptr) {
                return;
            }
            std::vector<Clip> clips = track->clips();
            for (Clip& clip : clips) {
                if (clip.id == clipId) {
                    clip = retimed;
                } else if (ripple && clip.start() >= retimed.timelineRange.start() &&
                           clip.id != clipId) {
                    clip.timelineRange = time::TimeRange{clip.start() + shift, clip.duration()};
                }
            }
            track->setClips(std::move(clips));
        });
}

Result<CommandPtr> makeSetMask(Project& project, const EditTarget& target, ClipId clipId,
                               const model::Mask& mask) {
    for (const double value :
         {mask.width, mask.height, mask.centreX, mask.centreY, mask.cornerRadius, mask.feather}) {
        if (!std::isfinite(value)) {
            return Error{ErrorCode::InvalidData, "a mask has to be real numbers"};
        }
    }
    return modifyClip(project, target, clipId, "Adjust mask", "mask:" + idText(clipId),
                      [mask](Clip& clip) { clip.mask = mask; });
}

Result<CommandPtr> makeSetGraphic(Project& project, const EditTarget& target, ClipId clipId,
                                  const model::Graphic& graphic) {
    for (const double value :
         {graphic.width, graphic.height, graphic.centreX, graphic.centreY, graphic.cornerRadius,
          graphic.feather, graphic.red, graphic.green, graphic.blue, graphic.alpha}) {
        if (!std::isfinite(value)) {
            return Error{ErrorCode::InvalidData, "a graphic has to be real numbers"};
        }
    }
    return modifyClip(project, target, clipId, "Adjust graphic", "graphic:" + idText(clipId),
                      [graphic](Clip& clip) {
                          // Keep the name in step, but only while it is still
                          // the one that was generated for it. Somebody who
                          // renamed a title "lower third" meant it, and having
                          // that replaced by the words the next time they were
                          // edited would be the panel undoing their work.
                          if (clip.name == model::autoNameFor(clip.graphic)) {
                              clip.name = model::autoNameFor(graphic);
                          }
                          clip.graphic = graphic;
                      });
}

Result<CommandPtr> makeSetLut(Project& project, const EditTarget& target, ClipId clipId,
                              const model::LutRef& lut) {
    if (!std::isfinite(lut.amount)) {
        return Error{ErrorCode::InvalidData, "the LUT amount has to be a real number"};
    }
    return modifyClip(project, target, clipId, lut.path.empty() ? "Clear LUT" : "Set LUT",
                      "lut:" + idText(clipId), [lut](Clip& clip) { clip.lut = lut; });
}

Result<CommandPtr> makeReplaceSource(Project& project, const EditTarget& target, ClipId clipId,
                                     model::MediaRefId media) {
    auto found = lookupClip(project, target, clipId);
    if (!found) {
        return found.error();
    }
    const Clip& existing = **found;
    const model::MediaRef* ref = project.findMedia(media);
    if (ref == nullptr) {
        return Error{ErrorCode::NotFound, "no such media in this project"};
    }
    if (existing.nested.isValid() || existing.graphic.isSet()) {
        return Error{ErrorCode::InvalidData, "that clip has no media to replace"};
    }
    if (existing.isMulticam()) {
        // Which angle would it replace? Answering that would be guessing, and
        // the operation that changes an angle's media is the one that sets the
        // angles.
        return Error{ErrorCode::InvalidData, "a multicam clip's angles are replaced together"};
    }

    const time::Rational& rate = existing.sourceRange.start().rate();
    const time::RationalTime available = time::RationalTime::fromSeconds(ref->info.duration, rate);
    if (available < existing.sourceRange.duration()) {
        return Error{ErrorCode::InvalidData, "that media is shorter than the clip"};
    }

    // Keep the in point where it was if the new file reaches that far, and
    // slide back to the last position that fits if it does not. Sliding rather
    // than clamping the duration: the clip's length is the cut, and the cut is
    // the thing being kept.
    time::RationalTime start = existing.sourceRange.start();
    const time::RationalTime latest = available - existing.sourceRange.duration();
    if (start > latest) {
        start = latest;
    }
    const time::TimeRange sourceRange{start, existing.sourceRange.duration()};

    return modifyClip(project, target, clipId, "Replace footage", "replace:" + idText(clipId),
                      [media, sourceRange](Clip& clip) {
                          clip.source = media;
                          clip.sourceRange = sourceRange;
                      });
}

Result<CommandPtr> makeSetEffects(Project& project, const EditTarget& target, ClipId clipId,
                                  const std::vector<model::Effect>& effects) {
    for (const model::Effect& effect : effects) {
        // Both the static values and the curves: a keyframe on a parameter the
        // effect does not take is the same silent nothing as a value on one,
        // and it is easier to reach because the panel builds its rows from the
        // table while a copied stack need not have.
        std::vector<model::EffectParam> named;
        for (const auto& [param, value] : effect.values) {
            named.push_back(param);
            if (!std::isfinite(value)) {
                return Error{ErrorCode::InvalidData, "an effect parameter has to be a real number"};
            }
        }
        for (const auto& [param, curve] : effect.animation) {
            named.push_back(param);
            for (const model::Keyframe& key : curve.keyframes()) {
                if (!std::isfinite(key.value)) {
                    return Error{ErrorCode::InvalidData,
                                 "an effect keyframe has to be a real number"};
                }
            }
        }
        for (const model::EffectParam param : named) {
            bool belongs = false;
            for (const model::EffectParamInfo& info : model::parametersOf(effect.kind)) {
                belongs = belongs || info.param == param;
            }
            if (!belongs) {
                // Not pedantry: a value under a parameter the effect does not
                // take is one that will be written to the file, read back, and
                // never used -- a setting somebody made that quietly does
                // nothing.
                return Error{ErrorCode::InvalidData, std::string{model::toString(effect.kind)} +
                                                         " has no " + model::toString(param)};
            }
        }
    }
    return modifyClip(project, target, clipId, effects.empty() ? "Clear effects" : "Set effects",
                      "effects:" + idText(clipId),
                      [effects](Clip& clip) { clip.effects = effects; });
}

Result<CommandPtr> makeSetVignette(Project& project, const EditTarget& target, ClipId clipId,
                                   const model::Vignette& vignette) {
    for (const double value :
         {vignette.amount, vignette.midpoint, vignette.feather, vignette.roundness}) {
        if (!std::isfinite(value)) {
            return Error{ErrorCode::InvalidData, "a vignette has to be real numbers"};
        }
    }
    if (vignette.feather < 0.0 || vignette.midpoint < 0.0) {
        return Error{ErrorCode::InvalidData, "a vignette cannot fall off backwards"};
    }
    return modifyClip(project, target, clipId, "Set vignette", "vignette:" + idText(clipId),
                      [vignette](Clip& clip) { clip.vignette = vignette; });
}

Result<CommandPtr> makeSetWheels(Project& project, const EditTarget& target, ClipId clipId,
                                 const model::ColorWheels& wheels) {
    for (const double value :
         {wheels.slopeR, wheels.slopeG, wheels.slopeB, wheels.offsetR, wheels.offsetG,
          wheels.offsetB, wheels.powerR, wheels.powerG, wheels.powerB}) {
        if (!std::isfinite(value)) {
            return Error{ErrorCode::InvalidData, "a wheel has to be a real number"};
        }
    }
    return modifyClip(project, target, clipId, "Set wheels", "wheels:" + idText(clipId),
                      [wheels](Clip& clip) { clip.wheels = wheels; });
}

Result<CommandPtr> makeSetKeyer(Project& project, const EditTarget& target, ClipId clipId,
                                const model::Keyer& keyer) {
    for (const double value : {keyer.red, keyer.green, keyer.blue, keyer.tolerance, keyer.softness,
                               keyer.lumaLow, keyer.lumaHigh, keyer.lumaSoftness, keyer.spill}) {
        if (!std::isfinite(value)) {
            return Error{ErrorCode::InvalidData, "a key has to be real numbers"};
        }
    }
    if (keyer.lumaHigh < keyer.lumaLow) {
        return Error{ErrorCode::InvalidData, "the luma key's window ends before it starts"};
    }
    return modifyClip(project, target, clipId, keyer.isSet() ? "Set key" : "Clear key",
                      "keyer:" + idText(clipId), [keyer](Clip& clip) { clip.keyer = keyer; });
}

Result<CommandPtr> makeSetSecondary(Project& project, const EditTarget& target, ClipId clipId,
                                    const model::Secondary& secondary) {
    const model::HslQualifier& window = secondary.qualifier;
    for (const double value :
         {window.hueCentre, window.hueWidth, window.hueSoftness, window.saturationLow,
          window.saturationHigh, window.saturationSoftness, window.lumaLow, window.lumaHigh,
          window.lumaSoftness, secondary.correction.temperature, secondary.correction.tint,
          secondary.correction.exposure, secondary.correction.contrast,
          secondary.correction.saturation}) {
        if (!std::isfinite(value)) {
            return Error{ErrorCode::InvalidData, "a secondary has to be real numbers"};
        }
    }
    return modifyClip(project, target, clipId, "Adjust secondary", "secondary:" + idText(clipId),
                      [secondary](Clip& clip) { clip.secondary = secondary; });
}

Result<CommandPtr> makeSetToneCurves(Project& project, const EditTarget& target, ClipId clipId,
                                     const model::ToneCurves& curves) {
    for (const model::ToneCurve* curve :
         {&curves.master, &curves.red, &curves.green, &curves.blue}) {
        for (const model::CurvePoint& point : curve->points()) {
            if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
                return Error{ErrorCode::InvalidData, "a curve point has to be real numbers"};
            }
        }
    }
    // One merge key for all four curves: dragging a point is one gesture even
    // though it rewrites the whole set.
    return modifyClip(project, target, clipId, "Adjust curves", "curves:" + idText(clipId),
                      [curves](Clip& clip) { clip.curves = curves; });
}

Result<CommandPtr> makeSetColorCorrection(Project& project, const EditTarget& target, ClipId clipId,
                                          const model::ColorCorrection& color) {
    for (const double value :
         {color.temperature, color.tint, color.exposure, color.contrast, color.saturation}) {
        if (!std::isfinite(value)) {
            return Error{ErrorCode::InvalidData, "a colour correction has to be real numbers"};
        }
    }
    return modifyClip(project, target, clipId, "Adjust colour", "color:" + idText(clipId),
                      [color](Clip& clip) { clip.color = color; });
}

Result<CommandPtr> makeSetClipAudio(Project& project, const EditTarget& target, ClipId clipId,
                                    double gainDb, double pan) {
    if (!std::isfinite(gainDb) || !std::isfinite(pan)) {
        return Error{ErrorCode::InvalidData, "gain and pan have to be real numbers"};
    }
    const double clampedPan = std::clamp(pan, -1.0, 1.0);
    return modifyClip(project, target, clipId, "Adjust audio", "audio:" + idText(clipId),
                      [gainDb, clampedPan](Clip& clip) {
                          clip.gainDb = gainDb;
                          clip.pan = clampedPan;
                      });
}

Result<CommandPtr> makeSetClipProcessing(Project& project, const EditTarget& target, ClipId clipId,
                                         const model::AudioEq& eq,
                                         const model::Compressor& compressor) {
    for (const double value : {eq.highPassHz, eq.lowPassHz, eq.peakHz, eq.peakGainDb, eq.peakQ,
                               compressor.thresholdDb, compressor.ratio, compressor.attackMs,
                               compressor.releaseMs, compressor.makeupDb}) {
        if (!std::isfinite(value)) {
            return Error{ErrorCode::InvalidData, "processing settings have to be real numbers"};
        }
    }
    // A ratio below 1 is expansion, which this compressor does not do, and a
    // ratio of 0 divides by nothing. Q and the frequencies have the same
    // problem one step further down, in the filter design.
    if (eq.peakQ <= 0.0 || eq.highPassHz < 0.0 || eq.lowPassHz < 0.0 || eq.peakHz <= 0.0) {
        return Error{ErrorCode::InvalidData, "a filter needs a positive frequency and Q"};
    }
    if (compressor.ratio < 1.0 || compressor.attackMs <= 0.0 || compressor.releaseMs <= 0.0) {
        return Error{ErrorCode::InvalidData,
                     "a compressor needs a ratio of at least 1 and times above zero"};
    }
    return modifyClip(project, target, clipId, "Adjust clip processing",
                      "processing:" + idText(clipId), [eq, compressor](Clip& clip) {
                          clip.eq = eq;
                          clip.compressor = compressor;
                      });
}

Result<CommandPtr> makeSetClipEnabled(Project& project, const EditTarget& target, ClipId clipId,
                                      bool enabled) {
    return modifyClip(project, target, clipId, enabled ? "Enable clip" : "Disable clip",
                      // No merge key: this is a toggle, and two of them in a row
                      // are two decisions rather than one gesture.
                      {}, [enabled](Clip& clip) { clip.enabled = enabled; });
}

}  // namespace zaro::edit
