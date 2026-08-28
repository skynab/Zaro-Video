// A clip's own parameters, and the keyframes that animate them.
//
// Nothing here moves a clip or touches a neighbour, so none of it has to
// check for collisions -- see detail::modifyClip, which is the shape they
// nearly all take.

#include <algorithm>
#include <cmath>
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
using detail::locate;
using detail::lookupClip;
using detail::makeCommand;
using detail::modifyClip;

std::string keyframeKey(ClipId clip, model::Param param, const time::RationalTime& at) {
    return "keyframe:" + idText(clip) + ":" + model::toString(param) + ":" +
           std::to_string(at.frames()) + "@" + at.rate().toString();
}

}  // namespace

Result<CommandPtr> makeSetTransform(Project& project, const EditTarget& target, ClipId clipId,
                                    const model::Transform& transform) {
    return modifyClip(project, target, clipId, "Adjust motion", "transform:" + idText(clipId),
                      [transform](Clip& clip) { clip.transform = transform; });
}

Result<CommandPtr> makeSetCurve(Project& project, const EditTarget& target, ClipId clipId,
                                model::Param param, const model::Curve& curve) {
    for (const model::Keyframe& key : curve.keyframes()) {
        if (!std::isfinite(key.value)) {
            return Error{ErrorCode::InvalidData, "a keyframe has to be a real number"};
        }
    }
    return modifyClip(
        project, target, clipId, curve.empty() ? "Clear automation" : "Set automation",
        "curve:" + idText(clipId) + ":" + model::toString(param), [param, curve](Clip& clip) {
            if (curve.empty()) {
                clip.animation.erase(param);
                return;
            }
            clip.animation.curve(param) = curve;
        });
}

Result<CommandPtr> makeTrackMask(Project& project, const EditTarget& target, ClipId clipId,
                                 const model::Curve& x, const model::Curve& y) {
    for (const model::Curve* curve : {&x, &y}) {
        for (const model::Keyframe& key : curve->keyframes()) {
            if (!std::isfinite(key.value)) {
                return Error{ErrorCode::InvalidData, "a keyframe has to be a real number"};
            }
        }
    }
    const bool clearing = x.empty() && y.empty();
    return modifyClip(project, target, clipId, clearing ? "Clear mask track" : "Track mask",
                      "masktrack:" + idText(clipId), [x, y](Clip& clip) {
                          if (x.empty()) {
                              clip.animation.erase(model::Param::MaskX);
                          } else {
                              clip.animation.curve(model::Param::MaskX) = x;
                          }
                          if (y.empty()) {
                              clip.animation.erase(model::Param::MaskY);
                          } else {
                              clip.animation.curve(model::Param::MaskY) = y;
                          }
                      });
}

