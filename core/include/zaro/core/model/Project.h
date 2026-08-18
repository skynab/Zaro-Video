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
    Project() = default;

    [[nodiscard]] const std::vector<MediaRef>& media() const noexcept { return media_; }
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
    std::vector<Sequence> sequences_;
    SequenceId activeSequence_;
    IdGenerator ids_;
};

}  // namespace zaro::model
