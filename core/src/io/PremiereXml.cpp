#include "zaro/core/io/PremiereXml.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

#include "zaro/core/time/Timecode.h"

#include "Xml.h"

namespace zaro::io {
namespace {

using xml::Node;

// --- Rates ------------------------------------------------------------------

/// `xmeml` states a rate as a whole `timebase` plus an `ntsc` flag, and that
/// pair is exactly the two facts a broadcast rate is made of: the number it is
/// called, and whether it is pulled down by 1000/1001. 23.976 is `24` and
/// `TRUE`; 25 is `25` and `FALSE`. Nothing is approximated in either direction,
/// which is the whole reason this format is worth writing to.
void writeRate(Node& parent, const time::Rational& rate) {
    Node& node = parent.add("rate");
    node.add("timebase", time::nominalRate(rate));
    node.addBool("ntsc", rate.den() == 1001);
}

time::Rational readRate(const Node* node, const time::Rational& fallback) {
    if (node == nullptr) {
        return fallback;
    }
    const std::int64_t timebase = node->intOf("timebase", 0);
    if (timebase <= 0) {
        return fallback;
    }
    return node->boolOf("ntsc", false) ? time::Rational{timebase * 1000, 1001}
                                       : time::Rational{timebase, 1};
}

/// The rate a `<rate>` element states, looked for on the node itself.
time::Rational rateOf(const Node& node, const time::Rational& fallback) {
    return readRate(node.child("rate"), fallback);
}

// --- Paths ------------------------------------------------------------------

bool isUnreservedPathChar(char c) noexcept {
    const auto u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '-' || c == '_' || c == '.' || c == '~' || c == '/' ||
           c == '+' || c == ',' || c == '(' || c == ')' || c == '\'' || c == '!' || c == '$' ||
           c == '&' || c == '=' || c == '@' || c == ':';
}

/// A path as `file://localhost/...`, which is the spelling FCP wrote and
/// Premiere still emits. The two-slash form with an empty authority is also
/// read; `pathFromUrl` accepts both because other programs write both.
std::string pathUrl(const std::string& path) {
    std::string out = "file://localhost";
    if (path.empty() || path.front() != '/') {
        // A relative path, or a Windows one. Neither is what a pathurl is meant
        // to carry, but refusing to write the file over it would lose the whole
        // edit for the sake of one clip nobody can relink anyway.
        out += '/';
    }
    for (const char c : path) {
        if (isUnreservedPathChar(c)) {
            out += c;
        } else {
            static constexpr char kHex[] = "0123456789ABCDEF";
            out += '%';
            const auto u = static_cast<unsigned char>(c);
            out += kHex[u >> 4U];
            out += kHex[u & 0x0FU];
        }
    }
    return out;
}

int hexValue(char c) noexcept {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

std::string pathFromUrl(const std::string& url) {
    std::string rest = url;
    if (rest.rfind("file://localhost", 0) == 0) {
        rest = rest.substr(16);
    } else if (rest.rfind("file://", 0) == 0) {
        rest = rest.substr(7);
    }
    std::string out;
    out.reserve(rest.size());
    for (std::size_t i = 0; i < rest.size(); ++i) {
        if (rest[i] == '%' && i + 2 < rest.size()) {
            const int hi = hexValue(rest[i + 1]);
            const int lo = hexValue(rest[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += rest[i];
    }
    return out;
}

std::string fileNameOf(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// --- Writing ----------------------------------------------------------------

void writeTimecode(Node& parent, const time::RationalTime& at, const time::Rational& rate) {
    const time::RationalTime frames =
        at.rate().isPositive() ? at.rescaledTo(rate) : time::RationalTime{0, rate};
    Node& node = parent.add("timecode");
    writeRate(node, rate);
    // Never drop frame. The model has no drop-frame flag -- timecode is a
    // label this program derives, not a field it stores -- and claiming DF on a
    // file that was counted NDF shifts every label after the first minute.
    node.add("string", time::timecodeFromFrames(frames.frames(), rate, false).toString());
    node.add("frame", frames.frames());
    node.add("displayformat", std::string{"NDF"});
    node.add("source", std::string{"source"});
}

/// The `<file>` element for a clip: the full definition the first time this
/// media is mentioned, and a bare reference to that id every time after.
///
/// That is the format's own rule, not a size optimisation. A second full
/// definition under a second id is a second file as far as an importer is
/// concerned, and a timeline where every clip re-declares its media arrives in
/// Premiere as forty copies of the same footage in the bin.
void writeFile(Node& clipItem, const model::MediaRef& media, const time::Rational& sourceRate,
               std::map<std::uint64_t, std::string>& emitted) {
    const auto found = emitted.find(media.id.value());
    if (found != emitted.end()) {
        Node& node = clipItem.add("file");
        node.setAttribute("id", found->second);
        return;
    }
    const std::string id = "file-" + std::to_string(emitted.size() + 1);
    emitted.emplace(media.id.value(), id);

    Node& node = clipItem.add("file");
    node.setAttribute("id", id);
    node.add("name", media.name.empty() ? fileNameOf(media.path) : media.name);
    // The original, never the proxy. A proxy is a local convenience and the
    // path to one is meaningless on the machine this file is going to -- and if
    // it did resolve there, the other program would cut with the small copy.
    node.add("pathurl", pathUrl(media.path));
    writeRate(node, sourceRate);
    const time::RationalTime duration =
        time::RationalTime::fromSeconds(media.info.duration, sourceRate);
    if (duration.frames() > 0) {
        node.add("duration", duration.frames());
    }

    const media::VideoStreamInfo* video = media.info.primaryVideo();
    const media::AudioStreamInfo* audio = media.info.primaryAudio();
    if (video == nullptr && audio == nullptr) {
        // Never probed. Saying nothing is right: Premiere reads the file at the
        // pathurl and finds out for itself, and a guessed `<media>` block would
        // be a claim about somebody else's footage.
        return;
    }
    Node& mediaNode = node.add("media");
    if (video != nullptr) {
        Node& videoNode = mediaNode.add("video");
        Node& characteristics = videoNode.add("samplecharacteristics");
        writeRate(characteristics, sourceRate);
        characteristics.add("width", video->width);
        characteristics.add("height", video->height);
    }
    if (audio != nullptr) {
        Node& audioNode = mediaNode.add("audio");
        Node& characteristics = audioNode.add("samplecharacteristics");
        characteristics.add("depth", std::int64_t{16});
        characteristics.add("samplerate", audio->sampleRate.roundToInt());
        audioNode.add("channelcount", audio->channelCount);
    }
}

void writeClipItem(Node& track, const model::Project& project, const model::Clip& clip,
                   model::TrackKind kind, std::int64_t index,
                   std::map<std::uint64_t, std::string>& emitted) {
    const model::MediaRef* media = project.findMedia(clip.activeSource());
    // The clip's source range carries the rate its media is counted at, which
    // is the rate `<in>` and `<out>` are frames of. For a generated clip there
    // is no media and the two rates are the same by construction.
    const time::Rational sourceRate = clip.sourceRange.start().rate().isPositive()
                                          ? clip.sourceRange.start().rate()
                                          : clip.timelineRange.start().rate();

    Node& item = track.add("clipitem");
    item.setAttribute("id", "clipitem-" + std::to_string(index));
    item.add("name", clip.name);
    item.addBool("enabled", clip.enabled);
    writeRate(item, sourceRate);
    if (media != nullptr) {
        const time::RationalTime duration =
            time::RationalTime::fromSeconds(media->info.duration, sourceRate);
        if (duration.frames() > 0) {
            item.add("duration", duration.frames());
        }
    }
    // Timeline frames. `end` is exclusive in this format as it is in the model,
    // so no adjustment is made and none should be added.
    item.add("start", clip.start().frames());
    item.add("end", clip.endExclusive().frames());
    // Source frames, at the file's rate rather than the sequence's.
    item.add("in", clip.sourceRange.start().frames());
    item.add("out", clip.sourceRange.endExclusive().frames());

    if (media != nullptr) {
        writeFile(item, *media, sourceRate, emitted);
    }
    Node& sourceTrack = item.add("sourcetrack");
    sourceTrack.add("mediatype", std::string{kind == model::TrackKind::Video ? "video" : "audio"});
    sourceTrack.add("trackindex", std::int64_t{1});
}

void writeTracks(Node& parent, const model::Project& project, const model::Sequence& sequence,
                 model::TrackKind kind, std::int64_t& nextItem,
                 std::map<std::uint64_t, std::string>& emitted) {
    const std::vector<model::Track>& tracks =
        kind == model::TrackKind::Video ? sequence.videoTracks() : sequence.audioTracks();
    for (const model::Track& track : tracks) {
        Node& node = parent.add("track");
        for (const model::Clip& clip : track.clips()) {
            writeClipItem(node, project, clip, kind, nextItem++, emitted);
        }
        // After the items, which is where FCP wrote them and where every reader
        // of this format expects to find them.
        node.addBool("enabled", !track.isMuted());
        node.addBool("locked", track.isLocked());
    }
}

void writeMarkers(Node& sequenceNode, const model::Sequence& sequence) {
    for (const model::Marker& marker : sequence.markers()) {
        Node& node = sequenceNode.add("marker");
        node.add("name", marker.name);
        node.add("comment", marker.note);
        node.add("in", marker.range.start().frames());
        // A point marker has no out, and the format spells that -1 rather than
        // repeating the in. Writing them equal makes a zero-length span, which
        // some importers drop.
        node.add("out", marker.isPoint() ? std::int64_t{-1} : marker.range.endExclusive().frames());
    }
}

// --- Reading ----------------------------------------------------------------

/// The first `<sequence>` anywhere in the document, in document order.
///
/// Not simply a child of the root: Premiere wraps its exports in
/// `<project><children>`, other tools write the sequence at the top level, and
/// a bin can sit between the two. Pre-order finds the outermost one either way,
/// which matters because a nested sequence is a `<sequence>` inside a
/// `<clipitem>` and must never be mistaken for the timeline being imported.
const Node* findSequence(const Node& node) {
    if (node.name == "sequence") {
        return &node;
    }
    for (const Node& child : node.children) {
        if (const Node* found = findSequence(child); found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

/// What a `<file>` element resolved to, whether it defined the media or
/// referred to a definition earlier in the document.
struct FileTable {
    std::map<std::string, model::MediaRefId> byId;
    std::map<std::string, time::Rational> rateById;
    std::map<std::string, model::MediaRefId> byPath;
};

/// Resolve a clipitem's `<file>`, creating the media reference if this is where
/// it is defined.
///
/// The two-shapes rule from `writeFile` read backwards: an element with
/// children defines a file, and one with only an `id` refers to a definition
/// that has already gone past. A reference to an id never defined is not an
/// error -- it is a file section this reader skipped, or a malformed export --
/// and the clip is kept without media rather than dropped.
model::MediaRefId resolveFile(const Node& fileNode, model::Project& project, FileTable& table,
                              time::Rational& sourceRate) {
    const std::string id = fileNode.attribute("id");
    if (fileNode.children.empty()) {
        if (const auto rate = table.rateById.find(id); rate != table.rateById.end()) {
            sourceRate = rate->second;
        }
        const auto found = table.byId.find(id);
        return found == table.byId.end() ? model::MediaRefId{} : found->second;
    }

    sourceRate = rateOf(fileNode, sourceRate);
    if (!id.empty()) {
        table.rateById.emplace(id, sourceRate);
    }

    const Node* url = fileNode.child("pathurl");
    const std::string path = url == nullptr ? std::string{} : pathFromUrl(url->text);
    if (path.empty()) {
        // A file with no path: a title or a colour matte generated inside the
        // other program. There is nothing here to point a media reference at.
        return {};
    }

    // Keyed by path as well as by id, because a document that declares the same
    // footage twice under two ids is describing one file, and importing it as
    // two would put two of everything in the bin and break relinking for both.
    if (const auto found = table.byPath.find(path); found != table.byPath.end()) {
        if (!id.empty()) {
            table.byId.emplace(id, found->second);
        }
        return found->second;
    }

    model::MediaRef media;
    media.id = project.ids().next<model::MediaRefTag>();
    media.path = path;
    media.name = fileNode.textOf("name", fileNameOf(path));
    const std::int64_t duration = fileNode.intOf("duration", 0);
    if (duration > 0 && sourceRate.isPositive()) {
        media.info.duration = time::Rational{duration, 1} / sourceRate;
    }
    media.info.path = path;

    if (const Node* mediaNode = fileNode.child("media"); mediaNode != nullptr) {
        if (const Node* video = mediaNode->child("video"); video != nullptr) {
            const Node* characteristics = video->child("samplecharacteristics");
            media::VideoStreamInfo stream;
            stream.frameRate = sourceRate;
            stream.averageFrameRate = sourceRate;
            stream.duration = media.info.duration;
            if (characteristics != nullptr) {
                stream.width = static_cast<std::int32_t>(characteristics->intOf("width", 0));
                stream.height = static_cast<std::int32_t>(characteristics->intOf("height", 0));
            }
            media.info.videoStreams.push_back(stream);
        }
        if (const Node* audio = mediaNode->child("audio"); audio != nullptr) {
            const Node* characteristics = audio->child("samplecharacteristics");
            media::AudioStreamInfo stream;
            stream.duration = media.info.duration;
            stream.channelCount = static_cast<std::int32_t>(audio->intOf("channelcount", 2));
            if (characteristics != nullptr) {
                const std::int64_t sampleRate = characteristics->intOf("samplerate", 48000);
                stream.sampleRate = time::Rational{sampleRate, 1};
            }
            media.info.audioStreams.push_back(stream);
        }
    }

    const model::MediaRefId added = project.addMedia(std::move(media));
    table.byPath.emplace(path, added);
    if (!id.empty()) {
        table.byId.emplace(id, added);
    }
    return added;
}

void readTracks(const Node& parent, model::TrackKind kind, model::Project& project,
                model::Sequence& sequence, const time::Rational& rate, FileTable& table) {
    std::int32_t number = 0;
    for (const Node* trackNode : parent.childrenNamed("track")) {
        ++number;
        const auto trackId = project.ids().next<model::TrackTag>();
        sequence.addTrack(trackId, kind,
                          (kind == model::TrackKind::Video ? "V" : "A") + std::to_string(number));
        model::Track* track = sequence.findTrack(trackId);
        track->setMuted(!trackNode->boolOf("enabled", true));
        track->setLocked(trackNode->boolOf("locked", false));

        for (const Node* item : trackNode->childrenNamed("clipitem")) {
            const std::int64_t start = item->intOf("start", -1);
            const std::int64_t end = item->intOf("end", -1);
            // -1 is the format's way of saying "this item's position is decided
            // by the transition it is inside". There is no transition model to
            // hang it on here, and a clip placed at an invented position would
            // be worse than one left out of a cut that is otherwise right.
            if (start < 0 || end <= start) {
                continue;
            }

            time::Rational sourceRate = rateOf(*item, rate);
            model::MediaRefId source;
            if (const Node* fileNode = item->child("file"); fileNode != nullptr) {
                source = resolveFile(*fileNode, project, table, sourceRate);
            }

            const std::int64_t in = item->intOf("in", 0);
            const std::int64_t out = item->intOf("out", -1);
            // A clipitem with no usable source out runs for as long as it sits
            // on the timeline, which is the only length the file still states.
            const std::int64_t sourceFrames = out > in ? out - in : end - start;

            model::Clip clip;
            clip.id = project.ids().next<model::ClipTag>();
            clip.source = source;
            clip.name = item->textOf("name");
            clip.enabled = item->boolOf("enabled", true);
            clip.sourceRange = time::TimeRange{time::RationalTime{in, sourceRate},
                                               time::RationalTime{sourceFrames, sourceRate}};
            clip.timelineRange = time::TimeRange{time::RationalTime{start, rate},
                                                 time::RationalTime{end - start, rate}};

            // A track in this model may not hold overlapping clips, and nothing
            // stops a file from containing two that do -- a hand-written one, or
            // an exporter with an off-by-one. Skipped rather than asserted on:
            // an import is the one place where the input is somebody else's.
            if (!track->isRangeFree(clip.timelineRange)) {
                continue;
            }
            track->insert(std::move(clip));
        }
    }
}

void readMarkers(const Node& sequenceNode, model::Project& project, model::Sequence& sequence,
                 const time::Rational& rate) {
    std::vector<model::Marker> markers;
    for (const Node* node : sequenceNode.childrenNamed("marker")) {
        const std::int64_t in = node->intOf("in", -1);
        if (in < 0) {
            continue;
        }
        const std::int64_t out = node->intOf("out", -1);
        model::Marker marker;
        marker.id = project.ids().next<model::MarkerTag>();
        marker.name = node->textOf("name");
        marker.note = node->textOf("comment");
        marker.range = time::TimeRange{time::RationalTime{in, rate},
                                       time::RationalTime{out > in ? out - in : 1, rate}};
        markers.push_back(std::move(marker));
    }
    if (!markers.empty()) {
        sequence.setMarkers(std::move(markers));
    }
}

}  // namespace

Result<std::string> writePremiereXml(const model::Project& project, model::SequenceId sequenceId) {
    const model::Sequence* sequence = project.findSequence(sequenceId);
    if (sequence == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }

    Node root;
    root.name = "xmeml";
    root.setAttribute("version", "4");

    Node& sequenceNode = root.add("sequence");
    sequenceNode.setAttribute("id", "sequence-1");
    sequenceNode.add("name", sequence->name());
    sequenceNode.add("duration", sequence->duration().frames());
    writeRate(sequenceNode, sequence->frameRate());
    writeTimecode(sequenceNode, sequence->startTime(), sequence->frameRate());
    sequenceNode.add("in", std::int64_t{-1});
    sequenceNode.add("out", std::int64_t{-1});

    // Built depth first and finished before the next sibling is started: every
    // reference here points into a vector its own parent still owns.
    std::int64_t nextItem = 1;
    std::map<std::uint64_t, std::string> emitted;
    {
        Node& media = sequenceNode.add("media");
        {
            Node& video = media.add("video");
            {
                Node& format = video.add("format");
                Node& characteristics = format.add("samplecharacteristics");
                writeRate(characteristics, sequence->frameRate());
                characteristics.add("width", sequence->width());
                characteristics.add("height", sequence->height());
                characteristics.addBool("anamorphic", false);
                characteristics.add("pixelaspectratio", std::string{"square"});
                characteristics.add("fielddominance", std::string{"none"});
            }
            writeTracks(video, project, *sequence, model::TrackKind::Video, nextItem, emitted);
        }
        {
            Node& audio = media.add("audio");
            {
                Node& format = audio.add("format");
                Node& characteristics = format.add("samplecharacteristics");
                characteristics.add("depth", std::int64_t{16});
                characteristics.add("samplerate", sequence->audioSampleRate().roundToInt());
            }
            writeTracks(audio, project, *sequence, model::TrackKind::Audio, nextItem, emitted);
        }
    }
    writeMarkers(sequenceNode, *sequence);

    return xml::write(root,
                      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<!DOCTYPE xmeml>\n");
}

Result<model::Project> readPremiereXml(const std::string& text) {
    auto root = xml::parse(text);
    if (!root) {
        return root.error();
    }
    if (root->name != "xmeml") {
        return Error{ErrorCode::InvalidData,
                     "the root element is <" + root->name + ">, not <xmeml>"};
    }
    const Node* sequenceNode = findSequence(*root);
    if (sequenceNode == nullptr) {
        return Error{ErrorCode::InvalidData, "the file contains no sequence"};
    }

    model::Project project;
    const time::Rational rate = rateOf(*sequenceNode, time::rates::fps25);
    model::Sequence sequence{project.ids().next<model::SequenceTag>(),
                             sequenceNode->textOf("name", "Sequence"), rate};

    const Node* media = sequenceNode->child("media");
    const Node* video = media == nullptr ? nullptr : media->child("video");
    const Node* audio = media == nullptr ? nullptr : media->child("audio");

    if (video != nullptr) {
        if (const Node* format = video->child("format"); format != nullptr) {
            if (const Node* characteristics = format->child("samplecharacteristics");
                characteristics != nullptr) {
                const auto width = static_cast<std::int32_t>(characteristics->intOf("width", 0));
                const auto height = static_cast<std::int32_t>(characteristics->intOf("height", 0));
                if (width > 0 && height > 0) {
                    sequence.setSize(width, height);
                }
            }
        }
    }
    if (audio != nullptr) {
        if (const Node* format = audio->child("format"); format != nullptr) {
            if (const Node* characteristics = format->child("samplecharacteristics");
                characteristics != nullptr) {
                const std::int64_t sampleRate = characteristics->intOf("samplerate", 0);
                if (sampleRate > 0) {
                    sequence.setAudioSampleRate(time::Rational{sampleRate, 1});
                }
            }
        }
    }

    if (const Node* timecode = sequenceNode->child("timecode"); timecode != nullptr) {
        // The frame count where there is one, and the label otherwise. The two
        // disagree on a drop-frame sequence, and the count is the one that is
        // not a rendering of the other.
        if (timecode->child("frame") != nullptr) {
            sequence.setStartTime(time::RationalTime{timecode->intOf("frame", 0), rate});
        } else if (const Node* label = timecode->child("string"); label != nullptr) {
            if (const auto frames = time::framesFromTimecodeString(label->text, rate)) {
                sequence.setStartTime(time::RationalTime{*frames, rate});
            }
        }
    }

    FileTable table;
    if (video != nullptr) {
        readTracks(*video, model::TrackKind::Video, project, sequence, rate, table);
    }
    if (audio != nullptr) {
        readTracks(*audio, model::TrackKind::Audio, project, sequence, rate, table);
    }
    readMarkers(*sequenceNode, project, sequence, rate);

    // `addSequence` makes the first sequence the active one, which is what a
    // project with exactly one of them wants.
    project.addSequence(std::move(sequence));
    return project;
}

Status savePremiereXml(const model::Project& project, model::SequenceId sequenceId,
                       const std::string& path) {
    auto text = writePremiereXml(project, sequenceId);
    if (!text) {
        return text.error();
    }
    std::ofstream file{path, std::ios::binary};
    if (!file) {
        return Error{ErrorCode::Io, "cannot write " + path};
    }
    file.write(text->data(), static_cast<std::streamsize>(text->size()));
    if (!file) {
        return Error{ErrorCode::Io, "cannot write " + path};
    }
    return {};
}

Result<model::Project> loadPremiereXml(const std::string& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return Error{ErrorCode::NotFound, "cannot open " + path};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return readPremiereXml(buffer.str());
}

}  // namespace zaro::io
