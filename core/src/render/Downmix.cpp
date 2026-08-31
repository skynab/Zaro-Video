#include "zaro/core/render/Downmix.h"

#include <algorithm>

namespace zaro::render {
namespace {

/// Whether a channel count is one whose conventional order we are willing to
/// assume. Anything else is folded the old way rather than guessed at: putting
/// a five-channel file's third channel somewhere on the strength of a guess is
/// worse than the honest pass-through, because it cannot be heard as wrong.
bool known(std::int32_t channels) {
    return channels == 1 || channels == 2 || channels == 3 || channels == 4 || channels == 6 ||
           channels == 8;
}

/// Which side of the image a source channel belongs to, in the conventional
/// order. -1 left, 1 right, 0 centre, and 2 for the LFE, which goes nowhere.
std::int32_t sideOf(std::int32_t channels, std::int32_t index) {
    if (channels <= 2) {
        return index == 0 ? -1 : 1;
    }
    if (channels == 3) {  // L R C
        return index == 0 ? -1 : index == 1 ? 1 : 0;
    }
    if (channels == 4) {  // L R Ls Rs
        return (index % 2) == 0 ? -1 : 1;
    }
    // 6 and 8: L R C LFE Ls Rs [Lb Rb]
    switch (index) {
        case 0:
            return -1;
        case 1:
            return 1;
        case 2:
            return 0;
        case 3:
            return 2;
        default:
            return (index % 2) == 0 ? -1 : 1;
    }
}

/// Whether a source channel is a front left or right, which arrives at unity
/// rather than folded.
bool isFront(std::int32_t channels, std::int32_t index) {
    return index < 2 || channels <= 2;
}

}  // namespace

DownmixMatrix::DownmixMatrix(std::int32_t sourceChannels, std::int32_t destinationChannels)
    : source_{sourceChannels}, destination_{destinationChannels} {
    if (source_ <= 0 || destination_ <= 0) {
        return;
    }
    const std::int32_t rows = std::min(destination_, kMaxChannels);
    const std::int32_t columns = std::min(source_, kMaxChannels);

    // The cases the old code got right, kept exactly: a matched layout passes
    // through, and a mono source feeds every output. Both stay pass-through so
    // the common paths do not start paying for a matrix.
    if (source_ == destination_ || source_ == 1 || !known(source_) || destination_ > 2) {
        for (std::int32_t to = 0; to < rows; ++to) {
            const std::int32_t from = std::min(to, columns - 1);
            weights_[to][from] = 1.0F;
        }
        passThrough_ = true;
        return;
    }
    passThrough_ = false;

    if (destination_ == 1) {
        // Everything to one output. Half rather than -3 dB: a mono fold sums
        // correlated material, and equal-power weights on a centred mix come
        // back louder than the stereo it was folded from.
        for (std::int32_t from = 0; from < columns; ++from) {
            if (sideOf(source_, from) == 2) {
                continue;  // the LFE, which is not folded in
            }
            weights_[0][from] = isFront(source_, from) ? 0.5F : (0.5F * kFold);
        }
        return;
    }

    // To stereo: fronts at unity, centre to both, surrounds to their own side,
    // all folded at -3 dB. ATSC A/52's default, and what every other tool does.
    for (std::int32_t from = 0; from < columns; ++from) {
        const std::int32_t side = sideOf(source_, from);
        if (side == 2) {
            continue;
        }
        if (isFront(source_, from)) {
            weights_[side < 0 ? 0 : 1][from] = 1.0F;
        } else if (side == 0) {
            weights_[0][from] = kFold;
            weights_[1][from] = kFold;
        } else {
            weights_[side < 0 ? 0 : 1][from] = kFold;
        }
    }
}

float DownmixMatrix::weight(std::int32_t to, std::int32_t from) const {
    if (to < 0 || from < 0 || to >= std::min(destination_, kMaxChannels) ||
        from >= std::min(source_, kMaxChannels)) {
        return 0.0F;
    }
    return weights_[to][from];
}

}  // namespace zaro::render
