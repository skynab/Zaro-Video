// Turning a path into a file URL, and back.
//
// Internal to core/src/io. Shared rather than written twice because both XML
// interchange formats carry media as a URL and neither of them may mangle one:
// a path that comes back with its spaces still percent-encoded relinks to
// nothing, and a path written raw makes a document some readers reject.
//
// The *prefix* is not shared, because the two formats do not agree on it. FCP7
// XML writes `file://localhost/...`, which is what Final Cut wrote and what
// Premiere still emits; FCPXML writes `file:///...`. Only the encoding of the
// path itself is common, so only that is here, and each writer spells its own
// scheme.
#pragma once

#include <string>

namespace zaro::io {

/// Percent-encode a path for use in the path part of a file URL.
///
/// Reserved characters that appear in real filenames -- spaces, `#`, `?`, and
/// every byte of a non-ASCII name -- are escaped. Separators and the ordinary
/// punctuation of a filename are not, because a URL nobody can read is harder
/// to relink by hand and no more correct.
[[nodiscard]] std::string percentEncodePath(const std::string& path);

/// The path inside a file URL, decoded.
///
/// Both `file://localhost/...` and the empty-authority `file:///...` are
/// accepted, along with a bare path, because other programs write all three.
/// A `%` that does not begin a valid escape is kept as itself rather than
/// treated as an error: it is a legal character in a filename.
[[nodiscard]] std::string pathFromUrl(const std::string& url);

/// The last component of a path, for naming media that arrived without a name.
[[nodiscard]] std::string fileNameOf(const std::string& path);

}  // namespace zaro::io
