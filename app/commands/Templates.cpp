// Motion graphics templates: a title saved out of one cut and dropped into
// another.

#include "Templates.h"

#include "zaro/core/edit/Operations.h"
#include "zaro/core/io/ProjectIo.h"

namespace zaro::app::commands {

/// Save the selected graphic on its own, so it can be used again.
Status saveGraphicTemplate(const Context& context, const std::string& path) {
    const model::Clip* clip = context.selectedClip();
    if (clip == nullptr) {
        return Error{ErrorCode::InvalidData, "select the title or shape to save"};
    }
    return io::saveGraphicTemplate(*clip, path);
}

/// Drop a saved graphic in at the playhead, on the selected track.
Result<model::ClipId> placeGraphicTemplate(const Context& context, const std::string& path,
                                           const time::RationalTime& duration) {
    const model::Sequence* sequence = context.sequence();
    if (sequence == nullptr) {
        return Error{ErrorCode::InvalidData, "there is no sequence to place it in"};
    }
    const model::TrackId trackId =
        context.track.isValid() ? context.track : sequence->videoTracks().front().id();
    auto loaded = io::loadGraphicTemplate(path);
    if (!loaded) {
        return loaded.error();
    }
    // As long as it was designed to be, unless somebody says otherwise: a
    // template dropped in at some arbitrary length is a template whose
    // timing nobody chose.
    time::RationalTime length = duration;
    if (length.toSecondsDouble() <= 0.0) {
        length = loaded->responsive.authored.toSecondsDouble() > 0.0
                     ? loaded->responsive.authored
                     : loaded->sourceRange.duration();
    }
    const time::TimeRange range{context.position, length.rescaledTo(context.position.rate())};
    auto built = edit::makePlaceGraphicTemplate(context.project(), {sequence->id(), trackId},
                                                *loaded, range);
    if (!built) {
        return built.error();
    }
    context.commands().execute(context.project(), std::move(*built));

    const model::Track* track = context.project().findSequence(sequence->id())->findTrack(trackId);
    for (const model::Clip& candidate : track->clips()) {
        if (candidate.start() == range.start()) {
            return candidate.id;
        }
    }
    return Error{ErrorCode::InvalidData, "the template did not land anywhere"};
}

}  // namespace zaro::app::commands
