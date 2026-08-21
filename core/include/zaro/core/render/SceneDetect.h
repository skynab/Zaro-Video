#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

#include "zaro/core/render/RgbaImage.h"
#include "zaro/core/time/RationalTime.h"

namespace zaro::render {

/// Where one shot ends and the next begins.
struct SceneCut {
    /// The first frame of the new shot, in whatever time was pushed with it.
    time::RationalTime at;
    /// How different the two shots are, 0 to 1. Reported rather than
    /// thresholded away, so a caller can show the marginal ones differently
    /// from the obvious ones.
    double confidence{0.0};
};

struct SceneDetectOptions {
    /// How much of the picture has to change to count as a cut.
    ///
    /// Measured as the fraction of the colour distribution that moved, so it
    /// means the same thing on a wide shot and a close-up. The default is
    /// deliberately not sensitive: a missed cut is one somebody adds by hand,
    /// and a false one is a clip split in the middle of a take, which is worse
    /// because it looks like an edit they made.
    double threshold{0.45};

    /// No shot shorter than this, including the first one.
    ///
    /// Not a guess about how people cut -- a guard against one transition being
    /// reported as several, and against a cut so close to the beginning that it
    /// is not an edit at all. The clip already starts there; splitting a frame
    /// off the front of it produces a piece nobody asked for, and the material
    /// before a candidate that early is too short to tell a cut from the shot
    /// simply beginning.
    time::RationalTime minimumShot{12, time::Rational{24, 1}};

    /// How far either side of a candidate to look when confirming it.
    ///
    /// **This is what tells a cut from a flash.** A camera flash, a lightning
    /// strike or a white frame differs enormously from the frame before it and
    /// then goes back to looking exactly like it. A cut does not go back.
    ///
    /// So confirmation compares *the shot before with the shot after*, a few
    /// frames either side, rather than the two frames on the boundary. Both
    /// halves matter: without looking forward, a flash is reported going in;
    /// without looking back, it is reported coming out, because the frame
    /// before that boundary is the flash itself and the flash never returns.
    /// Zero disables the check.
    std::int32_t confirmAfter{3};
};

/// Find the cuts in a stream of frames.
///
/// Streaming rather than taking a list: a shot is measured against the frame
/// before it, so nothing needs to hold a whole file of pictures in memory --
/// which at 8 MB a frame is the difference between analysing a feature and
/// running out of address space.
class SceneDetector {
public:
    explicit SceneDetector(SceneDetectOptions options = {}) : options_{std::move(options)} {}

    /// Feed one frame. Frames must arrive in order.
    void push(const RgbaImage& frame, const time::RationalTime& at);

    /// The cuts found so far. Candidates still waiting for confirmation are not
    /// in here, so call `flush` when the frames run out.
    [[nodiscard]] const std::vector<SceneCut>& cuts() const noexcept { return cuts_; }

    /// Decide any candidate still waiting.
    ///
    /// A cut in the last few frames has nothing after it to confirm against.
    /// Accepted rather than dropped: the evidence for it is the same as for any
    /// other, and only the confirmation is missing.
    void flush();

private:
    /// Bins per channel. Sixteen is enough to tell two shots apart and coarse
    /// enough that a pan, which moves every pixel and almost no bin, does not
    /// register.
    static constexpr std::size_t kBins = 16;
    using Histogram = std::array<double, kBins * 3>;

    struct Pending {
        time::RationalTime at;
        double confidence;
        Histogram before;
        std::int32_t framesSeen;
    };

    void accept(const time::RationalTime& at, double confidence);

    SceneDetectOptions options_;
    std::vector<SceneCut> cuts_;
    Histogram previous_{};
    bool hasPrevious_{false};
    /// When the frames started, so the first shot is held to the same minimum
    /// length as every other.
    time::RationalTime firstTime_{};
    std::deque<Pending> pending_;
    /// The last few frames, so a candidate can be measured against the shot it
    /// came out of rather than against the single frame before it.
    std::deque<Histogram> recent_;
};

/// The normalised colour histogram of one frame, exposed because it is the
/// whole measurement and is worth being able to test on its own.
void sceneHistogram(const RgbaImage& frame, std::array<double, 48>& out);

/// How much two histograms differ, 0 (identical) to 1 (no overlap).
[[nodiscard]] double histogramDistance(const std::array<double, 48>& a,
                                       const std::array<double, 48>& b);

}  // namespace zaro::render
