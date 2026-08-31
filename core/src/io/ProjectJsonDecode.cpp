// Reading a project back: JSON to model types.
//
// Every decoder is total -- a missing field takes the model's default rather
// than failing -- except where a malformed one would silently change the cut,
// which is why some return Result and most do not.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "zaro/core/model/ClipEffects.h"
#include "zaro/core/model/Transition.h"
#include "zaro/core/time/Timecode.h"

#include "ProjectJson.h"

namespace zaro::io::detail {

Result<time::Rational> decodeRational(const json& node, const char* what) {
    if (!node.is_string()) {
        return Error{ErrorCode::InvalidData,
                     std::string{what} + " should be a string like \"30000/1001\""};
    }
    const auto parsed = time::Rational::parse(node.get<std::string>());
    if (!parsed) {
        return Error{ErrorCode::InvalidData, std::string{what} + ": cannot read \"" +
                                                 node.get<std::string>() + "\" as a rate"};
    }
    return *parsed;
}

Result<time::RationalTime> decodeTime(const json& node, const char* what) {
    if (!node.is_object() || !node.contains("frames") || !node.contains("rate")) {
        return Error{ErrorCode::InvalidData, std::string{what} + " needs \"frames\" and \"rate\""};
    }
    auto rate = decodeRational(node.at("rate"), what);
    if (!rate) {
        return rate.error();
    }
    return time::RationalTime{node.at("frames").get<std::int64_t>(), *rate};
}

Result<time::TimeRange> decodeRange(const json& node, const char* what) {
    if (!node.is_object()) {
        return Error{ErrorCode::InvalidData, std::string{what} + " should be an object"};
    }
    auto start = decodeTime(node.at("start"), what);
    if (!start) {
        return start.error();
    }
    auto duration = decodeTime(node.at("duration"), what);
    if (!duration) {
        return duration.error();
    }
    if (duration->frames() < 0) {
        return Error{ErrorCode::InvalidData, std::string{what} + " has a negative duration"};
    }
    return time::TimeRange{*start, *duration};
}

model::Transform decodeTransform(const json& node) {
    model::Transform out;
    if (!node.is_object()) {
        return out;
    }
    out.positionX = node.value("positionX", out.positionX);
    out.positionY = node.value("positionY", out.positionY);
    out.scaleX = node.value("scaleX", out.scaleX);
    out.scaleY = node.value("scaleY", out.scaleY);
    out.rotationDegrees = node.value("rotationDegrees", out.rotationDegrees);
    out.anchorX = node.value("anchorX", out.anchorX);
    out.anchorY = node.value("anchorY", out.anchorY);
    out.opacity = node.value("opacity", out.opacity);
    return out;
}

model::ColorCorrection decodeColor(const json& node) {
    model::ColorCorrection out;
    if (!node.is_object()) {
        return out;
    }
    out.temperature = node.value("temperature", out.temperature);
    out.tint = node.value("tint", out.tint);
    out.exposure = node.value("exposure", out.exposure);
    out.contrast = node.value("contrast", out.contrast);
    out.saturation = node.value("saturation", out.saturation);
    return out;
}

Result<model::Curve> decodeCurve(const json& keys) {
    model::Curve curve;
    if (!keys.is_array()) {
        return curve;
    }
    for (const json& encoded : keys) {
        if (!encoded.is_object() || !encoded.contains("time")) {
            return Error{ErrorCode::InvalidData, "a keyframe has no time"};
        }
        auto when = decodeTime(encoded.at("time"), "keyframe time");
        if (!when) {
            return when.error();
        }
        model::Keyframe key;
        key.time = *when;
        key.value = encoded.value("value", 0.0);
        if (encoded.contains("interpolation")) {
            key.interpolation = model::interpolationFromString(
                encoded.at("interpolation").get<std::string>().c_str());
        }
        const auto handle = [&encoded](const char* which, model::Handle& into) {
            if (!encoded.contains(which) || !encoded.at(which).is_object()) {
                return;
            }
            const json& from = encoded.at(which);
            into.dx = from.value("dx", into.dx);
            into.dy = from.value("dy", into.dy);
        };
        handle("out", key.out);
        handle("in", key.in);
        // set() rather than push_back: a hand-edited or corrupted file can hold
        // keyframes out of order or twice over, and evaluation depends on
        // neither being possible.
        curve.set(key);
    }
    return curve;
}

std::vector<model::Effect> decodeEffects(const json& node) {
    std::vector<model::Effect> out;
    if (!node.is_array()) {
        return out;
    }
    for (const json& entry : node) {
        if (!entry.is_object()) {
            continue;
        }
        model::Effect effect;
        const std::string kind = entry.value("kind", std::string{});
        if (!model::effectKindFromString(kind.c_str(), effect.kind)) {
            // An effect this build has never heard of, from a file a later one
            // wrote. Dropped rather than refused: the rest of the cut is still
            // openable, and UnknownFields carries the original back out on save.
            continue;
        }
        effect.enabled = entry.value("enabled", true);
        if (entry.contains("values") && entry.at("values").is_object()) {
            for (const auto& [name, value] : entry.at("values").items()) {
                model::EffectParam param{};
                if (model::effectParamFromString(name.c_str(), param) && value.is_number()) {
                    effect.values[param] = value.get<double>();
                }
            }
        }
        if (entry.contains("curves") && entry.at("curves").is_object()) {
            for (const auto& [name, keys] : entry.at("curves").items()) {
                model::EffectParam param{};
                if (!model::effectParamFromString(name.c_str(), param)) {
                    continue;
                }
                if (auto curve = decodeCurve(keys); curve && !curve->empty()) {
                    effect.animation[param] = std::move(*curve);
                }
            }
        }
        out.push_back(std::move(effect));
    }
    return out;
}

/// The two audio processors, read once and used by both the track that has a
/// pair and the clip that has a pair. Every field falls back to the struct's
/// own default, so a file written before a field existed reads as the value
/// that field was introduced with rather than as zero.
model::AudioEq decodeEq(const json& node) {
    model::AudioEq eq;
    eq.enabled = node.value("enabled", false);
    eq.highPassHz = node.value("highPassHz", eq.highPassHz);
    eq.lowPassHz = node.value("lowPassHz", eq.lowPassHz);
    eq.peakHz = node.value("peakHz", eq.peakHz);
    eq.peakGainDb = node.value("peakGainDb", eq.peakGainDb);
    eq.peakQ = node.value("peakQ", eq.peakQ);
    return eq;
}

model::Compressor decodeCompressor(const json& node) {
    model::Compressor compressor;
    compressor.enabled = node.value("enabled", false);
    compressor.thresholdDb = node.value("thresholdDb", compressor.thresholdDb);
    compressor.ratio = node.value("ratio", compressor.ratio);
    compressor.attackMs = node.value("attackMs", compressor.attackMs);
    compressor.releaseMs = node.value("releaseMs", compressor.releaseMs);
    compressor.makeupDb = node.value("makeupDb", compressor.makeupDb);
    return compressor;
}

model::Vignette decodeVignette(const json& node) {
    model::Vignette out;
    if (!node.is_object()) {
        return out;
    }
    out.amount = node.value("amount", out.amount);
    out.midpoint = node.value("midpoint", out.midpoint);
    out.feather = node.value("feather", out.feather);
    out.roundness = node.value("roundness", out.roundness);
    return out;
}

model::ColorWheels decodeWheels(const json& node) {
    model::ColorWheels out;
    if (!node.is_object()) {
        return out;
    }
    out.slopeR = node.value("slopeR", out.slopeR);
    out.slopeG = node.value("slopeG", out.slopeG);
    out.slopeB = node.value("slopeB", out.slopeB);
    out.offsetR = node.value("offsetR", out.offsetR);
    out.offsetG = node.value("offsetG", out.offsetG);
    out.offsetB = node.value("offsetB", out.offsetB);
    out.powerR = node.value("powerR", out.powerR);
    out.powerG = node.value("powerG", out.powerG);
    out.powerB = node.value("powerB", out.powerB);
    return out;
}

model::Keyer decodeKeyer(const json& node) {
    model::Keyer out;
    if (!node.is_object()) {
        return out;
    }
    const std::string kind = node.value("kind", std::string{});
    if (kind == "chroma") {
        out.kind = model::KeyKind::Chroma;
    } else if (kind == "luma") {
        out.kind = model::KeyKind::Luma;
    }
    out.red = node.value("red", out.red);
    out.green = node.value("green", out.green);
    out.blue = node.value("blue", out.blue);
    out.tolerance = node.value("tolerance", out.tolerance);
    out.softness = node.value("softness", out.softness);
    out.lumaLow = node.value("lumaLow", out.lumaLow);
    out.lumaHigh = node.value("lumaHigh", out.lumaHigh);
    out.lumaSoftness = node.value("lumaSoftness", out.lumaSoftness);
    out.spill = node.value("spill", out.spill);
    return out;
}

model::Secondary decodeSecondary(const json& node) {
    model::Secondary out;
    if (!node.is_object()) {
        return out;
    }
    if (node.contains("qualifier") && node.at("qualifier").is_object()) {
        const json& window = node.at("qualifier");
        model::HslQualifier& into = out.qualifier;
        into.enabled = window.value("enabled", false);
        into.hueCentre = window.value("hueCentre", into.hueCentre);
        into.hueWidth = window.value("hueWidth", into.hueWidth);
        into.hueSoftness = window.value("hueSoftness", into.hueSoftness);
        into.saturationLow = window.value("saturationLow", into.saturationLow);
        into.saturationHigh = window.value("saturationHigh", into.saturationHigh);
        into.saturationSoftness = window.value("saturationSoftness", into.saturationSoftness);
        into.lumaLow = window.value("lumaLow", into.lumaLow);
        into.lumaHigh = window.value("lumaHigh", into.lumaHigh);
        into.lumaSoftness = window.value("lumaSoftness", into.lumaSoftness);
    }
    if (node.contains("correction")) {
        out.correction = decodeColor(node.at("correction"));
    }
    return out;
}

model::ColorCurves decodeColorCurves(const json& node) {
    model::ColorCurves out;
    if (!node.is_object()) {
        return out;
    }
    const auto load = [&node](const char* name, model::ToneCurve& into) {
        if (!node.contains(name) || !node.at(name).is_array()) {
            return;
        }
        for (const json& point : node.at(name)) {
            if (!point.is_object()) {
                continue;
            }
            // set() rather than push_back, for the reason the tone curves give:
            // a hand-edited file can hold points out of order or twice over,
            // and evaluation depends on neither being possible.
            into.set(model::CurvePoint{point.value("x", 0.0), point.value("y", 0.0)});
        }
    };
    // "saturation" is the hue curve's key from the build that shipped it.
    // Renaming it in the model does not license renaming it in the file: a
    // project written by that build has to keep opening.
    load("saturation", out.againstHue);
    load("luma", out.againstLuma);
    load("hueShift", out.hueShift);
    return out;
}

model::ToneCurves decodeCurves(const json& node) {
    model::ToneCurves out;
    if (!node.is_object()) {
        return out;
    }
    const auto load = [&node](const char* name, model::ToneCurve& into) {
        if (!node.contains(name) || !node.at(name).is_array()) {
            return;
        }
        for (const json& point : node.at(name)) {
            if (!point.is_object()) {
                continue;
            }
            // set() rather than push_back: a hand-edited file can hold points
            // out of order or twice over, and evaluation depends on neither
            // being possible.
            into.set(model::CurvePoint{point.value("x", 0.0), point.value("y", 0.0)});
        }
    };
    load("master", out.master);
    load("red", out.red);
    load("green", out.green);
    load("blue", out.blue);
    return out;
}

Result<model::ClipAnimation> decodeAnimation(const json& node) {
    model::ClipAnimation out;
    if (!node.is_object()) {
        return out;
    }
    for (const auto& [name, keys] : node.items()) {
        model::Param param{};
        if (!model::paramFromString(name.c_str(), param) || !keys.is_array()) {
            // A parameter this version does not know about. Written by a later
            // one, most likely; dropping the curve is better than refusing to
            // open the project.
            continue;
        }
        auto curve = decodeCurve(keys);
        if (!curve) {
            return curve.error();
        }
        if (!curve->empty()) {
            out.curve(param) = std::move(*curve);
        }
    }
    return out;
}

Result<model::Transition> decodeTransition(const json& node) {
    model::Transition transition;
    transition.id = model::TransitionId{node.value("id", std::uint64_t{0})};
    transition.from = model::ClipId{node.value("from", std::uint64_t{0})};
    transition.to = model::ClipId{node.value("to", std::uint64_t{0})};
    if (!transition.id.isValid() || !transition.from.isValid() || !transition.to.isValid()) {
        return Error{ErrorCode::InvalidData, "a transition is missing an id"};
    }
    if (node.contains("kind")) {
        transition.kind =
            model::transitionKindFromString(node.at("kind").get<std::string>().c_str());
    }
    if (node.contains("direction")) {
        model::TransitionDirection direction{};
        if (model::transitionDirectionFromString(node.at("direction").get<std::string>().c_str(),
                                                 direction)) {
            transition.direction = direction;
        }
    }
    auto range = decodeRange(node.at("range"), "transition range");
    if (!range) {
        return range.error();
    }
    transition.range = *range;
    return transition;
}

Result<model::Marker> decodeMarker(const json& node) {
    model::Marker marker;
    marker.id = model::MarkerId{node.value("id", std::uint64_t{0})};
    if (!marker.id.isValid()) {
        return Error{ErrorCode::InvalidData, "a marker has no id"};
    }
    auto range = decodeRange(node.at("range"), "marker range");
    if (!range) {
        return range.error();
    }
    marker.range = *range;
    marker.name = node.value("name", std::string{});
    marker.note = node.value("note", std::string{});
    marker.colour = node.value("colour", 0);
    marker.author = node.value("author", std::string{});
    marker.resolved = node.value("resolved", false);
    return marker;
}

Result<model::Subclip> decodeSubclip(const json& node) {
    if (!node.is_object() || !node.contains("range")) {
        return Error{ErrorCode::InvalidData, "a subclip has no range"};
    }
    auto range = decodeRange(node.at("range"), "subclip range");
    if (!range) {
        return range.error();
    }
    model::Subclip out;
    out.id = model::SubclipId{node.value("id", std::uint64_t{0})};
    out.source = model::MediaRefId{node.value("source", std::uint64_t{0})};
    out.range = *range;
    out.name = node.value("name", std::string{});
    return out;
}

// --- Decoding ---------------------------------------------------------------

Result<model::Clip> decodeClip(const json& node) {
    model::Clip clip;
    clip.id = model::ClipId{node.value("id", std::uint64_t{0})};
    clip.source = model::MediaRefId{node.value("source", std::uint64_t{0})};
    clip.name = node.value("name", std::string{});
    clip.enabled = node.value("enabled", true);

    if (!clip.id.isValid()) {
        return Error{ErrorCode::InvalidData, "a clip has no id"};
    }
    auto sourceRange = decodeRange(node.at("sourceRange"), "clip sourceRange");
    if (!sourceRange) {
        return sourceRange.error();
    }
    auto timelineRange = decodeRange(node.at("timelineRange"), "clip timelineRange");
    if (!timelineRange) {
        return timelineRange.error();
    }
    clip.sourceRange = *sourceRange;
    clip.timelineRange = *timelineRange;

    if (node.contains("transform")) {
        clip.transform = decodeTransform(node.at("transform"));
    }
    if (node.contains("blend")) {
        clip.blend = model::blendModeFromString(node.at("blend").get<std::string>().c_str());
    }
    clip.gainDb = node.value("gainDb", 0.0);
    clip.pan = node.value("pan", 0.0);
    if (node.contains("color")) {
        clip.color = decodeColor(node.at("color"));
    }
    clip.adjustment = node.value("adjustment", false);
    if (node.contains("angles") && node.at("angles").is_array()) {
        for (const json& entry : node.at("angles")) {
            if (!entry.is_object() || !entry.contains("offset")) {
                continue;
            }
            auto offset = decodeTime(entry.at("offset"), "angle offset");
            if (!offset) {
                return offset.error();
            }
            model::Clip::Angle angle;
            angle.media = model::MediaRefId{entry.value("media", std::uint64_t{0})};
            angle.offset = *offset;
            angle.name = entry.value("name", std::string{});
            clip.angles.push_back(std::move(angle));
        }
        clip.activeAngle = node.value("activeAngle", 0);
    }
    clip.nested = model::SequenceId{node.value("nested", std::uint64_t{0})};
    clip.reversed = node.value("reversed", false);
    if (node.contains("mask") && node.at("mask").is_object()) {
        const json& mask = node.at("mask");
        model::Mask& into = clip.mask;
        into.shape = model::maskShapeFromString(mask.value("shape", std::string{"none"}).c_str());
        into.width = mask.value("width", into.width);
        into.height = mask.value("height", into.height);
        into.centreX = mask.value("centreX", into.centreX);
        into.centreY = mask.value("centreY", into.centreY);
        into.cornerRadius = mask.value("cornerRadius", into.cornerRadius);
        into.feather = mask.value("feather", into.feather);
        into.inverted = mask.value("inverted", false);
        if (mask.contains("path") && mask.at("path").is_array()) {
            for (const json& encoded : mask.at("path")) {
                if (!encoded.is_object()) {
                    continue;
                }
                model::MaskPoint point;
                point.x = encoded.value("x", 0.0);
                point.y = encoded.value("y", 0.0);
                const auto handle = [&encoded](const char* which, double& hx, double& hy) {
                    if (encoded.contains(which) && encoded.at(which).is_object()) {
                        hx = encoded.at(which).value("x", 0.0);
                        hy = encoded.at(which).value("y", 0.0);
                    }
                };
                handle("in", point.inX, point.inY);
                handle("out", point.outX, point.outY);
                into.path.points.push_back(point);
            }
        }
    }
    clip.pinnedTo = model::ClipId{node.value("pinnedTo", std::uint64_t{0})};
    if (node.contains("responsive") && node.at("responsive").is_object()) {
        const json& responsive = node.at("responsive");
        const auto part = [&responsive](const char* key, time::RationalTime& into) {
            if (responsive.contains(key)) {
                if (auto decoded = decodeTime(responsive.at(key), "clip responsive")) {
                    into = *decoded;
                }
            }
        };
        part("intro", clip.responsive.intro);
        part("outro", clip.responsive.outro);
        part("authored", clip.responsive.authored);
    }
    if (node.contains("graphic") && node.at("graphic").is_object()) {
        const json& graphic = node.at("graphic");
        model::Graphic& into = clip.graphic;
        into.kind =
            model::graphicKindFromString(graphic.value("kind", std::string{"none"}).c_str());
        into.width = graphic.value("width", into.width);
        into.height = graphic.value("height", into.height);
        into.centreX = graphic.value("centreX", into.centreX);
        into.centreY = graphic.value("centreY", into.centreY);
        into.cornerRadius = graphic.value("cornerRadius", into.cornerRadius);
        into.feather = graphic.value("feather", into.feather);
        into.red = graphic.value("red", into.red);
        into.green = graphic.value("green", into.green);
        into.blue = graphic.value("blue", into.blue);
        into.alpha = graphic.value("alpha", into.alpha);
        into.text = graphic.value("text", std::string{});
        into.family = graphic.value("family", std::string{});
        into.pointSize = graphic.value("pointSize", into.pointSize);
        into.bold = graphic.value("bold", false);
        into.italic = graphic.value("italic", false);
        into.alignment = graphic.value("alignment", into.alignment);
    }
    if (node.contains("lut") && node.at("lut").is_object()) {
        clip.lut.path = node.at("lut").value("path", std::string{});
        clip.lut.amount = node.at("lut").value("amount", 1.0);
    }
    if (node.contains("secondary")) {
        clip.secondary = decodeSecondary(node.at("secondary"));
    }
    if (node.contains("role")) {
        model::AudioRole role{};
        if (model::audioRoleFromString(node.at("role").get<std::string>().c_str(), role)) {
            clip.role = role;
        }
    }
    if (node.contains("eq") && node.at("eq").is_object()) {
        clip.eq = decodeEq(node.at("eq"));
    }
    if (node.contains("compressor") && node.at("compressor").is_object()) {
        clip.compressor = decodeCompressor(node.at("compressor"));
    }
    if (node.contains("vignette")) {
        clip.vignette = decodeVignette(node.at("vignette"));
    }
    if (node.contains("wheels")) {
        clip.wheels = decodeWheels(node.at("wheels"));
    }
    if (node.contains("keyer")) {
        clip.keyer = decodeKeyer(node.at("keyer"));
    }
    if (node.contains("effects")) {
        clip.effects = decodeEffects(node.at("effects"));
    }
    if (node.contains("hueCurves")) {
        clip.colorCurves = decodeColorCurves(node.at("hueCurves"));
    }
    if (node.contains("curves")) {
        clip.curves = decodeCurves(node.at("curves"));
    }
    if (node.contains("animation")) {
        auto animation = decodeAnimation(node.at("animation"));
        if (!animation) {
            return animation.error();
        }
        clip.animation = std::move(*animation);
    }
    clip.link = model::LinkId{node.value("link", std::uint64_t{0})};
    return clip;
}

Result<model::Track> decodeTrack(const json& node, model::TrackKind kind) {
    const auto id = model::TrackId{node.value("id", std::uint64_t{0})};
    if (!id.isValid()) {
        return Error{ErrorCode::InvalidData, "a track has no id"};
    }
    model::Track track{id, kind, node.value("name", std::string{})};
    track.setMuted(node.value("muted", false));
    track.setSoloed(node.value("soloed", false));
    if (node.contains("eq") && node.at("eq").is_object()) {
        track.setEq(decodeEq(node.at("eq")));
    }
    if (node.contains("compressor") && node.at("compressor").is_object()) {
        track.setCompressor(decodeCompressor(node.at("compressor")));
    }
    track.setLocked(node.value("locked", false));
    track.setGainDb(node.value("gainDb", 0.0));
    track.setPan(node.value("pan", 0.0));
    track.setSyncLocked(node.value("syncLocked", true));

    std::vector<model::Clip> clips;
    for (const json& clipNode : node.value("clips", json::array())) {
        auto clip = decodeClip(clipNode);
        if (!clip) {
            return clip.error();
        }
        clips.push_back(std::move(*clip));
    }
    // setClips enforces the sorted, non-overlapping invariant, so a corrupt or
    // hand-edited file is caught here rather than halfway through an edit.
    track.setClips(std::move(clips));

    std::vector<model::Transition> transitions;
    for (const json& transitionNode : node.value("transitions", json::array())) {
        auto transition = decodeTransition(transitionNode);
        if (!transition) {
            return transition.error();
        }
        transitions.push_back(std::move(*transition));
    }
    track.setTransitions(std::move(transitions));
    return track;
}

Result<model::Sequence> decodeSequence(const json& node) {
    const auto id = model::SequenceId{node.value("id", std::uint64_t{0})};
    if (!id.isValid()) {
        return Error{ErrorCode::InvalidData, "a sequence has no id"};
    }
    auto frameRate = decodeRational(node.at("frameRate"), "sequence frameRate");
    if (!frameRate) {
        return frameRate.error();
    }
    model::Sequence sequence{id, node.value("name", std::string{}), *frameRate};

    if (node.contains("audioSampleRate")) {
        auto rate = decodeRational(node.at("audioSampleRate"), "sequence audioSampleRate");
        if (!rate) {
            return rate.error();
        }
        sequence.setAudioSampleRate(*rate);
    }
    sequence.setSize(node.value("width", 1920), node.value("height", 1080));
    if (node.contains("output") && node.at("output").is_object()) {
        const json& output = node.at("output");
        model::Sequence::Output delivery;
        if (output.contains("transfer")) {
            media::TransferFunction transfer{};
            if (media::transferFunctionFromString(output.at("transfer").get<std::string>().c_str(),
                                                  transfer)) {
                delivery.transfer = transfer;
            }
        }
        delivery.highlightKnee = output.value("highlightKnee", delivery.highlightKnee);
        sequence.setOutput(delivery);
    }
    if (node.contains("startTime")) {
        auto start = decodeTime(node.at("startTime"), "sequence startTime");
        if (!start) {
            return start.error();
        }
        sequence.setStartTime(*start);
    }

    const auto loadTracks = [&](const char* key, model::TrackKind kind) -> Status {
        for (const json& trackNode : node.value(key, json::array())) {
            auto track = decodeTrack(trackNode, kind);
            if (!track) {
                return track.error();
            }
            sequence.tracksMutable(kind).push_back(std::move(*track));
        }
        return {};
    };
    if (Status status = loadTracks("videoTracks", model::TrackKind::Video); !status) {
        return status.error();
    }
    if (Status status = loadTracks("audioTracks", model::TrackKind::Audio); !status) {
        return status.error();
    }

    if (node.contains("captions") && node.at("captions").is_object()) {
        const json& captionNode = node.at("captions");
        model::CaptionTrack& track = sequence.captions();
        track.setBurnedIn(captionNode.value("burnIn", false));
        if (captionNode.contains("style") && captionNode.at("style").is_object()) {
            const json& styleNode = captionNode.at("style");
            model::CaptionStyle style;
            style.family = styleNode.value("family", std::string{});
            style.pointSize = styleNode.value("pointSize", style.pointSize);
            style.bold = styleNode.value("bold", false);
            style.bottomMargin = styleNode.value("bottomMargin", style.bottomMargin);
            style.widthFraction = styleNode.value("widthFraction", style.widthFraction);
            style.red = styleNode.value("red", style.red);
            style.green = styleNode.value("green", style.green);
            style.blue = styleNode.value("blue", style.blue);
            style.alpha = styleNode.value("alpha", style.alpha);
            track.setStyle(style);
        }
        for (const json& cue : captionNode.value("cues", json::array())) {
            if (!cue.is_object() || !cue.contains("range")) {
                continue;
            }
            auto range = decodeRange(cue.at("range"), "caption range");
            if (!range) {
                return range.error();
            }
            model::Caption caption;
            caption.range = *range;
            caption.text = cue.value("text", std::string{});
            track.add(caption);
        }
    }

    std::vector<model::Marker> markers;
    for (const json& markerNode : node.value("markers", json::array())) {
        auto marker = decodeMarker(markerNode);
        if (!marker) {
            return marker.error();
        }
        markers.push_back(std::move(*marker));
    }
    sequence.setMarkers(std::move(markers));
    return sequence;
}

Result<model::MediaRef> decodeMedia(const json& node) {
    model::MediaRef ref;
    ref.id = model::MediaRefId{node.value("id", std::uint64_t{0})};
    if (!ref.id.isValid()) {
        return Error{ErrorCode::InvalidData, "a media reference has no id"};
    }
    ref.path = node.value("path", std::string{});
    ref.proxyPath = node.value("proxyPath", std::string{});
    if (node.contains("primariesOverride")) {
        media::ColorPrimaries primaries{};
        if (media::colorPrimariesFromString(node.at("primariesOverride").get<std::string>().c_str(),
                                            primaries)) {
            ref.primariesOverride = primaries;
        }
    }
    if (node.contains("transferOverride")) {
        media::TransferFunction transfer{};
        if (media::transferFunctionFromString(
                node.at("transferOverride").get<std::string>().c_str(), transfer)) {
            ref.transferOverride = transfer;
        }
    }
    ref.contentHash = node.value("contentHash", std::string{});
    ref.contentDigest = node.value("contentDigest", std::string{});
    ref.notes = node.value("notes", std::string{});
    ref.name = node.value("name", std::string{});

    if (node.contains("cachedInfo")) {
        const json& cached = node.at("cachedInfo");
        ref.info.path = ref.path;
        if (cached.contains("duration")) {
            // Written by encode(Rational) as "400/1", so it has to be read back
            // the same way. Reading it as a {frames, rate} object silently
            // failed, and a media duration of zero means every trim bound
            // disappears the moment a project is reopened.
            auto duration = decodeRational(cached.at("duration"), "media duration");
            if (!duration) {
                return duration.error();
            }
            ref.info.duration = *duration;
        }
        if (cached.contains("audioSampleRate")) {
            media::AudioStreamInfo audio;
            if (auto rate = decodeRational(cached.at("audioSampleRate"), "media audioSampleRate")) {
                audio.sampleRate = *rate;
            }
            audio.channelCount = cached.value("audioChannels", 0);
            audio.codecName = cached.value("audioCodec", std::string{});
            audio.duration = ref.info.duration;
            ref.info.audioStreams.push_back(std::move(audio));
        }
        if (cached.contains("width") && cached.contains("frameRate")) {
            media::VideoStreamInfo video;
            video.width = cached.value("width", 0);
            video.height = cached.value("height", 0);
            video.codecName = cached.value("videoCodec", std::string{});
            if (auto rate = decodeRational(cached.at("frameRate"), "media frameRate")) {
                video.frameRate = *rate;
                video.averageFrameRate = *rate;
            }
            video.duration = ref.info.duration;
            ref.info.videoStreams.push_back(std::move(video));
        }
    }
    return ref;
}

/// The largest id of any kind in the project.
///
/// Every id type shares one counter, so this has to see all of them. Missing
/// one means the counter restarts below an id already in use and the next
/// thing created silently collides with something — which is far worse than a
/// field failing to round trip, because the file is fine and the corruption
/// happens later, in memory, to whoever opens it.
std::uint64_t highestId(const model::Project& project) {
    std::uint64_t highest = 0;
    const auto bump = [&highest](std::uint64_t value) { highest = std::max(highest, value); };
    for (const model::MediaRef& ref : project.media()) {
        bump(ref.id.value());
    }
    for (const model::Subclip& subclip : project.subclips()) {
        bump(subclip.id.value());
    }
    for (const model::Sequence& sequence : project.sequences()) {
        bump(sequence.id().value());
        for (const model::Marker& marker : sequence.markers()) {
            bump(marker.id.value());
        }
        for (const auto* list : {&sequence.videoTracks(), &sequence.audioTracks()}) {
            for (const model::Track& track : *list) {
                bump(track.id().value());
                for (const model::Clip& clip : track.clips()) {
                    bump(clip.id.value());
                    bump(clip.link.value());
                }
                for (const model::Transition& transition : track.transitions()) {
                    bump(transition.id.value());
                }
            }
        }
    }
    return highest;
}

}  // namespace zaro::io::detail
