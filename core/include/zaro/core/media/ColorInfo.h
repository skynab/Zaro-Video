#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace zaro::media {

/// Colour is tagged from the moment a frame is decoded and travels with it
/// everywhere. Compositing footage without knowing what its numbers mean is how
/// an editor ends up with washed-out Log clips and green-shifted SD, and
/// retrofitting the tags later means rebuilding the entire pixel pipeline.
enum class ColorPrimaries : std::uint8_t {
    Unknown,
    BT709,      ///< HD, and sRGB's primaries.
    BT601_525,  ///< SMPTE 170M -- NTSC-derived SD.
    BT601_625,  ///< BT.470BG -- PAL-derived SD.
    BT2020,     ///< UHD / HDR.
    DisplayP3,
};

enum class TransferFunction : std::uint8_t {
    Unknown,
    BT709,  ///< The broadcast curve; near gamma 2.4 in practice.
    Gamma22,
    Gamma28,
    SMPTE170M,
    SRGB,
    Linear,
    PQ,   ///< SMPTE ST 2084, HDR.
    HLG,  ///< Hybrid log gamma, HDR.

    // Camera log curves. These are what a camera writes when it is asked to
    // keep its whole range in an 8- or 10-bit file, and a file carrying one is
    // almost never tagged as such -- the container says BT.709 because that is
    // what the container has a number for. `MediaRef::transferOverride` is how
    // somebody says what it really is.
    SLog3,  ///< Sony.
    VLog,   ///< Panasonic.
    LogC3,  ///< Arri, EI 800.
};

/// The matrix used to get from Y'CbCr back to R'G'B'. Getting this wrong does
/// not break the image, it just tints it -- which is why it survives so long
/// unnoticed.
enum class ColorMatrix : std::uint8_t {
    Unknown,
    BT709,
    BT601,
    BT2020NCL,
    SMPTE240M,
    Identity,  ///< Already RGB.
};

/// Whether luma spans 16-235 (limited/video) or 0-255 (full). Mismatching this
/// is the classic crushed-blacks-and-clipped-whites bug.
enum class ColorRange : std::uint8_t { Unknown, Limited, Full };

struct ColorInfo {
    ColorPrimaries primaries{ColorPrimaries::Unknown};
    TransferFunction transfer{TransferFunction::Unknown};
    ColorMatrix matrix{ColorMatrix::Unknown};
    ColorRange range{ColorRange::Unknown};

    [[nodiscard]] bool isFullyTagged() const noexcept {
        return primaries != ColorPrimaries::Unknown && transfer != TransferFunction::Unknown &&
               matrix != ColorMatrix::Unknown && range != ColorRange::Unknown;
    }

    /// Fill in whatever is Unknown using the conventions the industry actually
    /// relies on: untagged SD is BT.601 (525- or 625-line by height), untagged
    /// HD and larger is BT.709, and untagged Y'CbCr is limited range.
    ///
    /// This is a guess and is marked as such by the caller, but it is the same
    /// guess every other tool makes, so making it explicitly and in one place is
    /// better than letting each consumer improvise.
    [[nodiscard]] ColorInfo resolved(std::int32_t width, std::int32_t height) const;

    friend bool operator==(const ColorInfo&, const ColorInfo&) = default;
};

[[nodiscard]] const char* toString(ColorPrimaries v) noexcept;
/// The gamut of that name, or nothing. Names come from project files, which
/// outlive the build that wrote them: a gamut a later version added is dropped
/// rather than refusing to open the project -- the same rule the transfer
/// functions follow below.
[[nodiscard]] bool colorPrimariesFromString(const char* name, ColorPrimaries& out) noexcept;
/// Every gamut, once, so anything that has to offer them all has one list.
[[nodiscard]] std::span<const ColorPrimaries> allColorPrimaries() noexcept;
[[nodiscard]] const char* toString(TransferFunction v) noexcept;
/// The curve of that name, or nothing. Names come from project files, which
/// outlive the build that wrote them: a curve a later version added is dropped
/// rather than refusing to open the project.
[[nodiscard]] bool transferFunctionFromString(const char* name, TransferFunction& out) noexcept;
/// Every curve, once, so anything that has to offer them all has one list.
[[nodiscard]] std::span<const TransferFunction> allTransferFunctions() noexcept;
[[nodiscard]] const char* toString(ColorMatrix v) noexcept;
[[nodiscard]] const char* toString(ColorRange v) noexcept;
[[nodiscard]] std::string toString(const ColorInfo& v);

}  // namespace zaro::media
