#include "zaro/core/edit/Snapping.h"

#include <algorithm>

namespace zaro::edit {
namespace {

struct Candidate {
    time::RationalTime time;
    SnapKind kind;
    model::TrackId track;
};

void collect(const model::Sequence& sequence, model::ClipId ignoring,
             const time::RationalTime* playhead, std::vector<Candidate>& out) {
    out.push_back({time::RationalTime{0, sequence.frameRate()}, SnapKind::SequenceStart, {}});
    if (playhead != nullptr) {
        out.push_back({*playhead, SnapKind::Playhead, {}});
    }
    for (const auto* list : {&sequence.videoTracks(), &sequence.audioTracks()}) {
        for (const model::Track& track : *list) {
            for (const model::Clip& clip : track.clips()) {
                if (clip.id == ignoring) {
                    continue;
                }
                out.push_back({clip.start(), SnapKind::ClipStart, track.id()});
                out.push_back({clip.endExclusive(), SnapKind::ClipEnd, track.id()});
            }
        }
    }
    std::sort(out.begin(), out.end(),
              [](const Candidate& a, const Candidate& b) { return a.time < b.time; });
}

}  // namespace

std::vector<time::RationalTime> snapCandidates(const model::Sequence& sequence,
                                               model::ClipId ignoring) {
    std::vector<Candidate> candidates;
    collect(sequence, ignoring, nullptr, candidates);

    std::vector<time::RationalTime> out;
    out.reserve(candidates.size());
    for (const Candidate& candidate : candidates) {
        if (out.empty() || out.back() != candidate.time) {
            out.push_back(candidate.time);
        }
    }
    return out;
}

SnapResult snapTime(const model::Sequence& sequence, const time::RationalTime& t,
                    const time::RationalTime& threshold, model::ClipId ignoring,
                    const time::RationalTime* playhead) {
    std::vector<Candidate> candidates;
    collect(sequence, ignoring, playhead, candidates);

    SnapResult best{t, SnapKind::None, {}};
    time::RationalTime bestDistance = threshold + time::RationalTime{1, threshold.rate()};

    for (const Candidate& candidate : candidates) {
        const time::RationalTime distance = (candidate.time - t).abs();
        if (distance > threshold) {
            continue;
        }
        // Strictly less, so the first of several equidistant candidates wins
        // and the result stays stable as the pointer moves.
        if (distance < bestDistance) {
            bestDistance = distance;
            best = SnapResult{candidate.time, candidate.kind, candidate.track};
        }
    }
    return best;
}

}  // namespace zaro::edit
