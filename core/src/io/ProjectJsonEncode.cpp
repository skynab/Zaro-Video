// Writing a project out: model types to JSON.
//
// Nothing here reads. See ProjectJson.h for why the two directions are
// separate files and why the declarations are not.

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "zaro/core/model/ClipEffects.h"
#include "zaro/core/model/Transition.h"
#include "zaro/core/time/Timecode.h"

#include "ProjectJson.h"

namespace zaro::io::detail {

// --- Time encoding ----------------------------------------------------------
// Rationals are written as "30000/1001" rather than a decimal, because the
// whole point of the type is that 29.97 is not a decimal. A project file that
// says 29.97 has already lost the information.

json encode(const time::Rational& value) {
    return value.toString();
}

json encode(const time::RationalTime& value) {
    return json{{"frames", value.frames()}, {"rate", encode(value.rate())}};
}

json encode(const time::TimeRange& value) {
    return json{{"start", encode(value.start())}, {"duration", encode(value.duration())}};
}

// --- Model encoding ---------------------------------------------------------

json encode(const model::Transform& transform) {
    // Only what differs from the default is written. A timeline of a thousand
    // untouched clips should not carry a thousand identity matrices, and
    // omitting defaults means adding a parameter later does not rewrite every
    // existing file.
    const model::Transform identity;
    json out = json::object();
    const auto put = [&out](const char* key, double value, double fallback) {
        if (value != fallback) {
            out[key] = value;
        }
    };
    put("positionX", transform.positionX, identity.positionX);
    put("positionY", transform.positionY, identity.positionY);
    put("scaleX", transform.scaleX, identity.scaleX);
    put("scaleY", transform.scaleY, identity.scaleY);
    put("rotationDegrees", transform.rotationDegrees, identity.rotationDegrees);
    put("anchorX", transform.anchorX, identity.anchorX);
    put("anchorY", transform.anchorY, identity.anchorY);
    put("opacity", transform.opacity, identity.opacity);
    return out;
}

json encode(const model::ColorCorrection& color) {
    // Only what differs from neutral, the same as the transform: a project of a
    // thousand ungraded clips should not carry a thousand copies of "no
    // correction".
    const model::ColorCorrection neutral;
    json out = json::object();
    const auto put = [&out](const char* key, double value, double fallback) {
        if (value != fallback) {
            out[key] = value;
        }
    };
    put("temperature", color.temperature, neutral.temperature);
    put("tint", color.tint, neutral.tint);
    put("exposure", color.exposure, neutral.exposure);
    put("contrast", color.contrast, neutral.contrast);
    put("saturation", color.saturation, neutral.saturation);
    return out;
}

/// One curve, as an array of keyframes.
///
/// Shared by the clip's own animation and by the curves an effect carries, so
/// that the two cannot come to disagree about how a bezier handle is written.
json encodeCurve(const model::Curve& curve) {
    json keys = json::array();
    for (const model::Keyframe& key : curve.keyframes()) {
        const model::Keyframe defaults;
        json encoded{{"time", encode(key.time)}, {"value", key.value}};
        if (key.interpolation != defaults.interpolation) {
            encoded["interpolation"] = model::toString(key.interpolation);
        }
        // Handles are only meaningful for beziers, and writing the default ease
        // onto every linear keyframe would triple the size of a long automation
        // curve for no information.
        if (key.out != defaults.out) {
            encoded["out"] = json{{"dx", key.out.dx}, {"dy", key.out.dy}};
        }
        if (key.in != defaults.in) {
            encoded["in"] = json{{"dx", key.in.dx}, {"dy", key.in.dy}};
        }
        keys.push_back(std::move(encoded));
    }
    return keys;
}

json encode(const std::vector<model::Effect>& effects) {
    json out = json::array();
    for (const model::Effect& effect : effects) {
        json entry{{"kind", model::toString(effect.kind)}};
        if (!effect.enabled) {
            entry["enabled"] = false;
        }
        json values = json::object();
        for (const auto& [param, value] : effect.values) {
            values[model::toString(param)] = value;
        }
        if (!values.empty()) {
            entry["values"] = std::move(values);
        }
        json curves = json::object();
        for (const auto& [param, curve] : effect.animation) {
            if (!curve.empty()) {
                curves[model::toString(param)] = encodeCurve(curve);
            }
        }
        if (!curves.empty()) {
            entry["curves"] = std::move(curves);
        }
        out.push_back(std::move(entry));
    }
    return out;
}

json encode(const model::Vignette& vignette) {
    const model::Vignette neutral;
    if (vignette == neutral) {
        return json::object();
    }
    json out = json::object();
    const auto put = [&out](const char* key, double value, double fallback) {
        if (value != fallback) {
            out[key] = value;
        }
    };
    put("amount", vignette.amount, neutral.amount);
    put("midpoint", vignette.midpoint, neutral.midpoint);
    put("feather", vignette.feather, neutral.feather);
    put("roundness", vignette.roundness, neutral.roundness);
    return out;
}

json encode(const model::ColorWheels& wheels) {
    const model::ColorWheels neutral;
    if (wheels == neutral) {
        return json::object();
    }
    json out = json::object();
    const auto put = [&out](const char* key, double value, double fallback) {
        if (value != fallback) {
            out[key] = value;
        }
    };
    put("slopeR", wheels.slopeR, neutral.slopeR);
    put("slopeG", wheels.slopeG, neutral.slopeG);
    put("slopeB", wheels.slopeB, neutral.slopeB);
    put("offsetR", wheels.offsetR, neutral.offsetR);
    put("offsetG", wheels.offsetG, neutral.offsetG);
    put("offsetB", wheels.offsetB, neutral.offsetB);
    put("powerR", wheels.powerR, neutral.powerR);
    put("powerG", wheels.powerG, neutral.powerG);
    put("powerB", wheels.powerB, neutral.powerB);
    return out;
}

json encode(const model::Keyer& keyer) {
    const model::Keyer neutral;
    if (keyer == neutral) {
        return json::object();
    }
    json out = json::object();
    const auto put = [&out](const char* key, double value, double fallback) {
        if (value != fallback) {
            out[key] = value;
        }
    };
    switch (keyer.kind) {
        case model::KeyKind::None:
            break;
        case model::KeyKind::Chroma:
            out["kind"] = "chroma";
            break;
        case model::KeyKind::Luma:
            out["kind"] = "luma";
            break;
    }
    put("red", keyer.red, neutral.red);
    put("green", keyer.green, neutral.green);
    put("blue", keyer.blue, neutral.blue);
    put("tolerance", keyer.tolerance, neutral.tolerance);
    put("softness", keyer.softness, neutral.softness);
    put("lumaLow", keyer.lumaLow, neutral.lumaLow);
    put("lumaHigh", keyer.lumaHigh, neutral.lumaHigh);
    put("lumaSoftness", keyer.lumaSoftness, neutral.lumaSoftness);
    put("spill", keyer.spill, neutral.spill);
    // Deliberately not saved, for the same reason the qualifier's mask view is
    // not: looking at the matte is how somebody is working right now, and
    // reopening a project into a black-and-white silhouette would be baffling.
    return out;
}

json encode(const model::Secondary& secondary) {
    const model::Secondary neutral;
    if (secondary == neutral) {
        return json::object();
    }
    const model::HslQualifier defaults;
    json window = json::object();
    const auto put = [&window](const char* key, double value, double fallback) {
        if (value != fallback) {
            window[key] = value;
        }
    };
    put("hueCentre", secondary.qualifier.hueCentre, defaults.hueCentre);
    put("hueWidth", secondary.qualifier.hueWidth, defaults.hueWidth);
    put("hueSoftness", secondary.qualifier.hueSoftness, defaults.hueSoftness);
    put("saturationLow", secondary.qualifier.saturationLow, defaults.saturationLow);
    put("saturationHigh", secondary.qualifier.saturationHigh, defaults.saturationHigh);
    put("saturationSoftness", secondary.qualifier.saturationSoftness, defaults.saturationSoftness);
    put("lumaLow", secondary.qualifier.lumaLow, defaults.lumaLow);
    put("lumaHigh", secondary.qualifier.lumaHigh, defaults.lumaHigh);
    put("lumaSoftness", secondary.qualifier.lumaSoftness, defaults.lumaSoftness);
    if (secondary.qualifier.enabled) {
        window["enabled"] = true;
    }

    json out = json::object();
    if (!window.empty()) {
        out["qualifier"] = std::move(window);
    }
    if (json correction = encode(secondary.correction); !correction.empty()) {
        out["correction"] = std::move(correction);
    }
    // Deliberately not saved: a mask view is how someone is looking at the
    // picture right now, not something about the cut. Reopening a project into
    // a grey silhouette would be baffling.
    return out;
}

json encode(const model::ToneCurves& curves) {
    json out = json::object();
    const auto put = [&out](const char* name, const model::ToneCurve& curve) {
        if (curve.isIdentity()) {
            return;
        }
        json points = json::array();
        for (const model::CurvePoint& point : curve.points()) {
            points.push_back(json{{"x", point.x}, {"y", point.y}});
        }
        out[name] = std::move(points);
    };
    put("master", curves.master);
    put("red", curves.red);
    put("green", curves.green);
    put("blue", curves.blue);
    return out;
}

json encode(const model::ClipAnimation& animation) {
    // Curves keyed by parameter name rather than an array of {param, curve}
    // pairs: a parameter can only be animated once, and a map says so in the
    // format instead of leaving a reader to enforce it.
    json out = json::object();
    for (const auto& [param, curve] : animation) {
        if (curve.empty()) {
            continue;
        }
        out[model::toString(param)] = encodeCurve(curve);
    }
    return out;
}

json encode(const model::Clip& clip) {
    json out{{"id", clip.id.value()},
             {"source", clip.source.value()},
             {"name", clip.name},
             {"enabled", clip.enabled},
             {"sourceRange", encode(clip.sourceRange)},
             {"timelineRange", encode(clip.timelineRange)}};

    if (json transform = encode(clip.transform); !transform.empty()) {
        out["transform"] = std::move(transform);
    }
    if (clip.blend != model::BlendMode::Normal) {
        out["blend"] = model::toString(clip.blend);
    }
    if (clip.gainDb != 0.0) {
        out["gainDb"] = clip.gainDb;
    }
    if (clip.pan != 0.0) {
        out["pan"] = clip.pan;
    }
    if (json color = encode(clip.color); !color.empty()) {
        out["color"] = std::move(color);
    }
    if (clip.adjustment) {
        out["adjustment"] = true;
    }
    if (clip.isMulticam()) {
        json angles = json::array();
        for (const model::Clip::Angle& angle : clip.angles) {
            json entry{{"media", angle.media.value()}, {"offset", encode(angle.offset)}};
            if (!angle.name.empty()) {
                entry["name"] = angle.name;
            }
            angles.push_back(std::move(entry));
        }
        out["angles"] = std::move(angles);
        if (clip.activeAngle != 0) {
            out["activeAngle"] = clip.activeAngle;
        }
    }
    if (clip.nested.isValid()) {
        out["nested"] = clip.nested.value();
    }
    if (clip.reversed) {
        out["reversed"] = true;
    }
    if (clip.mask.isSet()) {
        const model::Mask defaults;
        json mask{{"shape", model::toString(clip.mask.shape)}};
        const auto put = [&mask](const char* key, double value, double fallback) {
            if (value != fallback) {
                mask[key] = value;
            }
        };
        put("width", clip.mask.width, defaults.width);
        put("height", clip.mask.height, defaults.height);
        put("centreX", clip.mask.centreX, defaults.centreX);
        put("centreY", clip.mask.centreY, defaults.centreY);
        put("cornerRadius", clip.mask.cornerRadius, defaults.cornerRadius);
        put("feather", clip.mask.feather, defaults.feather);
        if (clip.mask.inverted) {
            mask["inverted"] = true;
        }
        if (clip.mask.shape == model::MaskShape::Path && clip.mask.path.isSet()) {
            json points = json::array();
            for (const model::MaskPoint& point : clip.mask.path.points) {
                // Handles written only when they are not zero, so a polygon of
                // corners -- which is most masks -- reads as a list of pairs
                // rather than a wall of zeroes.
                json encoded{{"x", point.x}, {"y", point.y}};
                if (point.inX != 0.0 || point.inY != 0.0) {
                    encoded["in"] = json{{"x", point.inX}, {"y", point.inY}};
                }
                if (point.outX != 0.0 || point.outY != 0.0) {
                    encoded["out"] = json{{"x", point.outX}, {"y", point.outY}};
                }
                points.push_back(std::move(encoded));
            }
            mask["path"] = std::move(points);
        }
        out["mask"] = std::move(mask);
    }
    if (clip.pinnedTo.isValid()) {
        out["pinnedTo"] = clip.pinnedTo.value();
    }
    if (clip.responsive.isSet()) {
        out["responsive"] = json{{"intro", encode(clip.responsive.intro)},
                                 {"outro", encode(clip.responsive.outro)},
                                 {"authored", encode(clip.responsive.authored)}};
    }
    if (clip.graphic.isSet()) {
        const model::Graphic defaults;
        json graphic{{"kind", model::toString(clip.graphic.kind)}};
        const auto put = [&graphic](const char* key, double value, double fallback) {
            if (value != fallback) {
                graphic[key] = value;
            }
        };
        put("width", clip.graphic.width, defaults.width);
        put("height", clip.graphic.height, defaults.height);
        put("centreX", clip.graphic.centreX, defaults.centreX);
        put("centreY", clip.graphic.centreY, defaults.centreY);
        put("cornerRadius", clip.graphic.cornerRadius, defaults.cornerRadius);
        put("feather", clip.graphic.feather, defaults.feather);
        put("red", clip.graphic.red, defaults.red);
        put("green", clip.graphic.green, defaults.green);
        put("blue", clip.graphic.blue, defaults.blue);
        put("alpha", clip.graphic.alpha, defaults.alpha);
        if (!clip.graphic.text.empty()) {
            graphic["text"] = clip.graphic.text;
        }
        if (!clip.graphic.family.empty()) {
            graphic["family"] = clip.graphic.family;
        }
        put("pointSize", clip.graphic.pointSize, defaults.pointSize);
        if (clip.graphic.bold) {
            graphic["bold"] = true;
        }
        if (clip.graphic.italic) {
            graphic["italic"] = true;
        }
        if (clip.graphic.alignment != defaults.alignment) {
            graphic["alignment"] = clip.graphic.alignment;
        }
        out["graphic"] = std::move(graphic);
    }
    if (clip.lut.isSet() || !clip.lut.path.empty()) {
        json lut{{"path", clip.lut.path}};
        if (clip.lut.amount != 1.0) {
            lut["amount"] = clip.lut.amount;
        }
        out["lut"] = std::move(lut);
    }
    if (json secondary = encode(clip.secondary); !secondary.empty()) {
        out["secondary"] = std::move(secondary);
    }
    if (clip.role != model::AudioRole::Unassigned) {
        out["role"] = model::toString(clip.role);
    }
    if (json vignette = encode(clip.vignette); !vignette.empty()) {
        out["vignette"] = std::move(vignette);
    }
    if (json wheels = encode(clip.wheels); !wheels.empty()) {
        out["wheels"] = std::move(wheels);
    }
    if (json keyer = encode(clip.keyer); !keyer.empty()) {
        out["keyer"] = std::move(keyer);
    }
    if (json effects = encode(clip.effects); !effects.empty()) {
        out["effects"] = std::move(effects);
    }
    if (json curves = encode(clip.curves); !curves.empty()) {
        out["curves"] = std::move(curves);
    }
    if (json animation = encode(clip.animation); !animation.empty()) {
        out["animation"] = std::move(animation);
    }
    if (clip.link.isValid()) {
        out["link"] = clip.link.value();
    }
    return out;
}

json encode(const model::Transition& transition) {
    json out{{"id", transition.id.value()},
             {"from", transition.from.value()},
             {"to", transition.to.value()},
             {"kind", model::toString(transition.kind)},
             {"range", encode(transition.range)}};
    if (transition.kind != model::TransitionKind::CrossDissolve) {
        // A dissolve has no direction to travel in, so writing one would be a
        // value that means nothing and reads as though it might.
        out["direction"] = model::toString(transition.direction);
    }
    return out;
}

json encode(const model::Track& track) {
    json clips = json::array();
    for (const model::Clip& clip : track.clips()) {
        clips.push_back(encode(clip));
    }
    json transitions = json::array();
    for (const model::Transition& transition : track.transitions()) {
        transitions.push_back(encode(transition));
    }

    json out{{"id", track.id().value()},   {"kind", model::toString(track.kind())},
             {"name", track.name()},       {"muted", track.isMuted()},
             {"locked", track.isLocked()}, {"clips", std::move(clips)}};
    if (!transitions.empty()) {
        out["transitions"] = std::move(transitions);
    }
    if (track.eq().enabled) {
        const model::AudioEq& eq = track.eq();
        out["eq"] =
            json{{"enabled", true},     {"highPassHz", eq.highPassHz}, {"lowPassHz", eq.lowPassHz},
                 {"peakHz", eq.peakHz}, {"peakGainDb", eq.peakGainDb}, {"peakQ", eq.peakQ}};
    }
    if (track.compressor().enabled) {
        const model::Compressor& compressor = track.compressor();
        out["compressor"] = json{{"enabled", true},
                                 {"thresholdDb", compressor.thresholdDb},
                                 {"ratio", compressor.ratio},
                                 {"attackMs", compressor.attackMs},
                                 {"releaseMs", compressor.releaseMs},
                                 {"makeupDb", compressor.makeupDb}};
    }
    if (track.isSoloed()) {
        // Only when set: solo is a transient state on most projects, and a
        // "soloed": false on every track is noise in a file people read.
        out["soloed"] = true;
    }
    if (track.gainDb() != 0.0) {
        out["gainDb"] = track.gainDb();
    }
    if (track.pan() != 0.0) {
        out["pan"] = track.pan();
    }
    if (!track.isSyncLocked()) {
        // Only written when off, since on is the default and the common case.
        out["syncLocked"] = false;
    }
    return out;
}

json encode(const model::Marker& marker) {
    json out{{"id", marker.id.value()}, {"range", encode(marker.range)}, {"name", marker.name}};
    if (!marker.note.empty()) {
        out["note"] = marker.note;
    }
    if (marker.colour != 0) {
        out["colour"] = marker.colour;
    }
    if (!marker.author.empty()) {
        out["author"] = marker.author;
    }
    if (marker.resolved) {
        out["resolved"] = true;
    }
    return out;
}

json encodeCaptions(const model::CaptionTrack& track) {
    json captions = json::array();
    for (const model::Caption& caption : track.captions()) {
        captions.push_back(json{{"range", encode(caption.range)}, {"text", caption.text}});
    }
    const model::CaptionStyle& style = track.style();
    const model::CaptionStyle defaultStyle;
    json captionStyle = json::object();
    const auto putStyle = [&captionStyle](const char* key, double value, double fallback) {
        if (value != fallback) {
            captionStyle[key] = value;
        }
    };
    if (!style.family.empty()) {
        captionStyle["family"] = style.family;
    }
    putStyle("pointSize", style.pointSize, defaultStyle.pointSize);
    putStyle("bottomMargin", style.bottomMargin, defaultStyle.bottomMargin);
    putStyle("widthFraction", style.widthFraction, defaultStyle.widthFraction);
    putStyle("red", style.red, defaultStyle.red);
    putStyle("green", style.green, defaultStyle.green);
    putStyle("blue", style.blue, defaultStyle.blue);
    putStyle("alpha", style.alpha, defaultStyle.alpha);
    if (style.bold) {
        captionStyle["bold"] = true;
    }

    json captionTrack = json::object();
    if (!captions.empty()) {
        captionTrack["cues"] = std::move(captions);
    }
    if (!captionStyle.empty()) {
        captionTrack["style"] = std::move(captionStyle);
    }
    if (track.isBurnedIn()) {
        captionTrack["burnIn"] = true;
    }

    return captionTrack;
}

json encode(const model::Sequence& sequence) {
    json videoTracks = json::array();
    for (const model::Track& track : sequence.videoTracks()) {
        videoTracks.push_back(encode(track));
    }
    json audioTracks = json::array();
    for (const model::Track& track : sequence.audioTracks()) {
        audioTracks.push_back(encode(track));
    }
    json markers = json::array();
    for (const model::Marker& marker : sequence.markers()) {
        markers.push_back(encode(marker));
    }
    json captionTrack = encodeCaptions(sequence.captions());
    const model::Sequence::Output defaultOutput;
    json output = json::object();
    if (sequence.output().transfer != defaultOutput.transfer) {
        output["transfer"] = media::toString(sequence.output().transfer);
    }
    if (sequence.output().highlightKnee != defaultOutput.highlightKnee) {
        output["highlightKnee"] = sequence.output().highlightKnee;
    }

    json out{{"id", sequence.id().value()},
             {"name", sequence.name()},
             {"frameRate", encode(sequence.frameRate())},
             {"audioSampleRate", encode(sequence.audioSampleRate())},
             {"width", sequence.width()},
             {"height", sequence.height()},
             {"startTime", encode(sequence.startTime())},
             {"videoTracks", std::move(videoTracks)},
             {"audioTracks", std::move(audioTracks)},
             {"markers", std::move(markers)}};
    if (!captionTrack.empty()) {
        out["captions"] = std::move(captionTrack);
    }
    if (!output.empty()) {
        // Only when it differs from the default: a Rec.709 sequence that clips
        // its highlights is what every project was before there was a choice,
        // and should not start carrying a line asserting it.
        out["output"] = std::move(output);
    }
    return out;
}

json encode(const model::Subclip& subclip) {
    return json{{"id", subclip.id.value()},
                {"source", subclip.source.value()},
                {"range", encode(subclip.range)},
                {"name", subclip.name}};
}

json encode(const model::MediaRef& ref) {
    // MediaInfo is a probe cache, not project data, so only the parts the model
    // actually reasons about are written: duration bounds trims, and the size
    // and rate let a bin show something before any file has been reopened.
    json cached{{"duration", encode(ref.info.duration)}};
    if (const media::VideoStreamInfo* video = ref.info.primaryVideo()) {
        cached["width"] = video->width;
        cached["height"] = video->height;
        cached["frameRate"] = encode(video->frameRate);
        // The codec name too, so a bin opened tomorrow can still be searched
        // for "prores" without reopening every file to find out.
        cached["videoCodec"] = video->codecName;
    }
    if (const media::AudioStreamInfo* audio = ref.info.primaryAudio()) {
        cached["audioSampleRate"] = encode(audio->sampleRate);
        cached["audioChannels"] = audio->channelCount;
        cached["audioCodec"] = audio->codecName;
    }
    json out{{"id", ref.id.value()},
             {"path", ref.path},
             {"contentHash", ref.contentHash},
             {"contentDigest", ref.contentDigest},
             {"notes", ref.notes},
             {"name", ref.name},
             {"cachedInfo", std::move(cached)}};
    if (!ref.proxyPath.empty()) {
        out["proxyPath"] = ref.proxyPath;
    }
    if (ref.transferOverride != media::TransferFunction::Unknown) {
        // Only when somebody has said so: a file the container described
        // correctly should not carry a line asserting what it already says.
        out["transferOverride"] = media::toString(ref.transferOverride);
    }
    return out;
}

// --- Unknown-field preservation ---------------------------------------------

/// Copy anything in `original` that `out` does not have.
///
/// Arrays of objects are matched by "id" rather than by position, because a
/// clip that moved from index 3 to index 5 is still the same clip and should
/// keep whatever the newer build attached to it.
void mergePreserved(json& out, const json& original) {
    if (out.is_object() && original.is_object()) {
        for (const auto& [key, value] : original.items()) {
            if (!out.contains(key)) {
                out[key] = value;
            } else {
                mergePreserved(out[key], value);
            }
        }
        return;
    }
    if (out.is_array() && original.is_array()) {
        for (json& element : out) {
            if (!element.is_object() || !element.contains("id")) {
                continue;
            }
            const auto& id = element.at("id");
            for (const json& originalElement : original) {
                if (originalElement.is_object() && originalElement.contains("id") &&
                    originalElement.at("id") == id) {
                    mergePreserved(element, originalElement);
                    break;
                }
            }
        }
    }
}

}  // namespace zaro::io::detail
