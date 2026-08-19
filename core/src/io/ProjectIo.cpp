#include "zaro/core/io/ProjectIo.h"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

#include "zaro/core/model/ClipEffects.h"
#include "zaro/core/model/Transition.h"
#include "zaro/core/time/Timecode.h"

namespace zaro::io {

using json = nlohmann::json;

/// Just the original document. Everything the writer does not re-emit gets
/// merged back from here.
class UnknownFields {
public:
    explicit UnknownFields(json document) : document_{std::move(document)} {}
    [[nodiscard]] const json& document() const noexcept { return document_; }

private:
    json document_;
};

namespace {

// --- Time encoding ----------------------------------------------------------
// Rationals are written as "30000/1001" rather than a decimal, because the
// whole point of the type is that 29.97 is not a decimal. A project file that
// says 29.97 has already lost the information.

json encode(const time::Rational& value) {
    return value.toString();
}

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

json encode(const time::RationalTime& value) {
    return json{{"frames", value.frames()}, {"rate", encode(value.rate())}};
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

json encode(const time::TimeRange& value) {
    return json{{"start", encode(value.start())}, {"duration", encode(value.duration())}};
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

json encode(const model::ClipAnimation& animation) {
    // Curves keyed by parameter name rather than an array of {param, curve}
    // pairs: a parameter can only be animated once, and a map says so in the
    // format instead of leaving a reader to enforce it.
    json out = json::object();
    for (const auto& [param, curve] : animation) {
        if (curve.empty()) {
            continue;
        }
        json keys = json::array();
        for (const model::Keyframe& key : curve.keyframes()) {
            const model::Keyframe defaults;
            json encoded{{"time", encode(key.time)}, {"value", key.value}};
            if (key.interpolation != defaults.interpolation) {
                encoded["interpolation"] = model::toString(key.interpolation);
            }
            // Handles are only meaningful for beziers, and writing the default
            // ease onto every linear keyframe would triple the size of a long
            // automation curve for no information.
            if (key.out != defaults.out) {
                encoded["out"] = json{{"dx", key.out.dx}, {"dy", key.out.dy}};
            }
            if (key.in != defaults.in) {
                encoded["in"] = json{{"dx", key.in.dx}, {"dy", key.in.dy}};
            }
            keys.push_back(std::move(encoded));
        }
        out[model::toString(param)] = std::move(keys);
    }
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
        model::Curve curve;
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
            // set() rather than push_back: a hand-edited or corrupted file can
            // hold keyframes out of order or twice over, and evaluation depends
            // on neither being possible.
            curve.set(key);
        }
        if (!curve.empty()) {
            out.curve(param) = std::move(curve);
        }
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
    if (json animation = encode(clip.animation); !animation.empty()) {
        out["animation"] = std::move(animation);
    }
    if (clip.link.isValid()) {
        out["link"] = clip.link.value();
    }
    return out;
}

json encode(const model::Transition& transition) {
    return json{{"id", transition.id.value()},
                {"from", transition.from.value()},
                {"to", transition.to.value()},
                {"kind", model::toString(transition.kind)},
                {"range", encode(transition.range)}};
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
    auto range = decodeRange(node.at("range"), "transition range");
    if (!range) {
        return range.error();
    }
    transition.range = *range;
    return transition;
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
    return out;
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
    return marker;
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
    return json{{"id", sequence.id().value()},
                {"name", sequence.name()},
                {"frameRate", encode(sequence.frameRate())},
                {"audioSampleRate", encode(sequence.audioSampleRate())},
                {"width", sequence.width()},
                {"height", sequence.height()},
                {"startTime", encode(sequence.startTime())},
                {"videoTracks", std::move(videoTracks)},
                {"audioTracks", std::move(audioTracks)},
                {"markers", std::move(markers)}};
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
    }
    if (const media::AudioStreamInfo* audio = ref.info.primaryAudio()) {
        cached["audioSampleRate"] = encode(audio->sampleRate);
        cached["audioChannels"] = audio->channelCount;
    }
    return json{{"id", ref.id.value()},
                {"path", ref.path},
                {"contentHash", ref.contentHash},
                {"name", ref.name},
                {"cachedInfo", std::move(cached)}};
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
    ref.contentHash = node.value("contentHash", std::string{});
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
            audio.duration = ref.info.duration;
            ref.info.audioStreams.push_back(std::move(audio));
        }
        if (cached.contains("width") && cached.contains("frameRate")) {
            media::VideoStreamInfo video;
            video.width = cached.value("width", 0);
            video.height = cached.value("height", 0);
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

}  // namespace

Result<std::string> saveProjectToString(const model::Project& project,
                                        const std::shared_ptr<const UnknownFields>& unknown) {
    json media = json::array();
    for (const model::MediaRef& ref : project.media()) {
        media.push_back(encode(ref));
    }
    json sequences = json::array();
    for (const model::Sequence& sequence : project.sequences()) {
        sequences.push_back(encode(sequence));
    }

    json document{{"zaro", {{"schemaVersion", kProjectSchemaVersion}}},
                  {"activeSequence", project.activeSequence().value()},
                  {"media", std::move(media)},
                  {"sequences", std::move(sequences)}};

    if (unknown != nullptr) {
        mergePreserved(document, unknown->document());
    }
    return document.dump(2) + "\n";
}

Status saveProject(const model::Project& project, const std::string& path,
                   const std::shared_ptr<const UnknownFields>& unknown) {
    auto text = saveProjectToString(project, unknown);
    if (!text) {
        return text.error();
    }
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file) {
        return Error{ErrorCode::Io, "cannot open " + path + " for writing"};
    }
    file << *text;
    if (!file) {
        return Error{ErrorCode::Io, "failed while writing " + path};
    }
    return {};
}

Result<LoadedProject> loadProjectFromString(const std::string& text) {
    json document = json::parse(text, nullptr, false);
    if (document.is_discarded()) {
        return Error{ErrorCode::InvalidData, "this is not valid JSON"};
    }
    if (!document.is_object() || !document.contains("zaro")) {
        return Error{ErrorCode::InvalidData, "this is not a Zaro project file"};
    }

    const int version = document.at("zaro").value("schemaVersion", 0);
    if (version > kProjectSchemaVersion) {
        // Load anyway. Unknown fields are preserved, so the worst case is that
        // parts of the project are invisible in this build rather than lost.
        // Refusing outright would be safer only if saving destroyed them.
    }
    if (version < 1) {
        return Error{ErrorCode::InvalidData, "this project file has no usable schema version"};
    }

    LoadedProject loaded;
    std::vector<model::MediaRef> media;
    for (const json& node : document.value("media", json::array())) {
        auto ref = decodeMedia(node);
        if (!ref) {
            return ref.error();
        }
        media.push_back(std::move(*ref));
    }
    std::vector<model::Sequence> sequences;
    for (const json& node : document.value("sequences", json::array())) {
        auto sequence = decodeSequence(node);
        if (!sequence) {
            return sequence.error();
        }
        sequences.push_back(std::move(*sequence));
    }

    loaded.project.setMedia(std::move(media));
    loaded.project.setSequences(std::move(sequences));
    loaded.project.setActiveSequence(
        model::SequenceId{document.value("activeSequence", std::uint64_t{0})});

    // Restart the id counter past everything in the file, so ids issued from
    // here on cannot collide with something already pointed at.
    loaded.project.ids().observe(highestId(loaded.project));
    loaded.unknown = std::make_shared<const UnknownFields>(std::move(document));
    return loaded;
}

Result<LoadedProject> loadProject(const std::string& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return Error{ErrorCode::NotFound, "cannot open " + path};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return loadProjectFromString(buffer.str());
}

}  // namespace zaro::io
