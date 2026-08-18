#pragma once

#include <cstdint>

namespace zaro::media {

/// The pixel layouts this pipeline handles directly.
///
/// Deliberately far shorter than FFmpeg's list. Everything exotic is converted
/// at the decode boundary, so the rest of the codebase reasons about a handful
/// of layouts instead of a hundred. The set covers what actually arrives from
/// camera and delivery codecs, plus the RGB forms the compositor produces.
enum class PixelFormat : std::uint8_t {
    Unknown,
    // Planar Y'CbCr, 8-bit
    YUV420P,
    YUV422P,
    YUV444P,
    // Planar Y'CbCr, 10-bit in 16-bit containers
    YUV420P10,
    YUV422P10,
    YUV444P10,
    // Semi-planar -- what hardware decoders hand back
    NV12,
    P010,
    // Packed RGB
    RGB24,
    RGBA8,
    // Float RGBA, the compositor's working format
    RGBA16F,
    RGBA32F,
};

struct PixelFormatInfo {
    const char* name;
    std::uint8_t planeCount;
    std::uint8_t bitsPerComponent;
    /// How much narrower chroma is than luma, as a power of two. 1 means half.
    std::uint8_t chromaShiftX;
    std::uint8_t chromaShiftY;
    std::uint8_t componentsPerPlane0;
    bool isPlanarYuv;
    bool hasAlpha;
};

[[nodiscard]] const PixelFormatInfo& info(PixelFormat format) noexcept;
[[nodiscard]] const char* toString(PixelFormat format) noexcept;

/// Bytes in one row of the given plane, before alignment padding.
[[nodiscard]] std::int32_t rowBytes(PixelFormat format, std::int32_t width,
                                    std::int32_t plane) noexcept;
/// Rows in the given plane, accounting for chroma subsampling.
[[nodiscard]] std::int32_t planeHeight(PixelFormat format, std::int32_t height,
                                       std::int32_t plane) noexcept;

}  // namespace zaro::media
