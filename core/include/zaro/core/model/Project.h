#pragma once

#include <string>
#include <vector>

#include "zaro/core/media/MediaInfo.h"
#include "zaro/core/model/Ids.h"
#include "zaro/core/model/Sequence.h"

namespace zaro::model {

/// A named range of a media reference.
///
/// **Not a new kind of media.** A subclip records where somebody said the good
/// part is; placing one makes an ordinary clip whose source range starts inside
/// it, and from there the renderer, the media source, the cache and every edit
/// operation carry on knowing nothing about subclips at all. Making it a media
/// reference with an offset would mean every read had to be translated, in a
/// layer that currently resolves a path and nothing else.
///
/// **So a clip made from one can be trimmed past its edges.** Premiere can
/// restrict those trims; doing that here would need a second kind of clip that
/// every trim, ripple, roll, slip and slide had to learn about, to enforce a
/// boundary somebody chose as a note to themselves. The subclip stays in the
/// bin as that note, and the cut is not constrained by it.
struct Subclip {
    SubclipId id;
    MediaRefId source;
    /// In the source's own time.
    time::TimeRange range;
    std::string name;

    friend bool operator==(const Subclip&, const Subclip&) = default;
};

/// A file on disk that clips point at.
///
/// The project references media; it never contains it. `contentDigest` is what
/// makes relinking possible when a path changes, and what tells a proxy from
/// the thing it stands in for.
struct MediaRef {
    MediaRefId id;
    std::string path;

    /// A smaller copy of the same material, for editing.
    ///
    /// Attached rather than generated: making one is a transcode, and a
    /// transcode belongs to whatever tool the footage came out of. What matters
    /// here is that the two files describe the same thing -- same duration,
    /// same rate -- so that swapping between them moves no edit. A proxy of a
    /// different length would silently retime the cut.
    std::string proxyPath;
    /// A cache key: changes when the file is touched, which is what a cache
    /// wants and what a relink must not have.
    std::string contentHash;
    /// An identity that survives being copied or restored, for finding this
    /// file again when it has moved. See `media::contentDigest`.
    std::string contentDigest;

    /// Whatever somebody wants to say about this file: which camera, which
    /// take, whether it is any good.
    ///
    /// One free-text field rather than a schema of named fields. A schema is a
    /// guess about what a production tracks, and productions that track
    /// something it did not think of end up putting it in a "notes" field
    /// anyway -- which is this one, without the two places to look.
    std::string notes;
    std::string name;
    media::MediaInfo info;

    /// What this footage's curve really is, when the file is wrong about it.
    ///
    /// Camera log is almost never tagged: a container has a number for BT.709
    /// and no number for S-Log3, so an S-Log3 file says BT.709 and decodes to a
    /// washed-out picture that grades badly and looks, at a glance, like
    /// footage somebody underexposed. There is no way to detect it from the
    /// pixels -- a flat shot and a log shot are the same picture -- so the only
    /// honest mechanism is somebody saying so.
    ///
    /// On the media rather than the clip: it is a fact about the file, and
    /// every clip that reads that file needs the same answer. `Unknown` means
    /// believe the container.
    media::TransferFunction transferOverride{media::TransferFunction::Unknown};

    /// The curve to decode this file through: the override if there is one,
    /// otherwise whatever the container said.
    [[nodiscard]] media::TransferFunction transfer() const {
        if (transferOverride != media::TransferFunction::Unknown) {
            return transferOverride;
        }
        const media::VideoStreamInfo* video = info.primaryVideo();
        return video != nullptr ? video->color.transfer : media::TransferFunction::Unknown;
    }

    friend bool operator==(const MediaRef& a, const MediaRef& b) {
        // MediaInfo is a cache of what the file said, not part of identity: two
        // refs to the same file are the same ref even if one has not been
        // probed yet.
        return a.id == b.id && a.path == b.path && a.contentHash == b.contentHash &&
               a.notes == b.notes && a.contentDigest == b.contentDigest && a.name == b.name &&
               a.transferOverride == b.transferOverride;
    }
};

class Project {
public:
    /// Whether to read proxies where a clip's media has one.
    ///
    /// A property of the project rather than of a clip: nobody wants some shots
    /// on proxies and some not, and the whole reason to be on proxies is that
    /// the machine cannot keep up with the originals.
    ///
    /// **Export ignores this.** Delivering the small copies because somebody
    /// left a toggle on is a mistake with no warning attached and no way back
    /// once the file has gone out.
    [[nodiscard]] bool usingProxies() const noexcept { return useProxies_; }
    void setUsingProxies(bool value) noexcept { useProxies_ = value; }

