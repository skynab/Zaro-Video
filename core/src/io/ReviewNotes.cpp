#include "zaro/core/io/ReviewNotes.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "zaro/core/time/Timecode.h"

namespace zaro::io {

std::string reviewNotes(const model::Sequence& sequence, const std::string& title) {
    std::vector<const model::Marker*> comments;
    for (const model::Marker& marker : sequence.markers()) {
        if (marker.isComment()) {
            comments.push_back(&marker);
        }
    }
    std::sort(comments.begin(), comments.end(), [](const model::Marker* a, const model::Marker* b) {
        return a->range.start() < b->range.start();
    });

    std::ostringstream out;
    out << "# " << (title.empty() ? sequence.name() : title) << "\n\n";
    if (comments.empty()) {
        // Said rather than left blank: an empty file looks like the export
        // having failed, and "no comments" is a real and useful answer.
        out << "No review comments.\n";
        return out.str();
    }

    const bool dropFrame = time::supportsDropFrame(sequence.frameRate());
    std::size_t done = 0;
    for (const model::Marker* marker : comments) {
        const time::Timecode at = time::timecodeFromTime(marker->range.start(), dropFrame);
        out << "- **" << at.toString() << "**";
        if (!marker->range.duration().isZero() && !marker->isPoint()) {
            const time::Timecode until =
                time::timecodeFromTime(marker->range.endExclusive(), dropFrame);
            out << "–**" << until.toString() << "**";
        }
        if (!marker->author.empty()) {
            out << " (" << marker->author << ")";
        }
        out << " — ";
        // The name first, because a marker that has one is usually a heading
        // for the note underneath it.
        if (!marker->name.empty()) {
            out << marker->name;
            if (!marker->note.empty()) {
                out << ": ";
            }
        }
        out << marker->note;
        if (marker->resolved) {
            out << "  ✅ done";
            ++done;
        }
        out << "\n";
    }
    out << "\n" << done << " of " << comments.size() << " done.\n";
    return out.str();
}

Status writeReviewNotes(const model::Sequence& sequence, const std::string& path,
                        const std::string& title) {
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    if (!file) {
        return Error{ErrorCode::Io, "cannot write " + path};
    }
    file << reviewNotes(sequence, title);
    file.flush();
    return file ? Status{} : Error{ErrorCode::Io, "failed while writing " + path};
}

}  // namespace zaro::io
