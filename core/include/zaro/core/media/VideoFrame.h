#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "zaro/core/media/ColorInfo.h"
#include "zaro/core/media/PixelFormat.h"
#include "zaro/core/time/RationalTime.h"

namespace zaro::media {

inline constexpr std::size_t kMaxPlanes = 4;

/// One decoded picture, owning its pixels on the CPU.
///
/// Frames are moved, never copied: a 4K RGBA float frame is about 32 MB, and an
/// accidental copy in a per-frame path is the difference between realtime and
/// not. The copy constructor is deleted for exactly that reason; `clone()` makes
/// the cost explicit where it is genuinely wanted.
///
/// Phase 3 will add a GPU-resident variant. Hardware-decoded frames currently
/// get downloaded to system memory at the decode boundary, which is the one
/// unnecessary round trip in this design and is deliberate for now -- it keeps
/// the whole pipeline uniform while there is no compositor to hand a texture to.
class VideoFrame {
public:
    VideoFrame() = default;

    VideoFrame(const VideoFrame&) = delete;
    VideoFrame& operator=(const VideoFrame&) = delete;
    VideoFrame(VideoFrame&&) noexcept = default;
    VideoFrame& operator=(VideoFrame&&) noexcept = default;
    ~VideoFrame() = default;

    /// Allocate an uninitialised frame. Rows are padded to `alignment` bytes so
    /// plane starts stay suitable for SIMD and for GPU upload.
    [[nodiscard]] static VideoFrame allocate(std::int32_t width, std::int32_t height,
                                             PixelFormat format, std::int32_t alignment = 64);

    [[nodiscard]] bool isValid() const noexcept { return storage_ != nullptr && width_ > 0; }

    [[nodiscard]] std::int32_t width() const noexcept { return width_; }
    [[nodiscard]] std::int32_t height() const noexcept { return height_; }
    [[nodiscard]] PixelFormat format() const noexcept { return format_; }

    [[nodiscard]] const ColorInfo& color() const noexcept { return color_; }
    void setColor(const ColorInfo& value) { color_ = value; }

    /// Presentation timestamp on the source stream's own timebase.
    [[nodiscard]] const time::RationalTime& pts() const noexcept { return pts_; }
    void setPts(time::RationalTime value) { pts_ = std::move(value); }

    /// The source frame index this decoded from, or -1 if not known.
    [[nodiscard]] std::int64_t sourceIndex() const noexcept { return sourceIndex_; }
    void setSourceIndex(std::int64_t value) noexcept { sourceIndex_ = value; }

    [[nodiscard]] bool isKeyframe() const noexcept { return keyframe_; }
    void setKeyframe(bool value) noexcept { keyframe_ = value; }

    /// Pixel aspect ratio. Anamorphic footage is not square-pixel and forgetting
    /// that stretches every frame it touches.
    [[nodiscard]] const time::Rational& pixelAspect() const noexcept { return pixelAspect_; }
    void setPixelAspect(time::Rational value) { pixelAspect_ = std::move(value); }

    [[nodiscard]] std::uint8_t* plane(std::size_t index) noexcept { return planes_[index]; }
    [[nodiscard]] const std::uint8_t* plane(std::size_t index) const noexcept {
        return planes_[index];
    }
    [[nodiscard]] std::int32_t stride(std::size_t index) const noexcept { return strides_[index]; }
    [[nodiscard]] std::size_t planeCount() const noexcept { return planeCount_; }

    [[nodiscard]] std::size_t byteSize() const noexcept { return byteSize_; }

    /// A deep copy. Named rather than implicit so the allocation is visible.
    [[nodiscard]] VideoFrame clone() const;

    /// Convenience for tests and probes: the first luma (or red) sample.
    [[nodiscard]] std::uint16_t sampleAt(std::int32_t x, std::int32_t y,
                                         std::size_t planeIndex = 0) const;

private:
    struct FreeDeleter {
        void operator()(void* p) const noexcept;
    };

    std::unique_ptr<std::uint8_t[], FreeDeleter> storage_;
    std::array<std::uint8_t*, kMaxPlanes> planes_{};
    std::array<std::int32_t, kMaxPlanes> strides_{};
    std::size_t planeCount_{0};
    std::size_t byteSize_{0};

    std::int32_t width_{0};
    std::int32_t height_{0};
    PixelFormat format_{PixelFormat::Unknown};
    ColorInfo color_{};
    time::RationalTime pts_{};
    time::Rational pixelAspect_{1, 1};
    std::int64_t sourceIndex_{-1};
    bool keyframe_{false};
};

}  // namespace zaro::media
