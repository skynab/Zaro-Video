#include "zaro/core/io/FinalCutXml.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "PathUrl.h"
#include "Xml.h"

namespace zaro::io {
namespace {

using xml::Node;

/// The version written. 1.9 is the last revision every Final Cut from 10.4.9
/// onward reads, and nothing here needs a word that a later one added -- so it
/// is the version that opens in the most copies of the application.
constexpr const char* kVersion = "1.9";

// --- Time -------------------------------------------------------------------
//
// FCPXML counts in seconds and never in frames. That removes the trap the FCP7
// writer has to keep stepping around -- `<start>` counted at one rate and
// `<in>` at another -- because a second is a second in both the sequence and
// the file. What it adds is that every value must land on a frame boundary,
// which it does here by construction: a frame count over its own rate is exact,
// and Rational does not round.

std::string secondsText(const time::Rational& value) {
    return value.toString() + "s";
}

std::string secondsText(const time::RationalTime& value) {
    return value.rate().isPositive() ? secondsText(value.toSeconds()) : std::string{"0s"};
}

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) {
        text.remove_suffix(1);
    }
    return text;
}

std::optional<time::Rational> parseSeconds(std::string_view text) {
    text = trimmed(text);
    if (!text.empty() && (text.back() == 's' || text.back() == 'S')) {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    return time::Rational::parse(text);
}

time::Rational attributeSeconds(const Node& node, std::string_view name,
                                const time::Rational& fallback) {
    const std::string raw = node.attribute(name);
    if (raw.empty()) {
        return fallback;
    }
    const auto parsed = parseSeconds(raw);
    return parsed ? *parsed : fallback;
}

std::int64_t attributeInt(const Node& node, std::string_view name, std::int64_t fallback) {
    const std::string raw = node.attribute(name);
    std::int64_t out = 0;
    const char* first = raw.data();
    const char* last = first + raw.size();
    const auto parsed = std::from_chars(first, last, out);
    return parsed.ec == std::errc{} && parsed.ptr == last ? out : fallback;
}

/// FCPXML spells its booleans 1 and 0, and leaves them out when they are the
/// default -- so a missing attribute is not false, it is whatever the DTD says.
bool attributeBool(const Node& node, std::string_view name, bool fallback) {
    const std::string raw = node.attribute(name);
    if (raw.empty()) {
        return fallback;
    }
    return raw != "0" && raw != "false";
}

/// A rate stated as the duration of one of its frames, which is how FCPXML
/// states one: 23.976 is `1001/24000s`, exactly. Nothing is approximated in
/// either direction, which is what OTIO's doubles cannot manage.
std::string frameDurationText(const time::Rational& rate) {
    return secondsText(rate.inverse());
}

time::Rational rateFromFrameDuration(const time::Rational& frameDuration,
                                     const time::Rational& fallback) {
    return frameDuration.isPositive() ? frameDuration.inverse() : fallback;
}

/// Final Cut writes a sequence's audio rate as `48k` and an asset's as `48000`,
/// and reads either in either place. The `k` spelling carries one decimal,
/// because 44.1 and 88.2 and 176.4 are the only fractions that occur.
std::string audioRateText(const time::Rational& rate) {
    const std::int64_t hz = rate.roundToInt();
    if (hz <= 0 || hz % 100 != 0) {
        return std::to_string(hz);
    }
    std::string out = std::to_string(hz / 1000);
    if (hz % 1000 != 0) {
        out += '.';
        out += std::to_string((hz % 1000) / 100);
    }
    return out + "k";
}

std::optional<time::Rational> parseAudioRate(std::string_view text) {
    text = trimmed(text);
    const bool kilo = !text.empty() && (text.back() == 'k' || text.back() == 'K');
    if (kilo) {
        text.remove_suffix(1);
    }
    const auto parsed = time::Rational::parse(text);
    if (!parsed || !parsed->isPositive()) {
        return std::nullopt;
    }
    return kilo ? *parsed * time::Rational::fromInt(1000) : *parsed;
}

/// Final Cut names a colour space with the three ITU code points and the name
/// they spell. Only the ones this program can deliver are written; anything
/// else would be a claim about a pipeline it does not have.
std::string colorSpaceText(media::TransferFunction transfer) {
    switch (transfer) {
        case media::TransferFunction::PQ:
            return "9-16-9 (Rec. 2020 PQ)";
        case media::TransferFunction::HLG:
            return "9-18-9 (Rec. 2020 HLG)";
        default:
            return "1-1-1 (Rec. 709)";
    }
}

// --- Roles ------------------------------------------------------------------

const char* audioRoleText(model::AudioRole role) noexcept {
    switch (role) {
        case model::AudioRole::Dialogue:
            return "dialogue";
        case model::AudioRole::Music:
            return "music";
        case model::AudioRole::Effects:
            return "effects";
        case model::AudioRole::Ambience:
            return "ambience";
        case model::AudioRole::Unassigned:
            break;
    }
    return "";
}

/// A Final Cut role may carry a subrole after a dot -- `dialogue.dialogue-1` is
/// one channel of dialogue -- and the subrole is a lane of the same role, not a
/// different one.
model::AudioRole audioRoleFrom(std::string text) {
    text = text.substr(0, text.find('.'));
    for (char& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (text == "dialogue") {
        return model::AudioRole::Dialogue;
    }
    if (text == "music") {
        return model::AudioRole::Music;
    }
    if (text == "effects") {
        return model::AudioRole::Effects;
    }
    if (text == "ambience") {
        return model::AudioRole::Ambience;
    }
    return model::AudioRole::Unassigned;
}

// --- Story elements ---------------------------------------------------------

/// The elements that describe something on a timeline.
///
/// A closed list rather than "anything that is not a marker", because the
/// children of a clip are a mixture of story elements, markers, keywords,
/// filters, adjustments and metadata, and walking into the wrong one would put
/// a colour correction on the timeline as a clip.
///
/// `transition` and `caption` are positioned like clips and are deliberately
/// absent: neither has anywhere to go in this model, and stepping over them is
/// what lets the cut around them survive.
bool isStoryElement(std::string_view name) noexcept {
    return name == "asset-clip" || name == "clip" || name == "gap" || name == "title" ||
           name == "video" || name == "audio" || name == "ref-clip" || name == "mc-clip" ||
           name == "sync-clip" || name == "audition" || name == "spine";
}

// --- Writing ----------------------------------------------------------------

/// Which media a sequence actually reads, in the order it first mentions them,
/// and how far into each one it reaches.
///
/// The order is what the resource ids are handed out in, and reach is the
/// fallback duration for a file nobody has probed: FCPXML has no way to say
/// "as long as it is", and an asset whose stated duration stops short of a
/// clip's out point is one Final Cut refuses to place.
struct MediaUse {
    std::vector<model::MediaRefId> order;
    std::map<std::uint64_t, time::Rational> reach;
};

MediaUse mediaUsedBy(const model::Project& project, const model::Sequence& sequence) {
    MediaUse used;
    for (const model::TrackKind kind : {model::TrackKind::Video, model::TrackKind::Audio}) {
        const std::vector<model::Track>& tracks =
            kind == model::TrackKind::Video ? sequence.videoTracks() : sequence.audioTracks();
        for (const model::Track& track : tracks) {
            for (const model::Clip& clip : track.clips()) {
                const model::MediaRef* media = project.findMedia(clip.activeSource());
                if (media == nullptr) {
                    continue;
                }
                const std::uint64_t key = media->id.value();
                if (used.reach.find(key) == used.reach.end()) {
                    used.order.push_back(media->id);
                }
                const time::Rational end = clip.sourceRange.endExclusive().rate().isPositive()
                                               ? clip.sourceRange.endExclusive().toSeconds()
                                               : time::Rational{};
                time::Rational& reach = used.reach[key];
                if (end > reach) {
                    reach = end;
                }
            }
        }
    }
    return used;
}

/// The `<format>` resources, kept unique.
///
/// Two files at one size and rate are one format, and Final Cut's own exports
/// say so. Writing a second identical one is not wrong, but it is a second
/// entry in a list somebody may have to read, for no fact that was not already
/// stated.
struct FormatWriter {
    std::map<std::string, std::string> byShape;

    /// The id for this shape, and whether it still has to be written out.
    std::pair<std::string, bool> take(const std::string& shape, std::int64_t& next) {
        if (const auto found = byShape.find(shape); found != byShape.end()) {
            return {found->second, false};
        }
        const std::string id = "r" + std::to_string(next++);
        byShape.emplace(shape, id);
        return {id, true};
    }
};

std::string formatShape(const time::Rational& rate, std::int32_t width, std::int32_t height,
                        const std::string& colorSpace) {
    return frameDurationText(rate) + "|" + std::to_string(width) + "x" + std::to_string(height) +
           "|" + colorSpace;
}

void writeFormat(Node& resources, const std::string& id, const time::Rational& rate,
                 std::int32_t width, std::int32_t height, const std::string& colorSpace) {
    Node& node = resources.add("format");
    node.setAttribute("id", id);
    // No `name`. Final Cut's format names are its own identifiers --
    // `FFVideoFormat1080p2398` and a hundred others -- and inventing one that
    // is not on that list is worse than leaving the attribute out: the frame
    // duration and the size are the facts, and the name is a label for them.
    node.setAttribute("frameDuration", frameDurationText(rate));
    if (width > 0 && height > 0) {
        node.setAttribute("width", std::to_string(width));
        node.setAttribute("height", std::to_string(height));
    }
    if (!colorSpace.empty()) {
        node.setAttribute("colorSpace", colorSpace);
    }
}

/// One `<asset>`, plus the `<format>` it needs when the file has picture.
///
/// Declared once, in `<resources>`, and referred to by id after -- which is not
/// an optimisation here but the only shape the format has. There is nowhere
/// else to put a file.
/// Returns the id the asset was declared under. Its format is written first,
/// so nothing in the document refers forward to an id it has not met yet.
std::string writeAsset(Node& resources, const model::MediaRef& media, FormatWriter& formats,
                       std::int64_t& next, const time::Rational& reach) {
    const media::VideoStreamInfo* video = media.info.primaryVideo();
    const media::AudioStreamInfo* audio = media.info.primaryAudio();

    std::string formatId;
    if (video != nullptr) {
        const std::string colorSpace = colorSpaceText(video->color.transfer);
        auto [id, fresh] = formats.take(
            formatShape(video->frameRate, video->width, video->height, colorSpace), next);
        formatId = id;
        if (fresh) {
            writeFormat(resources, formatId, video->frameRate, video->width, video->height,
                        colorSpace);
        }
    }

    const std::string assetId = "r" + std::to_string(next++);
    Node& node = resources.add("asset");
    node.setAttribute("id", assetId);
    node.setAttribute("name", media.name.empty() ? fileNameOf(media.path) : media.name);
    node.setAttribute("start", "0s");
    // As long as the file says, or as long as the cut reaches into it --
    // whichever is longer. FCPXML has no way to say "as long as it is", and an
    // asset whose stated duration stops short of a clip's out point is one
    // Final Cut refuses to place. Where the two disagree the cut is the
    // evidence: something read those frames.
    const time::Rational duration = media.info.duration > reach ? media.info.duration : reach;
    if (duration.isPositive()) {
        node.setAttribute("duration", secondsText(duration));
    }
    // FCPXML has no way to say "not probed yet", and an asset that claims
    // neither picture nor sound is one Final Cut will not place at all. Saying
    // picture is the useful guess: Final Cut opens the file at `src` and finds
    // out for itself, and a clip that arrives is one somebody can fix.
    node.setAttribute("hasVideo", video != nullptr || audio == nullptr ? "1" : "0");
    if (video != nullptr) {
        node.setAttribute("format", formatId);
        node.setAttribute("videoSources", "1");
    }
    if (audio != nullptr) {
        node.setAttribute("hasAudio", "1");
        node.setAttribute("audioSources", "1");
        node.setAttribute("audioChannels", std::to_string(audio->channelCount));
        node.setAttribute("audioRate", std::to_string(audio->sampleRate.roundToInt()));
    }

    Node& rep = node.add("media-rep");
    rep.setAttribute("kind", "original-media");
    // The original, never the proxy. A proxy is a local convenience and the
    // path to one is meaningless on the machine this file is going to -- and if
    // it did resolve there, the other program would cut with the small copy.
    rep.setAttribute("src",
                     "file://" + percentEncodePath(media.path.empty() || media.path.front() == '/'
                                                       ? media.path
                                                       : "/" + media.path));
    return assetId;
}

void writeMarker(Node& parent, const model::Marker& marker, const time::RationalTime& at,
                 const time::RationalTime& oneFrame) {
    Node& node = parent.add("marker");
    node.setAttribute("start", secondsText(at));
    // Final Cut refuses a marker of no length, so a point marker is written a
    // frame long -- and read back as a point, because that is what a one-frame
    // span means to this model too.
    node.setAttribute(
        "duration",
        secondsText(marker.range.duration().frames() > 0 ? marker.range.duration() : oneFrame));
    node.setAttribute("value", marker.name.empty() ? "Marker" : marker.name);
    if (!marker.note.empty()) {
        node.setAttribute("note", marker.note);
    }
    // `completed` at all is what makes a marker a to-do in Final Cut, so it is
    // written only for one that has been dealt with. Writing `completed="0"`
    // for every ordinary marker would turn a timeline's notes into a punch list.
    if (marker.resolved) {
        node.setAttribute("completed", "1");
    }
}

void writeClipItem(Node& parent, const model::Project& project, const model::Clip& clip,
                   model::TrackKind kind, std::int32_t lane, const time::RationalTime& offset,
                   const std::map<std::uint64_t, std::string>& assetIds) {
    const model::MediaRef* media = project.findMedia(clip.activeSource());
    const auto found = media == nullptr ? assetIds.end() : assetIds.find(media->id.value());
    const bool hasAsset = found != assetIds.end();

    // A clip with no file is a gap that carries a name. A nested sequence, an
    // adjustment layer, a title and a shape all land here: the shape of the cut
    // survives and what filled the slot does not, which is the same bargain the
    // FCP7 writer makes and for the same reason -- naming Final Cut's own
    // generators would be guessing at another program's identifiers.
    Node& item = parent.add(hasAsset ? "asset-clip" : "gap");
    if (hasAsset) {
        item.setAttribute("ref", found->second);
    }
    item.setAttribute("lane", std::to_string(lane));
    item.setAttribute("offset", secondsText(offset));
    item.setAttribute("name", clip.name.empty() ? "Clip" : clip.name);
    // Source seconds and timeline seconds, which need no rate to be compared.
    if (hasAsset) {
        item.setAttribute("start", secondsText(clip.sourceRange.start()));
    }
    item.setAttribute("duration", secondsText(clip.timelineRange.duration()));
    if (!clip.enabled) {
        item.setAttribute("enabled", "0");
    }
    // Only on a clip. A gap has no sound to give a role to, and the DTD does
    // not carry the attribute there.
    if (hasAsset && kind == model::TrackKind::Audio) {
        if (const char* role = audioRoleText(clip.role); role[0] != '\0') {
            item.setAttribute("audioRole", role);
        }
    }
}

// --- Reading ----------------------------------------------------------------

/// The `<format>` resources, by id.
struct FormatInfo {
    time::Rational rate;
    std::int32_t width{0};
    std::int32_t height{0};
};

/// An `<asset>` as the document describes it, before it becomes a media
/// reference. It becomes one the first time a clip actually names it: a
/// resource nothing refers to is a bin item in somebody else's library, and
/// importing it would fill this project's bin with files it does not cut with.
struct AssetInfo {
    std::string path;
    std::string name;
    time::Rational duration;
    time::Rational rate;
    std::int32_t width{0};
    std::int32_t height{0};
    bool hasVideo{true};
    bool hasAudio{false};
    std::int32_t channels{2};
    time::Rational sampleRate{time::rates::hz48000};
    model::MediaRefId created;
};

struct Resources {
    std::map<std::string, FormatInfo> formats;
    std::map<std::string, AssetInfo> assets;
    /// Media already made, keyed by path, so a document that declares one file
    /// under two ids does not put two of it in the bin and break relinking for
    /// both.
    std::map<std::string, model::MediaRefId> byPath;
};

Resources readResources(const Node& root, const time::Rational& fallbackRate) {
    Resources out;
    const Node* resources = root.child("resources");
    if (resources == nullptr) {
        return out;
    }
    for (const Node* node : resources->childrenNamed("format")) {
        FormatInfo info;
        info.rate = rateFromFrameDuration(
            attributeSeconds(*node, "frameDuration", time::Rational{}), fallbackRate);
        info.width = static_cast<std::int32_t>(attributeInt(*node, "width", 0));
        info.height = static_cast<std::int32_t>(attributeInt(*node, "height", 0));
        out.formats.emplace(node->attribute("id"), info);
    }
    for (const Node* node : resources->childrenNamed("asset")) {
        AssetInfo info;
        // 1.9 moved the path onto a `<media-rep>` child; before that it was a
        // `src` on the asset itself. Both are read, because both are out there.
        if (const Node* rep = node->child("media-rep"); rep != nullptr) {
            info.path = pathFromUrl(rep->attribute("src"));
        }
        if (info.path.empty()) {
            info.path = pathFromUrl(node->attribute("src"));
        }
        info.name = node->attribute("name");
        info.duration = attributeSeconds(*node, "duration", time::Rational{});
        info.hasVideo = attributeBool(*node, "hasVideo", false);
        info.hasAudio = attributeBool(*node, "hasAudio", false);
        if (!info.hasVideo && !info.hasAudio) {
            // Neither stated. Nothing is known about the file, so nothing is
            // claimed beyond it being placeable.
            info.hasVideo = true;
        }
        info.channels = static_cast<std::int32_t>(attributeInt(*node, "audioChannels", 2));
        if (const auto rate = parseAudioRate(node->attribute("audioRate")); rate) {
            info.sampleRate = *rate;
        }
        info.rate = fallbackRate;
        if (const auto format = out.formats.find(node->attribute("format"));
            format != out.formats.end()) {
            info.rate = format->second.rate;
            info.width = format->second.width;
            info.height = format->second.height;
        }
        out.assets.emplace(node->attribute("id"), info);
    }
    return out;
}

struct FoundSequence {
    const Node* node{nullptr};
    std::string name;
};

/// The first `<sequence>` that is a cut rather than a compound clip.
///
/// Compound clips are `<media><sequence>` and they live in `<resources>`, so
/// that whole subtree is stepped over. A pre-order search that did not would
/// hand back the first compound clip in somebody's library as the timeline
/// being imported, which is the sort of wrong that looks like a right answer.
void findSequence(const Node& node, const std::string& name, FoundSequence& out) {
    if (out.node != nullptr || node.name == "resources") {
        return;
    }
    if (node.name == "sequence") {
        out.node = &node;
        out.name = name;
        return;
    }
    // A sequence has no name of its own; the name a user sees is the project's.
    const std::string here = node.name == "project" ? node.attribute("name") : name;
    for (const Node& child : node.children) {
        findSequence(child, here, out);
    }
}

/// One placed thing, flattened out of the spine and its lanes.
struct Item {
    std::int32_t lane{0};
    time::Rational offset;
    time::Rational duration;
    time::Rational sourceStart;
    std::string ref;
    std::string name;
    model::AudioRole role{model::AudioRole::Unassigned};
    bool enabled{true};
    bool audio{false};
};

struct MarkerItem {
    time::Rational at;
    time::Rational duration;
    std::string name;
    std::string note;
    bool resolved{false};
};

/// Flattens a spine and everything anchored to it into positioned items.
///
/// The recursion carries three things: where the container starts on the
/// timeline, what that container calls its own zero, and which lane it sits
/// in. An anchored child's `offset` is in its parent's local time, so its place
/// on the timeline is the parent's place plus how far past the parent's own
/// start it sits -- and its lane is relative to the parent's, which is how a
/// clip inside a secondary storyline ends up above the storyline rather than
/// above the primary.
class SpineReader {
public:
    explicit SpineReader(const Resources& resources) : resources_{resources} {}

    [[nodiscard]] const std::vector<Item>& items() const noexcept { return items_; }
    [[nodiscard]] const std::vector<MarkerItem>& markers() const noexcept { return markers_; }

    /// `sequential` is the difference between a spine and everything else: a
    /// spine's children follow one another, so one that states no offset starts
    /// where the last one ended. Anchored children each state their own.
    void read(const Node& container, const time::Rational& containerOffset,
              const time::Rational& containerStart, std::int32_t containerLane, bool sequential) {
        time::Rational cursor = containerStart;
        const bool components = container.name == "clip" || container.name == "sync-clip";
        // An audition holds the take that is live and the ones that are not.
        // Only the first is on the timeline; the rest are in the file so that
        // Final Cut can offer them again, and reading them would stack every
        // rejected take on top of the cut.
        const bool onlyFirst = container.name == "audition";
        bool taken = false;

        for (const Node& child : container.children) {
            if (child.name == "marker" || child.name == "chapter-marker") {
                readMarker(child, containerOffset, containerStart);
                continue;
            }
            if (!isStoryElement(child.name) || (onlyFirst && taken)) {
                continue;
            }
            taken = true;
            // A `<video>` or `<audio>` with no lane inside a clip is that
            // clip's own picture or sound, not something anchored to it. Read
            // as an item it would be the same shot placed twice.
            if (components && child.attribute("lane").empty() &&
                (child.name == "video" || child.name == "audio")) {
                continue;
            }
            const time::Rational local =
                attributeSeconds(child, "offset", sequential ? cursor : containerStart);
            const time::Rational at = containerOffset + (local - containerStart);
            const time::Rational duration = attributeSeconds(child, "duration", time::Rational{});
            const auto lane =
                static_cast<std::int32_t>(containerLane + attributeInt(child, "lane", 0));
            if (sequential) {
                cursor = local + duration;
            }

            if (child.name == "audition") {
                read(child, at, time::Rational{}, lane, false);
                continue;
            }
            if (child.name == "spine") {
                // A secondary storyline: anchored where it says, and sequential
                // inside itself from its own zero.
                read(child, at, time::Rational{}, lane, true);
                continue;
            }

            if (child.name != "gap" || lane != 0) {
                if (duration.isPositive()) {
                    items_.push_back(itemFrom(child, lane, at, duration));
                }
            }
            // Into it either way: the primary storyline's gaps are where Final
            // Cut hangs its connected clips, and a gap in the spine is the one
            // place a whole timeline can be anchored.
            read(child, at, attributeSeconds(child, "start", time::Rational{}), lane, false);
        }
    }

private:
    /// The asset a story element reads, which is on the element for the simple
    /// shapes and on a component child for a `<clip>`.
    static std::string refOf(const Node& node, bool& audioOnly) {
        audioOnly = node.name == "audio";
        if (node.name == "clip" || node.name == "sync-clip") {
            for (const Node& child : node.children) {
                if (child.name == "video" && child.attribute("lane").empty()) {
                    audioOnly = false;
                    return child.attribute("ref");
                }
            }
            for (const Node& child : node.children) {
                if (child.name == "audio" && child.attribute("lane").empty()) {
                    audioOnly = true;
                    return child.attribute("ref");
                }
            }
            return {};
        }
        // A title's `ref` names one of Final Cut's generators, not a file, and
        // a compound or multicam clip's names a `<media>` this reader does not
        // unfold. All three keep their place and lose their content.
        if (node.name == "title" || node.name == "ref-clip" || node.name == "mc-clip" ||
            node.name == "gap") {
            return {};
        }
        return node.attribute("ref");
    }

    [[nodiscard]] Item itemFrom(const Node& node, std::int32_t lane, const time::Rational& at,
                                const time::Rational& duration) const {
        Item item;
        item.lane = lane;
        item.offset = at;
        item.duration = duration;
        item.sourceStart = attributeSeconds(node, "start", time::Rational{});
        bool audioOnly = false;
        item.ref = refOf(node, audioOnly);
        item.name = node.attribute("name");
        item.enabled = attributeBool(node, "enabled", true);
        item.role = audioRoleFrom(node.attribute("audioRole"));

        // A lane's sign is what says picture or sound, because that is what it
        // means: Final Cut puts sound below the storyline and nothing else
        // there. Only on the storyline itself is there a question, and there
        // the file answers it.
        if (lane < 0) {
            item.audio = true;
        } else if (lane == 0) {
            if (const auto asset = resources_.assets.find(item.ref);
                asset != resources_.assets.end()) {
                item.audio = asset->second.hasAudio && !asset->second.hasVideo;
            } else {
                item.audio = audioOnly;
            }
        }
        return item;
    }

    void readMarker(const Node& node, const time::Rational& containerOffset,
                    const time::Rational& containerStart) {
        MarkerItem marker;
        marker.at =
            containerOffset + (attributeSeconds(node, "start", containerStart) - containerStart);
        marker.duration = attributeSeconds(node, "duration", time::Rational{});
        marker.name = node.attribute("value");
        marker.note = node.attribute("note");
        marker.resolved = attributeBool(node, "completed", false);
        markers_.push_back(std::move(marker));
    }

    const Resources& resources_;
    std::vector<Item> items_;
    std::vector<MarkerItem> markers_;
};

/// The media reference for an asset id, made the first time it is asked for.
model::MediaRefId mediaFor(const std::string& ref, model::Project& project, Resources& resources) {
    const auto found = resources.assets.find(ref);
    if (found == resources.assets.end()) {
        return {};
    }
    AssetInfo& asset = found->second;
    if (asset.created.isValid()) {
        return asset.created;
    }
    if (asset.path.empty()) {
        return {};
    }
    if (const auto existing = resources.byPath.find(asset.path);
        existing != resources.byPath.end()) {
        asset.created = existing->second;
        return asset.created;
    }

    model::MediaRef media;
    media.id = project.ids().next<model::MediaRefTag>();
    media.path = asset.path;
    media.info.path = asset.path;
    media.name = asset.name.empty() ? fileNameOf(asset.path) : asset.name;
    media.info.duration = asset.duration;
    if (asset.hasVideo) {
        media::VideoStreamInfo video;
        video.width = asset.width;
        video.height = asset.height;
        video.frameRate = asset.rate;
        video.averageFrameRate = asset.rate;
        video.duration = asset.duration;
        media.info.videoStreams.push_back(video);
    }
    if (asset.hasAudio) {
        media::AudioStreamInfo audio;
        audio.channelCount = asset.channels;
        audio.sampleRate = asset.sampleRate;
        audio.duration = asset.duration;
        media.info.audioStreams.push_back(audio);
    }

    asset.created = project.addMedia(std::move(media));
    resources.byPath.emplace(asset.path, asset.created);
    return asset.created;
}

/// The distinct lanes, in the order they should become tracks: picture stacking
/// up from the lowest lane, sound stacking down from the one nearest zero.
std::vector<std::int32_t> lanesOf(const std::vector<Item>& items, bool audio) {
    std::vector<std::int32_t> lanes;
    for (const Item& item : items) {
        if (item.audio == audio &&
            std::find(lanes.begin(), lanes.end(), item.lane) == lanes.end()) {
            lanes.push_back(item.lane);
        }
    }
    std::sort(lanes.begin(), lanes.end());
    if (audio) {
        std::reverse(lanes.begin(), lanes.end());
    }
    return lanes;
}

void buildTracks(const std::vector<Item>& items, bool audio, model::Project& project,
                 model::Sequence& sequence, const time::Rational& rate, Resources& resources) {
    const model::TrackKind kind = audio ? model::TrackKind::Audio : model::TrackKind::Video;
    const std::vector<std::int32_t> lanes = lanesOf(items, audio);

    std::vector<model::TrackId> trackIds;
    trackIds.reserve(lanes.size());
    for (std::size_t i = 0; i < lanes.size(); ++i) {
        const auto id = project.ids().next<model::TrackTag>();
        sequence.addTrack(id, kind, (audio ? "A" : "V") + std::to_string(i + 1));
        trackIds.push_back(id);
    }

    for (const Item& item : items) {
        if (item.audio != audio) {
            continue;
        }
        const auto lane = std::find(lanes.begin(), lanes.end(), item.lane);
        model::Track* track =
            sequence.findTrack(trackIds[static_cast<std::size_t>(lane - lanes.begin())]);

        const model::MediaRefId source = mediaFor(item.ref, project, resources);
        time::Rational sourceRate = rate;
        if (const auto asset = resources.assets.find(item.ref);
            asset != resources.assets.end() && asset->second.rate.isPositive()) {
            sourceRate = asset->second.rate;
        }

        model::Clip clip;
        clip.id = project.ids().next<model::ClipTag>();
        clip.source = source;
        clip.name = item.name;
        clip.enabled = item.enabled;
        clip.role = item.role;
        clip.timelineRange = time::TimeRange{time::RationalTime::fromSeconds(item.offset, rate),
                                             time::RationalTime::fromSeconds(item.duration, rate)};
        // The same span, counted in the file's own frames. Without a `<timeMap>`
        // -- which this reader does not read -- an FCPXML clip consumes exactly
        // as much source as it occupies, so the two durations are one fact.
        clip.sourceRange =
            time::TimeRange{time::RationalTime::fromSeconds(item.sourceStart, sourceRate),
                            time::RationalTime::fromSeconds(item.duration, sourceRate)};

        if (clip.timelineRange.duration().frames() <= 0) {
            continue;
        }
        // A track in this model holds no overlaps, and nothing stops a file
        // from containing two items that do -- a hand-written one, or a lane
        // this reader flattened more eagerly than the writer stacked it.
        // Skipped rather than asserted on: an import is the one place where the
        // input is somebody else's.
        if (!track->isRangeFree(clip.timelineRange)) {
            continue;
        }
        track->insert(std::move(clip));
    }
}

}  // namespace

Result<std::string> writeFcpXml(const model::Project& project, model::SequenceId sequenceId) {
    const model::Sequence* sequence = project.findSequence(sequenceId);
    if (sequence == nullptr) {
        return Error{ErrorCode::NotFound, "no such sequence"};
    }
    const time::Rational rate = sequence->frameRate();
    const time::RationalTime oneFrame{1, rate};
    // Where the timeline starts being counted. FCPXML places everything against
    // the sequence's own timecode, so a cut that starts at 01:00:00:00 has its
    // first clip at `3600s` and not at zero. Writing zero-based offsets under a
    // non-zero tcStart puts the whole cut before the start of the timeline.
    const time::RationalTime start = sequence->startTime().rate().isPositive()
                                         ? sequence->startTime().rescaledTo(rate)
                                         : time::RationalTime{0, rate};

    const MediaUse used = mediaUsedBy(project, *sequence);

    Node root;
    root.name = "fcpxml";
    root.setAttribute("version", kVersion);

    // Resources are finished before the library is begun: `add` returns a
    // reference into its parent's vector, and the next sibling of `resources`
    // would invalidate it.
    std::map<std::uint64_t, std::string> assetIds;
    std::string sequenceFormat;
    {
        Node& resources = root.add("resources");
        const std::string colorSpace = colorSpaceText(sequence->output().transfer);
        FormatWriter formats;
        std::int64_t next = 1;
        // The sequence's own format is the first resource, so a file whose
        // footage matches its timeline -- which is most of them -- declares one
        // format and every asset shares it.
        sequenceFormat =
            formats.take(formatShape(rate, sequence->width(), sequence->height(), colorSpace), next)
                .first;
        writeFormat(resources, sequenceFormat, rate, sequence->width(), sequence->height(),
                    colorSpace);
        for (const model::MediaRefId id : used.order) {
            const model::MediaRef* media = project.findMedia(id);
            if (media == nullptr) {
                continue;
            }
            const auto reach = used.reach.find(id.value());
            assetIds.emplace(id.value(), writeAsset(resources, *media, formats, next,
                                                    reach == used.reach.end() ? time::Rational{}
                                                                              : reach->second));
        }
    }

    {
        Node& library = root.add("library");
        Node& event = library.add("event");
        event.setAttribute("name", "CutReel");
        Node& projectNode = event.add("project");
        projectNode.setAttribute("name", sequence->name());

        Node& sequenceNode = projectNode.add("sequence");
        sequenceNode.setAttribute("format", sequenceFormat);
        const time::RationalTime duration =
            sequence->duration().frames() > 0 ? sequence->duration() : oneFrame;
        sequenceNode.setAttribute("duration", secondsText(duration));
        sequenceNode.setAttribute("tcStart", secondsText(start));
        // Never drop frame. The model has no drop-frame flag -- timecode is a
        // label this program derives, not a field it stores -- and claiming DF
        // on a cut that was counted NDF shifts every label after the minute.
        sequenceNode.setAttribute("tcFormat", "NDF");
        sequenceNode.setAttribute("audioLayout", "stereo");
        sequenceNode.setAttribute("audioRate", audioRateText(sequence->audioSampleRate()));

        Node& spine = sequenceNode.add("spine");
        // One gap the length of the cut, with every track hanging off it. See
        // the header: FCPXML has lanes and no tracks, and anchoring everything
        // to one gap is what keeps each clip's position a fact of its own
        // rather than a consequence of whatever ended up beneath it.
        Node& gap = spine.add("gap");
        gap.setAttribute("name", "Timeline");
        gap.setAttribute("offset", secondsText(start));
        gap.setAttribute("start", secondsText(start));
        gap.setAttribute("duration", secondsText(duration));

        for (const model::TrackKind kind : {model::TrackKind::Video, model::TrackKind::Audio}) {
            const std::vector<model::Track>& tracks =
                kind == model::TrackKind::Video ? sequence->videoTracks() : sequence->audioTracks();
            for (std::size_t i = 0; i < tracks.size(); ++i) {
                const auto lane = static_cast<std::int32_t>(i + 1);
                for (const model::Clip& clip : tracks[i].clips()) {
                    writeClipItem(gap, project, clip, kind,
                                  kind == model::TrackKind::Video ? lane : -lane,
                                  start + clip.start(), assetIds);
                }
            }
        }
        // After the story elements, which is the order the DTD states.
        for (const model::Marker& marker : sequence->markers()) {
            writeMarker(gap, marker, start + marker.range.start(), oneFrame);
        }
    }

    return xml::write(root,
                      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                      "<!DOCTYPE fcpxml>\n");
}

Result<model::Project> readFcpXml(const std::string& text) {
    auto root = xml::parse(text);
    if (!root) {
        return root.error();
    }
    if (root->name != "fcpxml") {
        return Error{ErrorCode::InvalidData,
                     "the root element is <" + root->name + ">, not <fcpxml>"};
    }
    FoundSequence found;
    findSequence(*root, {}, found);
    if (found.node == nullptr) {
        return Error{ErrorCode::InvalidData, "the file contains no sequence"};
    }

    model::Project project;
    Resources resources = readResources(*root, time::rates::fps25);

    time::Rational rate = time::rates::fps25;
    std::int32_t width = 0;
    std::int32_t height = 0;
    if (const auto format = resources.formats.find(found.node->attribute("format"));
        format != resources.formats.end()) {
        if (format->second.rate.isPositive()) {
            rate = format->second.rate;
        }
        width = format->second.width;
        height = format->second.height;
    }

    model::Sequence sequence{project.ids().next<model::SequenceTag>(),
                             found.name.empty() ? "Sequence" : found.name, rate};
    if (width > 0 && height > 0) {
        sequence.setSize(width, height);
    }
    if (const auto audioRate = parseAudioRate(found.node->attribute("audioRate")); audioRate) {
        sequence.setAudioSampleRate(*audioRate);
    }
    const time::Rational tcStart = attributeSeconds(*found.node, "tcStart", time::Rational{});
    sequence.setStartTime(time::RationalTime::fromSeconds(tcStart, rate));

    SpineReader reader{resources};
    if (const Node* spine = found.node->child("spine"); spine != nullptr) {
        reader.read(*spine, time::Rational{}, time::Rational{}, 0, true);
    }

    // Offsets are counted from the sequence's start timecode, so an hour-start
    // cut has its first clip at `3600s`. Taking that back off is what puts the
    // clips where the model counts from -- but only when everything is at or
    // after it. A file whose offsets begin at zero under a non-zero tcStart was
    // written by something that counts the other way, and shifting it would
    // push the whole cut off the front of the timeline.
    std::vector<Item> items = reader.items();
    std::vector<MarkerItem> markers = reader.markers();
    if (tcStart.isPositive()) {
        const bool afterStart = std::all_of(
            items.begin(), items.end(), [&](const Item& item) { return !(item.offset < tcStart); });
        if (afterStart) {
            for (Item& item : items) {
                item.offset -= tcStart;
            }
            for (MarkerItem& marker : markers) {
                marker.at -= tcStart;
            }
        }
    }

    buildTracks(items, false, project, sequence, rate, resources);
    buildTracks(items, true, project, sequence, rate, resources);

    std::vector<model::Marker> built;
    for (const MarkerItem& item : markers) {
        if (item.at.isNegative()) {
            continue;
        }
        model::Marker marker;
        marker.id = project.ids().next<model::MarkerTag>();
        marker.name = item.name;
        marker.note = item.note;
        marker.resolved = item.resolved;
        marker.range = time::TimeRange{time::RationalTime::fromSeconds(item.at, rate),
                                       time::RationalTime::fromSeconds(item.duration, rate)};
        built.push_back(std::move(marker));
    }
    if (!built.empty()) {
        sequence.setMarkers(std::move(built));
    }

    // `addSequence` makes the first sequence the active one, which is what a
    // project with exactly one of them wants.
    project.addSequence(std::move(sequence));
    return project;
}

Status saveFcpXml(const model::Project& project, model::SequenceId sequenceId,
                  const std::string& path) {
    auto text = writeFcpXml(project, sequenceId);
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

Result<model::Project> loadFcpXml(const std::string& path) {
    std::string opening = path;
    // A bundle is a directory with the document inside it. See the header: what
    // a user picks in a file dialog is the `.fcpxmld`, and being told it is not
    // a file would be a puzzle with no clue in it.
    if (opening.size() > 8 && opening.compare(opening.size() - 8, 8, ".fcpxmld") == 0) {
        opening += "/Info.fcpxml";
    }
    std::ifstream file{opening, std::ios::binary};
    if (!file) {
        return Error{ErrorCode::NotFound, "cannot open " + opening};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return readFcpXml(buffer.str());
}

}  // namespace zaro::io
