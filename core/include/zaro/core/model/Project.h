#pragma once

#include <string>
#include <vector>

#include "zaro/core/media/MediaInfo.h"
#include "zaro/core/model/Ids.h"
#include "zaro/core/model/Sequence.h"

namespace zaro::model {

/// A file on disk that clips point at.
///
/// The project references media; it never contains it. `contentHash` is what
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
    std::string contentHash;
    std::string name;
    media::MediaInfo info;

    friend bool operator==(const MediaRef& a, const MediaRef& b) {
        // MediaInfo is a cache of what the file said, not part of identity: two
        // refs to the same file are the same ref even if one has not been
        // probed yet.
        return a.id == b.id && a.path == b.path && a.contentHash == b.contentHash &&
               a.name == b.name;
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
        return a.media_ == b.media_ && a.sequences_ == b.sequences_ &&
               a.activeSequence_ == b.activeSequence_;
    }

private:
    std::vector<MediaRef> media_;
    bool useProxies_{false};
    std::vector<Sequence> sequences_;
    SequenceId activeSequence_;
    IdGenerator ids_;
};

}  // namespace zaro::model
