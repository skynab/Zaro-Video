#pragma once

#include <string>

#include "zaro/core/Error.h"
#include "zaro/core/model/Project.h"

namespace zaro::io {

/// Final Cut Pro interchange, as FCPXML.
///
/// **This is not the FCP7 XML that `PremiereXml.h` writes.** The two share a
/// vendor and nothing else. `xmeml` is the format Final Cut Pro 7 wrote and
/// Premiere inherited; FCPXML is what Final Cut Pro has written since version
/// 10, over a different model with different words for everything. Final Cut
/// cannot read `xmeml` and Premiere cannot read `.fcpxml`, so both files have
/// to exist and neither can be produced by adjusting the other.
///
/// **`.fcpproject` is not the deliverable**, for the reason `.prproj` is not:
/// a Final Cut library is a bundle of undocumented binary databases that moves
/// with the application version. FCPXML is the door Apple leaves open — a
/// published DTD, plain text, and what File ▸ Export XML and File ▸ Import ▸
/// XML move a cut through. Version 1.9 is written here, which every Final Cut
/// from 10.4.9 onward reads.
///
/// **Time is seconds, exactly, and never frames.** Every time in the document
/// is a rational number of seconds with an `s` after it: `"1001/24000s"` is one
/// frame of 23.976, and `"3600s"` is an hour. This is the one thing to hold on
/// to before reading `FinalCutXml.cpp` beside the other two writers, because
/// both of those count in frames. Values must land on frame boundaries, which
/// they do here by construction: a frame count divided by its own rate is
/// exact, and `Rational` does not round.
///
/// **A lane is a track.** FCPXML has no tracks. It has a *spine* — the primary
/// storyline — and objects anchored to it at numbered lanes: positive lanes
/// composite above, negative lanes are sound below. The mapping is the obvious
/// one and it is exact: the spine holds a single gap the length of the cut, V1
/// upward becomes lanes 1 upward, and A1 downward becomes lanes -1 downward.
///
/// Writing the tracks into the spine instead would have meant one of them —
/// there is only one spine — and hanging the rest off whichever clip happened
/// to be under them, which is what Final Cut itself does and what makes a
/// connected clip move when the shot beneath it is trimmed. Anchoring
/// everything to one gap is a state Final Cut produces on its own (it is what
/// a project of nothing but connected clips looks like) and it keeps every
/// clip's position a fact rather than a consequence.
///
/// The reader does not assume anything was written by this program: a spine
/// carrying clips at lane 0, connected clips hanging off those, and clips
/// nested inside a compound or a synchronised clip are all read, and the
/// distinct lanes found become tracks in the order they stack.
///
/// **A hole is an absence.** Every anchored item states its own `offset`, so
/// nothing has to stand in for the space between two clips — the same as FCP7
/// XML and the opposite of OTIO. Gaps inside the spine are still read, because
/// Final Cut writes them for its own primary storyline.
///
/// ### What survives a round trip
///
/// Tracks, their order and their stacking; clip positions, source trims, names
/// and enabled state; the media each clip reads, declared once; sequence name,
/// size, frame rate, audio rate and start timecode; markers, including ones
/// Final Cut attached to a clip rather than to the timeline.
///
/// ### What does not
///
/// Grades, effects, masks, keyframes, transitions, audio processing, reversed
/// playback, and track mute and lock — FCPXML has no word for a track, so it
/// has none for a muted one either. Nor does an empty track survive: a lane is
/// where a clip is, so a track with nothing on it has no lane.
///
/// Speed does not cross either, and this is where the two Apple formats part
/// company: `xmeml` states a timeline range and a source range and a clip whose
/// source is twice its timeline is a 200% clip on the way back in, while FCPXML
/// puts a retime in a `<timeMap>` that is not written or read here. A clip that
/// left at 200% arrives at 100%, over the frames it actually played. A nested sequence, an
/// adjustment layer and a multicam clip each write as a positioned item with no media: the shape of
/// the cut survives and what filled that slot does not. Final Cut's effects
/// are named by identifiers belonging to Apple's own plugins, and writing
/// those would be guessing — a grade that arrives wrong is harder to find than
/// one that arrives absent. Effects found on the way in are skipped for the
/// same reason, and the clip they were on is kept.

/// Write one sequence as an FCPXML document.
[[nodiscard]] Result<std::string> writeFcpXml(const model::Project& project,
                                              model::SequenceId sequence);

/// Read an FCPXML document into a project of its own.
///
/// A project rather than a sequence, for the reason `readOtio` and
/// `readPremiereXml` give: the file names media by path, those have to become
/// media references somewhere, and matching them against media already open is
/// a separate decision belonging to whoever is doing the importing.
[[nodiscard]] Result<model::Project> readFcpXml(const std::string& text);

[[nodiscard]] Status saveFcpXml(const model::Project& project, model::SequenceId sequence,
                                const std::string& path);

/// Load from a `.fcpxml` file, or from the `.fcpxmld` bundle around one.
///
/// Final Cut 10.6.6 began writing a bundle — a directory whose `Info.fcpxml`
/// is the document and whose siblings are the stills a title referred to. A
/// path ending `.fcpxmld` is opened as that directory, because what a user
/// picks in a file dialog is the bundle and being told it is not a file would
/// be a puzzle with no clue in it.
[[nodiscard]] Result<model::Project> loadFcpXml(const std::string& path);

}  // namespace zaro::io
