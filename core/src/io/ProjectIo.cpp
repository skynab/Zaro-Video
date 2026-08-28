#include "zaro/core/io/ProjectIo.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

#include "zaro/core/model/ClipEffects.h"
#include "zaro/core/model/Transition.h"
#include "zaro/core/time/Timecode.h"

#include "ProjectJson.h"

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

// Reading and writing live in ProjectJsonEncode.cpp and ProjectJsonDecode.cpp;
// what is left here is the file on disk -- where it goes, how it is replaced,
// and what a version of it is called.
using detail::decodeClip;
using detail::decodeMedia;
using detail::decodeSequence;
using detail::decodeSubclip;
using detail::encode;
using detail::encodeCaptions;
using detail::highestId;
using detail::mergePreserved;

}  // namespace

namespace {

/// FNV-1a over the encoded form. A non-cryptographic hash is the right tool:
/// the inputs are this program's own output, not something an attacker
/// supplies, and 64 bits makes an accidental collision between two versions of
/// one clip a thing that does not happen in a session.
std::uint64_t hashOf(const json& node) {
    const std::string text = node.dump();
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char c : text) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
        hash *= 1099511628211ULL;
    }
    return hash;
}

}  // namespace

std::uint64_t fingerprint(const model::Clip& clip) {
    return hashOf(encode(clip));
}
std::uint64_t fingerprint(const model::Transition& transition) {
    return hashOf(encode(transition));
}
std::uint64_t fingerprint(const model::MediaRef& media) {
    return hashOf(encode(media));
}
std::uint64_t fingerprint(const model::CaptionTrack& captions) {
    return hashOf(encodeCaptions(captions));
}

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
    json subclips = json::array();
    for (const model::Subclip& subclip : project.subclips()) {
        subclips.push_back(encode(subclip));
    }

    json document{{"zaro", {{"schemaVersion", kProjectSchemaVersion}}},
                  {"activeSequence", project.activeSequence().value()},
                  {"media", std::move(media)},
                  {"sequences", std::move(sequences)}};
    if (!subclips.empty()) {
        // Only when there are any: a project that has never had one should not
        // carry an empty list saying so.
        document["subclips"] = std::move(subclips);
    }
    if (project.usingProxies()) {
        // Only when on. A project that has never seen a proxy should not carry
        // a line saying so.
        document["useProxies"] = true;
    }

    if (unknown != nullptr) {
        mergePreserved(document, unknown->document());
    }
    return document.dump(2) + "\n";
}

namespace {

/// Write a file beside the target and rename over it.
///
/// A truncating write destroys the old file the instant it opens it, so a
/// crash, a full disk or a pulled cable partway through leaves neither the old
/// version nor the new -- and the moment somebody is most likely to lose a
/// day's work is the moment they were saving it. Rename within a directory is
/// atomic: either the new file is there whole or the old one still is.
Status writeAtomically(const std::string& path, const std::string& text) {
    std::error_code code;
    const std::filesystem::path target{path};
    std::filesystem::path temporary = target;
    temporary += ".saving";
    {
        std::ofstream file{temporary, std::ios::binary | std::ios::trunc};
        if (!file) {
            return Error{ErrorCode::Io, "cannot open " + temporary.string() + " for writing"};
        }
        file << text;
        file.flush();
        if (!file) {
            std::filesystem::remove(temporary, code);
            return Error{ErrorCode::Io, "failed while writing " + temporary.string()};
        }
    }

    // In the same directory as the target on purpose: a rename across
    // filesystems is a copy and a delete, which is exactly the non-atomic
    // operation this exists to avoid.
    std::filesystem::rename(temporary, target, code);
    if (code) {
        std::filesystem::remove(temporary, code);
        return Error{ErrorCode::Io, "cannot replace " + path + ": " + code.message()};
    }
    return {};
}

}  // namespace

Status saveProject(const model::Project& project, const std::string& path,
                   const std::shared_ptr<const UnknownFields>& unknown) {
    auto text = saveProjectToString(project, unknown);
    if (!text) {
        return text.error();
    }
    return writeAtomically(path, *text);
}

Status saveGraphicTemplate(const model::Clip& clip, const std::string& path) {
    if (!clip.graphic.isSet()) {
        return Error{ErrorCode::InvalidData, "only a title or a shape can be saved as a template"};
    }
    json out{{"zaroTemplate", 1}, {"clip", encode(clip)}};
    return writeAtomically(path, out.dump(2));
}

