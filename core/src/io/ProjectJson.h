// The project file's shape, as one declaration list.
//
// ProjectIo.cpp was 1801 lines: every encoder, every decoder, and the public
// save and load on top of them. The encoders and the decoders never call each
// other -- reading and writing are two independent walks over the same tree --
// so they are two files now, and this is the surface they present to each
// other's callers and to the public functions.
//
// Internal to core/src/io. The overloads have to be declared together in one
// place regardless of where they are defined: `encode` resolves against all of
// them at once, and a decoder that could see only half the set would silently
// pick a different overload.
#pragma once

#include <cstdint>
#include <vector>

#include <nlohmann/json.hpp>

#include "zaro/core/Error.h"
#include "zaro/core/model/Clip.h"
#include "zaro/core/model/ClipEffects.h"
#include "zaro/core/model/Project.h"
#include "zaro/core/model/Sequence.h"
#include "zaro/core/model/Transition.h"
#include "zaro/core/time/RationalTime.h"

namespace zaro::io::detail {

using json = nlohmann::json;

// --- Writing ----------------------------------------------------------------

json encode(const time::Rational& value);
json encode(const time::RationalTime& value);
json encode(const time::TimeRange& value);
json encode(const model::Transform& transform);
json encode(const model::ColorCorrection& color);
json encode(const std::vector<model::Effect>& effects);
json encode(const model::Vignette& vignette);
json encode(const model::ColorWheels& wheels);
json encode(const model::Keyer& keyer);
json encode(const model::Secondary& secondary);
json encode(const model::ToneCurves& curves);
json encode(const model::ClipAnimation& animation);
json encode(const model::Clip& clip);
json encode(const model::Transition& transition);
json encode(const model::Track& track);
json encode(const model::Marker& marker);
json encode(const model::Sequence& sequence);
json encode(const model::Subclip& subclip);
json encode(const model::MediaRef& ref);

/// One curve, as an array of keyframes. Shared by the clip's own animation and
/// by the curves an effect carries, so that the two cannot come to disagree
/// about how a bezier handle is written.
json encodeCurve(const model::Curve& curve);
json encodeCaptions(const model::CaptionTrack& track);

/// Everything the writer does not re-emit, merged back from the document as it
/// was read. A field this version does not know about survives a round trip.
void mergePreserved(json& out, const json& original);

// --- Reading ----------------------------------------------------------------

Result<time::Rational> decodeRational(const json& node, const char* what);
Result<time::RationalTime> decodeTime(const json& node, const char* what);
Result<time::TimeRange> decodeRange(const json& node, const char* what);
model::Transform decodeTransform(const json& node);
model::ColorCorrection decodeColor(const json& node);
Result<model::Curve> decodeCurve(const json& keys);
std::vector<model::Effect> decodeEffects(const json& node);
model::Vignette decodeVignette(const json& node);
model::ColorWheels decodeWheels(const json& node);
model::Keyer decodeKeyer(const json& node);
model::Secondary decodeSecondary(const json& node);
model::ToneCurves decodeCurves(const json& node);
Result<model::ClipAnimation> decodeAnimation(const json& node);
Result<model::Transition> decodeTransition(const json& node);
Result<model::Marker> decodeMarker(const json& node);
Result<model::Subclip> decodeSubclip(const json& node);
Result<model::Clip> decodeClip(const json& node);
Result<model::Track> decodeTrack(const json& node, model::TrackKind kind);
Result<model::Sequence> decodeSequence(const json& node);
Result<model::MediaRef> decodeMedia(const json& node);

/// The largest id the project uses, so the generator resumes past it rather
/// than handing out one that is already taken.
std::uint64_t highestId(const model::Project& project);

}  // namespace zaro::io::detail
