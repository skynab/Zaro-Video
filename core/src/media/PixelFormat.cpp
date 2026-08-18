#include "zaro/core/media/PixelFormat.h"

#include <array>

namespace zaro::media {
namespace {

// name, planes, bits, shiftX, shiftY, componentsInPlane0, planarYuv, alpha
constexpr std::array<PixelFormatInfo, 13> kFormats{{
    {"unknown", 0, 0, 0, 0, 0, false, false},
    {"yuv420p", 3, 8, 1, 1, 1, true, false},
    {"yuv422p", 3, 8, 1, 0, 1, true, false},
    {"yuv444p", 3, 8, 0, 0, 1, true, false},
    {"yuv420p10", 3, 10, 1, 1, 1, true, false},
    {"yuv422p10", 3, 10, 1, 0, 1, true, false},
    {"yuv444p10", 3, 10, 0, 0, 1, true, false},
    {"nv12", 2, 8, 1, 1, 1, true, false},
    {"p010", 2, 10, 1, 1, 1, true, false},
    {"rgb24", 1, 8, 0, 0, 3, false, false},
    {"rgba8", 1, 8, 0, 0, 4, false, true},
    {"rgba16f", 1, 16, 0, 0, 4, false, true},
    {"rgba32f", 1, 32, 0, 0, 4, false, true},
}};

}  // namespace

const PixelFormatInfo& info(PixelFormat format) noexcept {
    const auto index = static_cast<std::size_t>(format);
    return index < kFormats.size() ? kFormats[index] : kFormats[0];
}

const char* toString(PixelFormat format) noexcept {
    return info(format).name;
}

std::int32_t rowBytes(PixelFormat format, std::int32_t width, std::int32_t plane) noexcept {
    const PixelFormatInfo& f = info(format);
    if (plane >= f.planeCount || width <= 0) {
        return 0;
    }
    const std::int32_t bytesPerComponent = (f.bitsPerComponent + 7) / 8;

    if (!f.isPlanarYuv) {
        return width * f.componentsPerPlane0 * bytesPerComponent;
    }
    if (plane == 0) {
        return width * bytesPerComponent;
    }
    // Chroma planes are subsampled horizontally; NV12/P010 interleave Cb and Cr
    // into a single plane, which cancels out the horizontal halving.
    const std::int32_t chromaWidth = (width + (1 << f.chromaShiftX) - 1) >> f.chromaShiftX;
    const std::int32_t componentsInterleaved = (f.planeCount == 2) ? 2 : 1;
    return chromaWidth * componentsInterleaved * bytesPerComponent;
}

std::int32_t planeHeight(PixelFormat format, std::int32_t height, std::int32_t plane) noexcept {
    const PixelFormatInfo& f = info(format);
    if (plane >= f.planeCount || height <= 0) {
        return 0;
    }
    if (plane == 0 || !f.isPlanarYuv) {
        return height;
    }
    return (height + (1 << f.chromaShiftY) - 1) >> f.chromaShiftY;
}

}  // namespace zaro::media
