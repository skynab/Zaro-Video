#include "zaro/core/media/VideoFrame.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

// MSVC has never provided std::aligned_alloc: on Windows the block it returns
// could not be handed to free(), which is what the C standard requires of it,
// so the runtime offers _aligned_malloc and a matching _aligned_free instead.
// The two allocators are not interchangeable -- freeing an _aligned_malloc
// block with free() corrupts the heap -- so the deleter below is switched in
// step with the allocation.
#if defined(_MSC_VER)
#include <malloc.h>
#endif

#include "zaro/core/Check.h"

namespace zaro::media {
namespace {

std::size_t alignUp(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

}  // namespace

void VideoFrame::FreeDeleter::operator()(void* p) const noexcept {
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

VideoFrame VideoFrame::allocate(std::int32_t width, std::int32_t height, PixelFormat format,
                                std::int32_t alignment) {
    ZARO_CHECK(width > 0 && height > 0, "VideoFrame::allocate with non-positive dimensions");
    ZARO_CHECK(alignment > 0 && (alignment & (alignment - 1)) == 0,
               "VideoFrame alignment must be a power of two");

    const PixelFormatInfo& fmt = info(format);
    ZARO_CHECK(fmt.planeCount > 0, "VideoFrame::allocate with unknown pixel format");

    VideoFrame frame;
    frame.width_ = width;
    frame.height_ = height;
    frame.format_ = format;
    frame.planeCount_ = fmt.planeCount;

    const auto align = static_cast<std::size_t>(alignment);
    std::array<std::size_t, kMaxPlanes> offsets{};
    std::size_t total = 0;
    for (std::size_t i = 0; i < frame.planeCount_; ++i) {
        const auto index = static_cast<std::int32_t>(i);
        const std::size_t stride =
            alignUp(static_cast<std::size_t>(rowBytes(format, width, index)), align);
        const auto rows = static_cast<std::size_t>(planeHeight(format, height, index));
        frame.strides_[i] = static_cast<std::int32_t>(stride);
        offsets[i] = total;
        total += stride * rows;
    }

    // One allocation for all planes keeps them contiguous, which matters for
    // cache behaviour and lets the whole frame be uploaded to the GPU in a
    // single transfer later.
    frame.byteSize_ = total;
    const std::size_t bytes = alignUp(total, align);
#if defined(_MSC_VER)
    // Note the reversed argument order against std::aligned_alloc.
    auto* raw = static_cast<std::uint8_t*>(_aligned_malloc(bytes, align));
#else
    auto* raw = static_cast<std::uint8_t*>(std::aligned_alloc(align, bytes));
#endif
    ZARO_CHECK(raw != nullptr, "out of memory allocating a video frame");
    frame.storage_.reset(raw);

    for (std::size_t i = 0; i < frame.planeCount_; ++i) {
        frame.planes_[i] = raw + offsets[i];
    }
    return frame;
}

VideoFrame VideoFrame::clone() const {
    if (!isValid()) {
        return {};
    }
    VideoFrame copy = allocate(width_, height_, format_);
    copy.color_ = color_;
    copy.pts_ = pts_;
    copy.pixelAspect_ = pixelAspect_;
    copy.sourceIndex_ = sourceIndex_;
    copy.keyframe_ = keyframe_;

    for (std::size_t i = 0; i < planeCount_; ++i) {
        const auto index = static_cast<std::int32_t>(i);
        const auto rows = static_cast<std::size_t>(planeHeight(format_, height_, index));
        const auto bytes = static_cast<std::size_t>(rowBytes(format_, width_, index));
        for (std::size_t row = 0; row < rows; ++row) {
            std::memcpy(copy.planes_[i] + row * static_cast<std::size_t>(copy.strides_[i]),
                        planes_[i] + row * static_cast<std::size_t>(strides_[i]), bytes);
        }
    }
    return copy;
}

std::uint16_t VideoFrame::sampleAt(std::int32_t x, std::int32_t y, std::size_t planeIndex) const {
    ZARO_CHECK(isValid(), "sampleAt on an invalid frame");
    ZARO_CHECK(planeIndex < planeCount_, "sampleAt on a plane that does not exist");

    const PixelFormatInfo& fmt = info(format_);
    const auto index = static_cast<std::int32_t>(planeIndex);
    const std::int32_t maxX = rowBytes(format_, width_, index) / ((fmt.bitsPerComponent + 7) / 8);
    const std::int32_t maxY = planeHeight(format_, height_, index);
    ZARO_CHECK(x >= 0 && x < maxX && y >= 0 && y < maxY, "sampleAt out of bounds");

    const std::uint8_t* row =
        planes_[planeIndex] +
        static_cast<std::size_t>(y) * static_cast<std::size_t>(strides_[planeIndex]);
    if (fmt.bitsPerComponent <= 8) {
        return row[x];
    }
    std::uint16_t value = 0;
    std::memcpy(&value, row + static_cast<std::size_t>(x) * 2, sizeof(value));
    return value;
}

}  // namespace zaro::media
