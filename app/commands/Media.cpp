// Where a project's files are: finding them again, gathering them up, and
// making small copies to cut against.
//
// All three change the project's media rather than its cut, which is why they
// end by asking the bin to redraw rather than the monitor. See
// PreviewWindow::afterMediaChange.

#include "Media.h"

#include <filesystem>

#include "zaro/core/io/ProjectIo.h"
#include "zaro/core/io/Relink.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"

namespace zaro::app::commands {

/// Find the project's missing files under a folder and point it at them.
///
/// Everything found is relinked, and what is not found is reported: a
/// dialog per file would be a dialog per file, and the report says exactly
/// which ones were matched only by their name.
Result<io::RelinkReport> relinkMedia(const Context& context, const std::string& root) {
    auto report = io::findRelinks(context.project(), root);
    if (!report) {
        return report;
    }
    for (const io::RelinkMatch& match : report->matches) {
        auto built = edit::makeRelinkMedia(context.project(), match.media, match.found);
        if (!built) {
            continue;  // it went missing again between looking and linking
        }
        context.commands().execute(context.project(), std::move(*built));
    }
    if (!report->matches.empty()) {
    }
    return report;
}

/// Gather the project's media into one folder and point it at the copies.
///
/// The copying and the relinking are one action here and two underneath:
/// the copy is a filesystem change nothing can undo, and the relink is an
/// edit like any other. Undo therefore puts the project back on the
/// originals and leaves the copies where they are, which is the honest
/// half to be able to take back.
Result<io::ConsolidateReport> consolidateMedia(const Context& context,
                                               const std::string& destination) {
    auto report = io::consolidate(context.project(), destination);
    if (!report) {
        return report;
    }
    for (const io::ConsolidatedFile& file : report->files) {
        if (file.alreadyThere) {
            continue;
        }
        auto built = edit::makeRelinkMedia(context.project(), file.media, file.to);
        if (!built) {
            continue;
        }
        context.commands().execute(context.project(), std::move(*built));
    }
    return report;
}

/// Make a proxy for one media reference and attach it.
///
/// Beside the original by default, named after it. Not in a temporary
/// folder: a proxy the system might delete under a project is worse than
/// no proxy, and one nobody can find is one everybody remakes.
Result<platform::ffmpeg::ProxySummary> buildProxy(const Context& context, model::MediaRefId mediaId,
                                                  std::int32_t width) {
    const model::MediaRef* media = context.project().findMedia(mediaId);
    if (media == nullptr) {
        return Error{ErrorCode::NotFound, "no such media in this project"};
    }
    const std::filesystem::path original{media->path};
    platform::ffmpeg::ProxySettings settings;
    settings.source = media->path;
    settings.destination =
        (original.parent_path() / (original.stem().string() + "-proxy.mov")).string();
    settings.width = width;

    auto made = platform::ffmpeg::makeProxy(settings);
    if (!made) {
        return made;
    }
    for (model::MediaRef& entry : context.project().mediaMutable()) {
        if (entry.id == mediaId) {
            entry.proxyPath = made->path;
        }
    }
    // Not switched on by anything here: making one and using one are
    // separate decisions, and somebody who makes proxies for a long import
    // does not necessarily want the picture to change under them now.
    return made;
}

}  // namespace zaro::app::commands
