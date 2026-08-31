#pragma once

#include <cstdint>

namespace zaro::render {

/// How much of each source channel reaches each output channel.
///
/// **This is not cosmetic.** Before it existed the mixer took the first N
/// channels of anything wider than the bus, so a 5.1 file cut into a stereo
/// sequence kept L and R and dropped C, LFE, Ls and Rs -- and in a 5.1 mix the
/// centre channel is the dialogue. The picture and the music arrived and the
/// words did not, silently, with nothing in the interface to say so.
///
/// **The layout is assumed from the channel count**, because a count is all the
/// mixer has: the decoder's buffer carries no layout and threading one through
/// every audio path is a larger change than this bug deserves. The assumption
/// is the conventional order every tool falls back to --
///
///   * 1: mono
///   * 2: L R
///   * 3: L R C
///   * 4: L R Ls Rs
///   * 6: L R C LFE Ls Rs
///   * 8: L R C LFE Ls Rs and a second surround pair
///
/// -- and a count not on that list falls back to taking the first channels,
/// which is where this started and is at least not a regression.
///
/// **The LFE is left out.** It is band-limited rumble meant for a driver the
/// stereo bus does not have, and folding it in adds energy nobody mixed and
/// nothing can reproduce. That is what ATSC A/52 specifies and what every other
/// tool does by default.
class DownmixMatrix {
public:
    DownmixMatrix() = default;
    DownmixMatrix(std::int32_t sourceChannels, std::int32_t destinationChannels);

    /// The weight of source channel `from` in output channel `to`. Zero for
    /// anything out of range, so a caller cannot read past a buffer by asking.
    [[nodiscard]] float weight(std::int32_t to, std::int32_t from) const;

    /// Whether this is the plain pass-through the old code did: each output
    /// taking one input at unity. True for matched layouts and for mono, so the
    /// common paths pay nothing for the general one.
    [[nodiscard]] bool isPassThrough() const noexcept { return passThrough_; }

    [[nodiscard]] std::int32_t sourceChannels() const noexcept { return source_; }
    [[nodiscard]] std::int32_t destinationChannels() const noexcept { return destination_; }

    /// -3 dB, the coefficient ATSC A/52 gives for folding the centre and the
    /// surrounds into a stereo pair.
    static constexpr float kFold = 0.70710678F;

private:
    static constexpr std::int32_t kMaxChannels = 8;
    std::int32_t source_{0};
    std::int32_t destination_{0};
    bool passThrough_{true};
    float weights_[kMaxChannels][kMaxChannels]{};
};

}  // namespace zaro::render
