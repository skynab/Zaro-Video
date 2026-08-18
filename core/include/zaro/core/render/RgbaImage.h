#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace zaro::render {

/// One pixel in the working space: scene-linear, Rec.709 primaries,
/// premultiplied alpha. See docs/adr/0005.
struct Rgba {
    float r{0.0F};
    float g{0.0F};
    float b{0.0F};
    float a{0.0F};

    friend bool operator==(const Rgba&, const Rgba&) = default;
};

/// The compositor's frame buffer.
///
/// 32 bytes a pixel: 8 MB at 1080p, 33 MB at 4K. That figure is the reason the
/// frame cache has a hard budget rather than a heuristic, and the reason this
/// type is move-only -- an accidental copy in a per-frame path is the difference
/// between realtime and not.
class RgbaImage {
public:
    RgbaImage() = default;
    RgbaImage(std::int32_t width, std::int32_t height);

    RgbaImage(const RgbaImage&) = delete;
    RgbaImage& operator=(const RgbaImage&) = delete;
    RgbaImage(RgbaImage&&) noexcept = default;
    RgbaImage& operator=(RgbaImage&&) noexcept = default;
    ~RgbaImage() = default;

    [[nodiscard]] bool isValid() const noexcept { return width_ > 0 && height_ > 0; }
    [[nodiscard]] std::int32_t width() const noexcept { return width_; }
    [[nodiscard]] std::int32_t height() const noexcept { return height_; }

    [[nodiscard]] Rgba* row(std::int32_t y) {
        return pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width_);
    }
    [[nodiscard]] const Rgba* row(std::int32_t y) const {
        return pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width_);
    }

    [[nodiscard]] Rgba& at(std::int32_t x, std::int32_t y) { return row(y)[x]; }
    [[nodiscard]] const Rgba& at(std::int32_t x, std::int32_t y) const { return row(y)[x]; }

    [[nodiscard]] std::size_t byteSize() const noexcept { return pixels_.size() * sizeof(Rgba); }

    /// Fill with transparent black -- the identity for `over`.
    void clear();
    void fill(const Rgba& colour);

    [[nodiscard]] RgbaImage clone() const;

    /// Bilinear sample in pixel coordinates, where (0,0) is the centre of the
    /// top-left pixel. Outside the image returns transparent black, so anything
    /// scaled or rotated fades out at its edge rather than smearing the border.
    [[nodiscard]] Rgba sampleBilinear(float x, float y) const;

private:
    std::vector<Rgba> pixels_;
    std::int32_t width_{0};
    std::int32_t height_{0};
};

}  // namespace zaro::render