    /// The file to read for this media: its proxy when one is attached and
    /// proxies are on, its own path otherwise.
    [[nodiscard]] const std::string& resolvedPath(const MediaRef& media) const {
        return useProxies_ && !media.proxyPath.empty() ? media.proxyPath : media.path;
    }

    Project() = default;

    [[nodiscard]] const std::vector<MediaRef>& media() const noexcept { return media_; }
    [[nodiscard]] const std::vector<Subclip>& subclips() const noexcept { return subclips_; }
    [[nodiscard]] const Subclip* findSubclip(SubclipId id) const;
    /// Added directly rather than through a command, like media: the bin is not
    /// the cut, and nothing about a subclip changes what any sequence renders.
    SubclipId addSubclip(Subclip subclip);
    bool removeSubclip(SubclipId id);
    /// Mutable access, for the few things that change a media reference in
    /// place -- attaching a proxy, relinking a moved file. Not for editing:
    /// anything that changes the cut goes through a command.
    /// Whether putting `inner` inside `outer` would make a cycle.
    ///
    /// A sequence containing itself, directly or through any chain of nests, is
    /// not a picture -- it is a render that never finishes. This is checked
    /// before the edit rather than guarded against during the render, because a
    /// depth limit turns an impossible project into a merely wrong one, and the
    /// person who made it gets no explanation either way.
    [[nodiscard]] bool nestingWouldCycle(SequenceId outer, SequenceId inner) const;

    [[nodiscard]] std::vector<MediaRef>& mediaMutable() noexcept { return media_; }
    [[nodiscard]] const std::vector<Sequence>& sequences() const noexcept { return sequences_; }

    [[nodiscard]] const MediaRef* findMedia(MediaRefId id) const;
    [[nodiscard]] Sequence* findSequence(SequenceId id);
    [[nodiscard]] const Sequence* findSequence(SequenceId id) const;

    MediaRefId addMedia(MediaRef ref);
    SequenceId addSequence(Sequence sequence);

    [[nodiscard]] IdGenerator& ids() noexcept { return ids_; }
    [[nodiscard]] const IdGenerator& ids() const noexcept { return ids_; }

    [[nodiscard]] SequenceId activeSequence() const noexcept { return activeSequence_; }
    void setActiveSequence(SequenceId id) noexcept { activeSequence_ = id; }

    /// Fields a loader needs to restore verbatim.
    void setMedia(std::vector<MediaRef> value) { media_ = std::move(value); }
    void setSequences(std::vector<Sequence> value) { sequences_ = std::move(value); }

    friend bool operator==(const Project& a, const Project& b) {
        // The id counter is bookkeeping, not content: a project loaded from
        // disk and one built in memory are equal if they describe the same cut.
        return a.media_ == b.media_ && a.subclips_ == b.subclips_ && a.sequences_ == b.sequences_ &&
               a.activeSequence_ == b.activeSequence_;
    }

private:
    std::vector<MediaRef> media_;
    std::vector<Subclip> subclips_;
    bool useProxies_{false};
    std::vector<Sequence> sequences_;
    SequenceId activeSequence_;
    IdGenerator ids_;
};

/// A project with one empty sequence, ready to be edited into.
///
/// One sequence rather than none: a window with nothing to show has to special
/// case every panel, and "no sequence" is a state somebody can only leave by
/// making one anyway.
///
/// The rate and size are a placeholder, and are meant to be replaced by the
/// first thing put on the timeline -- see `edit::makeConformSequence`. Asking
/// somebody to choose a format before they have opened any footage is asking a
/// question whose answer is in the footage.
[[nodiscard]] Project newProject(const std::string& sequenceName = "Sequence 01");

}  // namespace zaro::model