Result<CommandPtr> makeRemix(Project& project, const EditTarget& target, ClipId clipId,
                             double cutAt, double resumeFrom, double joinFadeSeconds) {
    auto located = locate(project, target);
    if (!located) {
        return located.error();
    }
    const Clip* found = located->track->find(clipId);
    if (found == nullptr) {
        return Error{ErrorCode::NotFound, "no such clip"};
    }
    if (!(resumeFrom > cutAt) || cutAt < 0.0 || joinFadeSeconds < 0.0) {
        return Error{ErrorCode::InvalidData, "that is not a piece to take out"};
    }

    const Clip& original = *found;
    const time::Rational rate = original.sourceRange.start().rate();
    const auto seconds = [rate](double value) {
        return time::RationalTime{static_cast<std::int64_t>(std::llround(value * rate.toDouble())),
                                  rate};
    };
    const time::RationalTime sourceStart = original.sourceRange.start();
    const time::RationalTime cut = sourceStart + seconds(cutAt);
    const time::RationalTime resume = sourceStart + seconds(resumeFrom);
    const time::RationalTime sourceEnd = original.sourceRange.endExclusive();
    if (cut <= sourceStart || resume >= sourceEnd) {
        return Error{ErrorCode::InvalidData, "that cut falls outside the clip"};
    }

    const ClipId tailId = project.ids().next<model::ClipTag>();
    const TrackId trackId = target.track;
    const auto fade = seconds(joinFadeSeconds);

    return makeCommand(
        target.sequence, "Fit music to length", {},
        [clipId, tailId, trackId, cut, resume, sourceEnd, fade](Sequence& sequence) {
            Track* track = sequence.findTrack(trackId);
            ZARO_CHECK(track != nullptr, "track vanished between build and apply");
            const Clip* existing = track->find(clipId);
            ZARO_CHECK(existing != nullptr, "clip vanished between build and apply");
            const Clip before = *existing;

            Clip head = before;
            head.sourceRange =
                time::TimeRange{before.sourceRange.start(), cut - before.sourceRange.start()};
            head.timelineRange = time::TimeRange{before.start(), head.sourceRange.duration()};

            Clip tail = before;
            tail.id = tailId;
            tail.sourceRange = time::TimeRange{resume, sourceEnd - resume};
            // Butted, not overlapped: a track holds that its clips are in
            // order and do not overlap, and two halves that overlapped would
            // trip that the moment they were placed.
            tail.timelineRange =
                time::TimeRange{head.timelineRange.endExclusive(), tail.sourceRange.duration()};

            // Written as gain rather than as a transition: a dip at a join is
            // two level ramps, and the level curves already exist, are already
            // keyframed in source time, and already survive a trim.
            if (fade.toSecondsDouble() > 0.0) {
                model::Curve down;
                down.set(
                    model::Keyframe{cut - fade, head.gainDb, model::Interpolation::Linear, {}, {}});
                down.set(model::Keyframe{cut, -60.0, model::Interpolation::Linear, {}, {}});
                head.animation.curve(model::Param::GainDb) = down;

                model::Curve up;
                up.set(model::Keyframe{resume, -60.0, model::Interpolation::Linear, {}, {}});
                up.set(model::Keyframe{
                    resume + fade, tail.gainDb, model::Interpolation::Linear, {}, {}});
                tail.animation.curve(model::Param::GainDb) = up;
            }

            std::vector<Clip> rebuilt;
            for (const Clip& other : track->clips()) {
                if (other.id != clipId) {
                    rebuilt.push_back(other);
                }
            }
            rebuilt.push_back(std::move(head));
            rebuilt.push_back(std::move(tail));
            track->setClips(std::move(rebuilt));
        });
}

Result<CommandPtr> makeReframe(Project& project, const EditTarget& target, ClipId clipId,
                               const model::Curve& x, const model::Curve& y, double scale) {
    auto found = lookupClip(project, target, clipId);
    if (!found) {
        return found.error();
    }
    const Clip& existing = **found;
    for (const model::Param param : {model::Param::PositionX, model::Param::PositionY,
                                     model::Param::ScaleX, model::Param::ScaleY}) {
        const model::Curve* curve = existing.animation.find(param);
        if (curve != nullptr && !curve->empty()) {
            return Error{ErrorCode::InvalidData,
                         "this clip's position or scale is already animated -- clear it first"};
        }
    }
    if (!std::isfinite(scale) || scale <= 0.0) {
        return Error{ErrorCode::InvalidData, "a reframe scale has to be above zero"};
    }
    for (const model::Curve* curve : {&x, &y}) {
        for (const model::Keyframe& key : curve->keyframes()) {
            if (!std::isfinite(key.value)) {
                return Error{ErrorCode::InvalidData, "a keyframe has to be a real number"};
            }
        }
    }

    const bool clearing = x.empty() && y.empty();
    return modifyClip(project, target, clipId, clearing ? "Clear reframe" : "Auto-reframe",
                      "reframe:" + idText(clipId), [x, y, scale, clearing](Clip& clip) {
                          if (clearing) {
                              clip.animation.erase(model::Param::PositionX);
                              clip.animation.erase(model::Param::PositionY);
                              clip.transform.scaleX = 1.0;
                              clip.transform.scaleY = 1.0;
                              return;
                          }
                          clip.animation.curve(model::Param::PositionX) = x;
                          clip.animation.curve(model::Param::PositionY) = y;
                          // The scale is static: a frame that grew and shrank
                          // while it moved would read as a zoom nobody asked
                          // for.
                          clip.transform.scaleX = scale;
                          clip.transform.scaleY = scale;
                      });
}

