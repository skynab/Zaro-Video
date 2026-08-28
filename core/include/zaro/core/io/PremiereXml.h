#pragma once

#include <string>

#include "zaro/core/Error.h"
#include "zaro/core/model/Project.h"

namespace zaro::io {

/// Adobe Premiere Pro interchange, as FCP7 XML.
///
/// **`.prproj` is not this, and cannot be.** A Premiere project file is a
/// gzipped XML document over an internal, undocumented schema that changes with
/// the application version — it is Premiere's memory written to disk, not a
/// format anything else is meant to read. Reverse engineering one would produce
/// a reader that works against the version it was built from and silently
/// mangles the next, which is worse than not having one.
///
/// What Premiere does document is the door it leaves open: it imports and
/// exports **FCP7 XML** (`xmeml` version 4), and has since it took the format
/// over from Final Cut Pro 7. That is a published, stable, plain-text
/// description of a cut, and it is the file every facility actually moves a
/// timeline on. So `.xml` is the extension here, Premiere's File ▸ Import and
/// File ▸ Export ▸ Final Cut Pro XML are the two ends, and what travels is the
/// edit rather than the application state.
///
/// **A track here states where each clip is, and OTIO's does not.** This is the
/// one structural difference worth knowing before reading `PremiereXml.cpp`
/// beside `OtioIo.cpp`: an `xmeml` `<clipitem>` carries `<start>` and `<end>`
/// on the timeline outright, so holes are holes and need no object to stand for
/// them. Two consequences follow — the writer emits no gaps, and the reader
/// must not assume the items are in order or contiguous.
///
/// **Timelines and sources are counted at different rates.** `<start>` and
/// `<end>` are frames of the sequence; `<in>` and `<out>` are frames of the
/// file, at the rate that file's own `<rate>` states. Conflating them retimes
/// every clip whose media does not match the sequence, which is most of them on
/// any real job.
///
/// ### What survives a round trip
///
/// Tracks, their order, mute and lock; clip positions, source trims, names and
/// enabled state; the media each clip reads; sequence name, size, frame rate
/// and start timecode; sequence markers. Speed changes survive as a
/// consequence of both ranges being written — a clip whose source range is
/// twice its timeline range is a 200% clip on the way back in.
///
/// ### What does not
///
/// Grades, effects, masks, keyframes, transitions, audio processing and
/// reversed playback. Nor do the things this model has and the format has no
/// word for: a nested sequence, an adjustment layer and a multicam clip each
/// write as a positioned item with no media, so the shape of the cut survives
/// and what filled that slot does not. `xmeml` can express some of the rest as
/// `<filter>` elements
/// whose parameter names are Premiere's own; writing them would be guessing at
/// another program's plugin identifiers, and a grade that arrives wrong is
/// harder to find than one that arrives absent. Filters found on the way in are
/// skipped for the same reason, and the clip they were on is kept.

/// Write one sequence as an FCP7 XML document.
[[nodiscard]] Result<std::string> writePremiereXml(const model::Project& project,
                                                   model::SequenceId sequence);

/// Read an FCP7 XML document into a project of its own.
///
/// A project rather than a sequence, for the reason `readOtio` gives: the file
/// names media by path, those have to become media references somewhere, and
/// matching them against media already open is a separate decision belonging to
/// whoever is doing the importing.
[[nodiscard]] Result<model::Project> readPremiereXml(const std::string& text);

[[nodiscard]] Status savePremiereXml(const model::Project& project, model::SequenceId sequence,
                                     const std::string& path);
[[nodiscard]] Result<model::Project> loadPremiereXml(const std::string& path);

}  // namespace zaro::io
