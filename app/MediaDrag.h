// What a drag out of the media pane carries, and how the timeline reads it.
//
// One file so the two ends of the gesture cannot disagree about the format.
// The bin writes it and the timeline reads it; nothing else needs to know how
// it is spelled.
#pragma once

#include <optional>
#include <string>

#include "zaro/core/model/Ids.h"

class QMimeData;

namespace zaro::app {

/// A file being dragged out of the bin: which media, and which part of it.
///
/// Ids rather than a path, because both ends of this drag are inside one
/// project. The timeline has to end up with a clip pointing at the media
/// reference the bin already holds; a path would mean importing the file a
/// second time to get one, and the cut would then be against a different entry
/// than the bin is showing.
struct MediaDrag {
    model::MediaRefId media;
    /// Invalid unless the row dragged was a subclip, in which case the clip
    /// takes the subclip's range instead of the whole file.
    model::SubclipId subclip;

    /// Set instead of `media` when what is being dragged is a title.
    ///
    /// A title has no media to point at -- it generates its picture -- so the
    /// drag carries which preset it came from and the timeline builds the
    /// graphic on landing. One payload for both because it is one gesture: the
    /// pane hands something over, the timeline decides where it goes, and only
    /// the last step differs.
    std::string titlePreset;

    [[nodiscard]] bool isTitle() const noexcept { return !titlePreset.empty(); }
    [[nodiscard]] bool isSomething() const noexcept { return media.isValid() || isTitle(); }
};

/// How a media drag spells itself on the clipboard.
[[nodiscard]] const char* mediaDragMimeType();

/// The payload for one. The caller owns it until Qt takes the drag over.
[[nodiscard]] QMimeData* encodeMediaDrag(const MediaDrag& dragged);

/// What a drag carries, if it is one of ours and names something.
[[nodiscard]] std::optional<MediaDrag> decodeMediaDrag(const QMimeData* mime);

}  // namespace zaro::app