Result<CommandPtr> makeStabilise(Project& project, const EditTarget& target, ClipId clipId,
                                 const model::Curve& x, const model::Curve& y, double zoom) {
    for (const model::Curve* curve : {&x, &y}) {
        for (const model::Keyframe& key : curve->keyframes()) {
            if (!std::isfinite(key.value)) {
                return Error{ErrorCode::InvalidData, "a keyframe has to be a real number"};
            }
        }
    }
    if (!std::isfinite(zoom) || zoom <= 0.0) {
        return Error{ErrorCode::InvalidData, "a stabilise zoom has to be above zero"};
    }
    const bool clearing = x.empty() && y.empty();
    return modifyClip(
        project, target, clipId, clearing ? "Clear stabilisation" : "Stabilise",
        "stabilise:" + idText(clipId), [x, y, zoom, clearing](Clip& clip) {
            if (clearing) {
                clip.animation.erase(model::Param::StabiliseX);
                clip.animation.erase(model::Param::StabiliseY);
                clip.animation.erase(model::Param::StabiliseZoom);
                return;
            }
            clip.animation.curve(model::Param::StabiliseX) = x;
            clip.animation.curve(model::Param::StabiliseY) = y;
            model::Curve held;
            held.set(model::Keyframe{
                x.keyframes().front().time, zoom, model::Interpolation::Hold, {}, {}});
            clip.animation.curve(model::Param::StabiliseZoom) = held;
        });
}

Result<CommandPtr> makeSetResponsive(Project& project, const EditTarget& target, ClipId clipId,
                                     const time::RationalTime& intro,
                                     const time::RationalTime& outro) {
    if (intro.toSecondsDouble() < 0.0 || outro.toSecondsDouble() < 0.0) {
        return Error{ErrorCode::InvalidData, "an intro or outro cannot be negative"};
    }
    return modifyClip(project, target, clipId, "Set responsive timing",
                      "responsive:" + idText(clipId), [intro, outro](Clip& clip) {
                          clip.responsive.intro = intro;
                          clip.responsive.outro = outro;
                          // Recorded now: the length the keyframes are being
                          // protected against is the length they were drawn at,
                          // which is whatever the clip is at this moment.
                          clip.responsive.authored = clip.sourceRange.duration();
                      });
}

Result<CommandPtr> makeSetAudioRole(Project& project, const EditTarget& target, ClipId clipId,
                                    model::AudioRole role) {
    return modifyClip(project, target, clipId, "Set role", "role:" + idText(clipId),
                      [role](Clip& clip) { clip.role = role; });
}

Result<CommandPtr> makeSetKeyframe(Project& project, const EditTarget& target, ClipId clipId,
                                   model::Param param, const time::RationalTime& sourceTime,
                                   double value, model::Interpolation interpolation) {
    if (!std::isfinite(value)) {
        return Error{ErrorCode::InvalidData, "a keyframe value has to be a real number"};
    }
    return modifyClip(
        project, target, clipId, std::string{"Set "} + model::toString(param) + " keyframe",
        keyframeKey(clipId, param, sourceTime),
        [param, sourceTime, value, interpolation](Clip& clip) {
            model::Keyframe key;
            key.time = sourceTime;
            key.value = value;
            key.interpolation = interpolation;
            // Replacing an existing keyframe keeps its handles:
            // dragging a value should not silently flatten a
            // curve the user shaped.
            if (const model::Keyframe* existing = clip.animation.curve(param).at(sourceTime)) {
                key.interpolation = existing->interpolation;
                key.out = existing->out;
                key.in = existing->in;
            }
            clip.animation.curve(param).set(key);
        });
}