Result<model::Clip> loadGraphicTemplate(const std::string& path) {
    std::ifstream file{path, std::ios::binary};
    if (!file) {
        return Error{ErrorCode::NotFound, "cannot open " + path};
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();

    json parsed = json::parse(buffer.str(), nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("zaroTemplate")) {
        return Error{ErrorCode::InvalidData, path + " is not a graphic template"};
    }
    if (!parsed.contains("clip") || !parsed.at("clip").is_object()) {
        return Error{ErrorCode::InvalidData, "that template has no graphic in it"};
    }
    auto clip = decodeClip(parsed.at("clip"));
    if (!clip) {
        return clip.error();
    }
    if (!clip->graphic.isSet()) {
        return Error{ErrorCode::InvalidData, "that template has no graphic in it"};
    }
    return clip;
}

namespace {

/// The stem split into its name and its version number, if it has one.
///
/// A version suffix is `_v` followed by digits at the very end. Deliberately
/// strict: `take_v2_final` is a name somebody chose, not version two of
/// `take`, and renumbering it would be this tool having an opinion about
/// their filing.
struct Versioned {
    std::string base;
    int number{0};
    int width{0};
    bool numbered{false};
};

[[nodiscard]] Versioned splitVersion(const std::string& stem) {
    Versioned split;
    split.base = stem;
    std::size_t digits = stem.size();
    while (digits > 0 && std::isdigit(static_cast<unsigned char>(stem[digits - 1])) != 0) {
        --digits;
    }
    if (digits == stem.size() || digits < 2) {
        return split;
    }
    if (stem[digits - 1] != 'v' && stem[digits - 1] != 'V') {
        return split;
    }
    if (stem[digits - 2] != '_' && stem[digits - 2] != '-') {
        return split;
    }
    split.base = stem.substr(0, digits);
    split.width = static_cast<int>(stem.size() - digits);
    split.number = std::stoi(stem.substr(digits));
    split.numbered = true;
    return split;
}

}  // namespace

std::vector<std::string> versionsOf(const std::string& projectPath) {
    const std::filesystem::path path{projectPath};
    const Versioned split = splitVersion(path.stem().string());
    const std::string extension = path.extension().string();

    std::vector<std::pair<int, std::string>> found;
    std::error_code code;
    for (const auto& entry : std::filesystem::directory_iterator{
             path.parent_path().empty() ? std::filesystem::path{"."} : path.parent_path(), code}) {
        if (!entry.is_regular_file(code) || entry.path().extension() != extension) {
            continue;
        }
        const Versioned other = splitVersion(entry.path().stem().string());
        // Same name, whether or not either of them is numbered: the unnumbered
        // file is version one of itself.
        const std::string otherBase = other.numbered ? other.base : other.base + "_v";
        const std::string ourBase = split.numbered ? split.base : split.base + "_v";
        if (otherBase != ourBase) {
            continue;
        }
        found.emplace_back(other.numbered ? other.number : 1, entry.path().string());
    }
    std::sort(found.begin(), found.end());

    std::vector<std::string> paths;
    paths.reserve(found.size());
    for (auto& [number, where] : found) {
        paths.push_back(std::move(where));
    }
    return paths;
}

std::string nextVersionPath(const std::string& projectPath) {
    const std::filesystem::path path{projectPath};
    const Versioned split = splitVersion(path.stem().string());
    const std::string extension = path.extension().string();

    int highest = split.numbered ? split.number : 1;
    for (const std::string& sibling : versionsOf(projectPath)) {
        const Versioned other = splitVersion(std::filesystem::path{sibling}.stem().string());
        highest = std::max(highest, other.numbered ? other.number : 1);
    }

    const int width = split.width > 0 ? split.width : 3;
    std::ostringstream numbered;
    numbered << split.base << (split.numbered ? "" : "_v") << std::setw(width) << std::setfill('0')
             << (highest + 1) << extension;
    return (path.parent_path() / numbered.str()).string();
}

std::string autosavePath(const std::string& projectPath) {
    return projectPath + ".autosave";
}

bool hasNewerAutosave(const std::string& projectPath) {
    std::error_code code;
    const std::filesystem::path recovery{autosavePath(projectPath)};
    if (!std::filesystem::exists(recovery, code) || code) {
        return false;
    }
    const std::filesystem::path project{projectPath};
    if (!std::filesystem::exists(project, code) || code) {
        // No project to compare against -- an autosave of something never
        // saved. That is the case recovery matters most for.
        return true;
    }
    const auto recoveryTime = std::filesystem::last_write_time(recovery, code);
    if (code) {
        return false;
    }
    const auto projectTime = std::filesystem::last_write_time(project, code);
    if (code) {
        return false;
    }
    return recoveryTime > projectTime;
}

Result<LoadedProject> loadProjectFromString(const std::string& text) {
    json document = json::parse(text, nullptr, false);
    if (document.is_discarded()) {
        return Error{ErrorCode::InvalidData, "this is not valid JSON"};
    }
    if (!document.is_object() || !document.contains("zaro")) {
        return Error{ErrorCode::InvalidData, "this is not a CutReel project file"};
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

    loaded.project.setUsingProxies(document.value("useProxies", false));
    loaded.project.setMedia(std::move(media));
    loaded.project.setSequences(std::move(sequences));
    for (const json& node : document.value("subclips", json::array())) {
        auto subclip = decodeSubclip(node);
        if (!subclip) {
            return subclip.error();
        }
        // A subclip of media that is not here describes a range of nothing.
        // Dropped rather than kept: it would show in the bin as something that
        // cannot be opened, which is worse than not showing at all.
        if (loaded.project.findMedia(subclip->source) != nullptr) {
            loaded.project.addSubclip(std::move(*subclip));
        }
    }
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
