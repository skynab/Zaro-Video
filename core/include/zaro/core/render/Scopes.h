#pragma once

#include <cstdint>
#include <vector>

#include "zaro/core/media/VideoFrame.h"
#include "zaro/core/render/RgbaImage.h"

namespace zaro::render {

/// Waveform, parade, histogram and vectorscope for one frame.
///
/// **Scopes measure the display-referred signal, not the working space.** The
/// compositor works in scene-linear light ([ADR-005](docs/adr/0005)), and a
/// waveform of linear values is not the instrument anyone is asking for: middle
/// grey sits at 18 rather than near 50, and every reference a colourist works
/// against — legal range, skin tones around 60–70, the vectorscope's colour
/// targets — is defined on the encoded signal. So every measurement here
/// encodes through the transfer function first.
///
/// The consequence is that a scope reading depends on which transfer function
/// the frame is being *shown* through, which is correct: the instrument
/// measures what is going out, not what is being kept in memory.

enum class ScopeChannel : std::uint8_t { Luma, Red, Green, Blue };

/// A waveform: one column per horizontal position, one row per code value.
///
/// Counts rather than a rendered image, so the same measurement can be drawn at
/// any size, in any style, without being recomputed. Drawing is a UI decision;
/// this is the measurement.
class Waveform {
public:
    static constexpr std::int32_t kLevels = 256;

    Waveform() = default;
    Waveform(std::int32_t columns, ScopeChannel channel);

    [[nodiscard]] std::int32_t columns() const noexcept { return columns_; }
    [[nodiscard]] ScopeChannel channel() const noexcept { return channel_; }
    [[nodiscard]] bool isValid() const noexcept { return columns_ > 0; }

    /// How many pixels in `column` had this code value. Level 0 is black and
    /// `kLevels - 1` is white, so a caller drawing it downwards has to flip:
    /// the measurement is in signal order, not screen order.
    [[nodiscard]] std::uint32_t at(std::int32_t column, std::int32_t level) const;
    void add(std::int32_t column, std::int32_t level);

    /// The largest count in any cell, for scaling a drawing to fit.
    [[nodiscard]] std::uint32_t peak() const noexcept { return peak_; }

private:
    std::int32_t columns_{0};
    ScopeChannel channel_{ScopeChannel::Luma};
    std::uint32_t peak_{0};
    std::vector<std::uint32_t> cells_;
};

/// Code-value distribution over the whole frame.
struct Histogram {
    static constexpr std::int32_t kBins = 256;

    std::vector<std::uint32_t> red;
    std::vector<std::uint32_t> green;
    std::vector<std::uint32_t> blue;
    std::vector<std::uint32_t> luma;
    /// The largest count in any bin of any channel.
    std::uint32_t peak{0};

    [[nodiscard]] bool isValid() const noexcept { return red.size() == kBins; }
};

/// Chroma distribution on the Cb/Cr plane.
///
/// Square and centred: (size/2, size/2) is neutral, and distance from there is
/// saturation. Stored as counts for the same reason as the waveform.
class Vectorscope {
public:
    Vectorscope() = default;
    explicit Vectorscope(std::int32_t size);

    [[nodiscard]] std::int32_t size() const noexcept { return size_; }
    [[nodiscard]] bool isValid() const noexcept { return size_ > 0; }
    [[nodiscard]] std::uint32_t at(std::int32_t x, std::int32_t y) const;
    void add(std::int32_t x, std::int32_t y);
    [[nodiscard]] std::uint32_t peak() const noexcept { return peak_; }

    /// Where a colour lands, so a caller can draw the graticule targets with
    /// exactly the arithmetic the measurement uses rather than a copy of it.
    static void plotFor(float r, float g, float b, std::int32_t size, float& x, float& y);

private:
    std::int32_t size_{0};
    std::uint32_t peak_{0};
    std::vector<std::uint32_t> cells_;
};

/// Every measurement of one frame, computed in a single pass.
///
/// One pass because the expensive part is the transfer-function encode, and
/// four separate functions would pay it four times over.
struct FrameScopes {
    Waveform luma;
    Waveform red;
    Waveform green;
    Waveform blue;
    Histogram histogram;
    Vectorscope vectorscope;
};

struct ScopeOptions {
    /// Waveform columns. Fewer than the frame's width is the normal case: the
    /// panel is narrower than the picture, and measuring at the panel's
    /// resolution is what keeps this cheap enough to run while scrubbing.
    std::int32_t waveformColumns{256};
    std::int32_t vectorscopeSize{256};
    media::TransferFunction transfer{media::TransferFunction::BT709};
    /// Sample every Nth row. 1 measures everything; larger trades a little
    /// noise in the counts for a proportional speed-up, and the shape of a
    /// waveform is unchanged by measuring half its rows.
    std::int32_t rowStride{1};
};

[[nodiscard]] FrameScopes measure(const RgbaImage& frame, const ScopeOptions& options = {});

}  // namespace zaro::render