Result<CommandPtr> makeRemoveKeyframe(Project& project, const EditTarget& target, ClipId clipId,
                                      model::Param param, const time::RationalTime& sourceTime) {
    auto found = lookupClip(project, target, clipId);
    if (!found) {
        return found.error();
    }
    const Clip* existing = *found;
    const model::Curve* curve = existing->animation.find(param);
    if (curve == nullptr || curve->at(sourceTime) == nullptr) {
        return Error{ErrorCode::NotFound, "there is no keyframe there"};
    }
    return modifyClip(project, target, clipId,
                      std::string{"Delete "} + model::toString(param) + " keyframe",
                      // No merge key: deleting two keyframes is two decisions.
                      {}, [param, sourceTime](Clip& clip) {
                          clip.animation.curve(param).removeAt(sourceTime);
                          // A parameter with no keyframes left is not animated,
                          // and an empty curve left behind would say it still
                          // is. The static value takes over, which is the value
                          // the last keyframe was holding everywhere.
                          clip.animation.pruneEmpty();
                      });
}

Result<CommandPtr> makeMoveKeyframe(Project& project, const EditTarget& target, ClipId clipId,
                                    model::Param param, const time::RationalTime& from,
                                    const time::RationalTime& to) {
    auto found = lookupClip(project, target, clipId);
    if (!found) {
        return found.error();
    }
    const Clip* existing = *found;
    const model::Curve* curve = existing->animation.find(param);
    if (curve == nullptr || curve->at(from) == nullptr) {
        return Error{ErrorCode::NotFound, "there is no keyframe there"};
    }
    // Landing on another keyframe would silently destroy it. Refusing leaves
    // the dragged keyframe where it was, which is recoverable; overwriting is
    // not obviously undoable to someone who did not see what was underneath.
    if (from != to && curve->at(to) != nullptr) {
        return Error{ErrorCode::InvalidData, "another keyframe is already there"};
    }
    return modifyClip(project, target, clipId,
                      std::string{"Move "} + model::toString(param) + " keyframe",
                      keyframeKey(clipId, param, from), [param, from, to](Clip& clip) {
                          model::Curve& animated = clip.animation.curve(param);
                          const model::Keyframe* keyframe = animated.at(from);
                          if (keyframe == nullptr) {
                              return;
                          }
                          model::Keyframe moved = *keyframe;
                          moved.time = to;
                          animated.removeAt(from);
                          animated.set(moved);
                      });
}

Result<CommandPtr> makeSetKeyframeInterpolation(Project& project, const EditTarget& target,
                                                ClipId clipId, model::Param param,
                                                const time::RationalTime& sourceTime,
                                                model::Interpolation interpolation) {
    auto found = lookupClip(project, target, clipId);
    if (!found) {
        return found.error();
    }
    const Clip* existing = *found;
    const model::Curve* curve = existing->animation.find(param);
    if (curve == nullptr || curve->at(sourceTime) == nullptr) {
        return Error{ErrorCode::NotFound, "there is no keyframe there"};
    }
    return modifyClip(project, target, clipId,
                      std::string{"Set keyframe to "} + model::toString(interpolation), {},
                      [param, sourceTime, interpolation](Clip& clip) {
                          model::Curve& animated = clip.animation.curve(param);
                          const model::Keyframe* keyframe = animated.at(sourceTime);
                          if (keyframe == nullptr) {
                              return;
                          }
                          model::Keyframe changed = *keyframe;
                          changed.interpolation = interpolation;
                          animated.set(changed);
                      });
}

Result<CommandPtr> makeMoveKeyframesAt(Project& project, const EditTarget& target, ClipId clipId,
                                       const RationalTime& from, const RationalTime& to) {
    auto found = lookupClip(project, target, clipId);
    if (!found) {
        return found.error();
    }
    const Clip* existing = *found;

    bool any = false;
    for (const auto& [param, curve] : existing->animation) {
        if (curve.at(from) == nullptr) {
            continue;
        }
        any = true;
        // Refused wholesale rather than per parameter: moving some of a set of
        // keyframes and silently leaving the rest is worse than moving none.
        if (from != to && curve.at(to) != nullptr) {
            return Error{ErrorCode::InvalidData, "another keyframe is already there"};
        }
    }
    if (!any) {
        return Error{ErrorCode::NotFound, "there are no keyframes there"};
    }

    return modifyClip(project, target, clipId, "Move keyframes",
                      "keyframes:" + idText(clipId) + ":" + std::to_string(from.frames()),
                      [from, to](Clip& clip) {
                          for (model::Param param : model::allParams()) {
                              model::Curve* curve = clip.animation.find(param) != nullptr
                                                        ? &clip.animation.curve(param)
                                                        : nullptr;
                              if (curve == nullptr) {
                                  continue;
                              }
                              const model::Keyframe* at = curve->at(from);
                              if (at == nullptr) {
                                  continue;
                              }
                              model::Keyframe moved = *at;
                              moved.time = to;
                              curve->removeAt(from);
                              curve->set(moved);
                          }
                      });
}

Result<CommandPtr> makeRemoveKeyframesAt(Project& project, const EditTarget& target, ClipId clipId,
                                         const RationalTime& sourceTime) {
    auto found = lookupClip(project, target, clipId);
    if (!found) {
        return found.error();
    }
    bool any = false;
    for (const auto& [param, curve] : (*found)->animation) {
        any = any || curve.at(sourceTime) != nullptr;
    }
    if (!any) {
        return Error{ErrorCode::NotFound, "there are no keyframes there"};
    }

    return modifyClip(project, target, clipId, "Delete keyframes", {}, [sourceTime](Clip& clip) {
        for (model::Param param : model::allParams()) {
            if (clip.animation.find(param) != nullptr) {
                clip.animation.curve(param).removeAt(sourceTime);
            }
        }
        clip.animation.pruneEmpty();
    });
}

Result<CommandPtr> makeSetParameterAnimated(Project& project, const EditTarget& target,
                                            ClipId clipId, model::Param param, bool animated,
                                            const time::RationalTime& timelineTime) {
    auto found = lookupClip(project, target, clipId);
    if (!found) {
        return found.error();
    }
    const Clip* existing = *found;
    const model::Curve* curve = existing->animation.find(param);
    const bool alreadyAnimated = curve != nullptr && !curve->empty();
    if (alreadyAnimated == animated) {
        return Error{ErrorCode::InvalidData, "that parameter is already in that state"};
    }

    if (animated) {
        // The value it already had, at the moment the stopwatch was pressed, so
        // switching animation on never moves anything.
        const double held = existing->parameterValue(param);
        const time::RationalTime sourceTime = existing->sourceTimeAt(timelineTime);
        return modifyClip(project, target, clipId, std::string{"Animate "} + model::toString(param),
                          {}, [param, sourceTime, held](Clip& clip) {
                              model::Keyframe key;
                              key.time = sourceTime;
                              key.value = held;
                              clip.animation.curve(param).set(key);
                          });
    }

    // Keep what is on screen now. Reverting to the static value underneath
    // would make the picture jump at the instant animation was switched off,
    // and that value is usually the default rather than anything the user
    // chose.
    const double showing = existing->parameterAt(param, timelineTime);
    return modifyClip(project, target, clipId,
                      std::string{"Stop animating "} + model::toString(param), {},
                      [param, showing](Clip& clip) {
                          clip.animation.erase(param);
                          clip.setParameterValue(param, showing);
                      });
}

}  // namespace zaro::edit
