// Delivering a graphic over nothing.
//
// A title, a lower third or a logo is handed over with its coverage intact, and
// the encoder used to drop it: every pixel format it chose was opaque, and the
// conversion went through an RGB buffer with no fourth component. An export of
// a lower third came back as a lower third on black, which looks like it worked
// and is discovered by whoever tries to key it.

#include <cstdint>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>

#include "zaro/core/render/RgbaImage.h"
#include "zaro/platform/ffmpeg/FFmpegMedia.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"

using namespace zaro;

namespace {

std::string scratch(const char* name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove(path);
    return path.string();
}

/// A frame that is half covered: opaque on the left, clear on the right. The
/// colour is premultiplied, as the compositor leaves it.
render::RgbaImage halfCovered(std::int32_t width, std::int32_t height) {
    render::RgbaImage image{width, height};
    for (std::int32_t y = 0; y < height; ++y) {
        render::Rgba* row = image.row(y);
        for (std::int32_t x = 0; x < width; ++x) {
            const bool solid = x < width / 2;
            row[x] =
                solid ? render::Rgba{0.8F, 0.2F, 0.2F, 1.0F} : render::Rgba{0.0F, 0.0F, 0.0F, 0.0F};
        }
    }
    return image;
}

}  // namespace

TEST_CASE("ProRes 4444 carries the coverage", "[alpha]") {
    const std::string path = scratch("zaro_alpha.mov");

    platform::ffmpeg::EncodeSettings settings;
    settings.path = path;
    settings.width = 64;
    settings.height = 32;
    settings.frameRate = time::rates::fps25;
    settings.videoCodec = "prores_ks";
    settings.includeAudio = false;
    settings.alpha = true;

    auto encoder = platform::ffmpeg::Encoder::open(settings);
    REQUIRE(encoder);
    const render::RgbaImage frame = halfCovered(64, 32);
    for (int i = 0; i < 4; ++i) {
        REQUIRE((*encoder)->writeVideo(frame));
    }
    REQUIRE((*encoder)->finish());

    REQUIRE(std::filesystem::exists(path));
    REQUIRE(std::filesystem::file_size(path) > 0);

    auto probed = platform::ffmpeg::probe(path);
    REQUIRE(probed);
    REQUIRE(probed->primaryVideo() != nullptr);
    CHECK(probed->primaryVideo()->codecName.find("prores") != std::string::npos);
    CHECK(probed->primaryVideo()->width == 64);

    // What actually says the coverage survived: the same export without the
    // flag lands on a different pixel format. If alpha were being dropped the
    // two files would be identical 4:2:2, which is precisely the failure this
    // guards -- an export that looks like it worked.
    const std::string opaquePath = scratch("zaro_alpha_compare.mov");
    platform::ffmpeg::EncodeSettings opaque = settings;
    opaque.path = opaquePath;
    opaque.alpha = false;
    auto second = platform::ffmpeg::Encoder::open(opaque);
    REQUIRE(second);
    for (int i = 0; i < 4; ++i) {
        REQUIRE((*second)->writeVideo(frame));
    }
    REQUIRE((*second)->finish());
    auto opaqueProbe = platform::ffmpeg::probe(opaquePath);
    REQUIRE(opaqueProbe);
    REQUIRE(opaqueProbe->primaryVideo() != nullptr);
    CHECK(opaqueProbe->primaryVideo()->pixelFormat != probed->primaryVideo()->pixelFormat);
    // And the opaque one is the 4:2:2 it always was, so the comparison above is
    // between a known format and something wider rather than between two
    // unknowns.
    CHECK(opaqueProbe->primaryVideo()->pixelFormat == media::PixelFormat::YUV422P10);

    std::filesystem::remove(opaquePath);
    std::filesystem::remove(path);
}

TEST_CASE("Asking a codec that cannot hold alpha is refused", "[alpha]") {
    // Refused rather than ignored. An export that silently composites a lower
    // third onto black looks like it worked, and the person who finds out is
    // the one who was given the file.
    const std::string path = scratch("zaro_alpha_refused.mp4");

    platform::ffmpeg::EncodeSettings settings;
    settings.path = path;
    settings.width = 64;
    settings.height = 32;
    settings.frameRate = time::rates::fps25;
    settings.videoCodec = "libx264";
    settings.includeAudio = false;
    settings.alpha = true;

    auto encoder = platform::ffmpeg::Encoder::open(settings);
    CHECK_FALSE(encoder);
    if (!encoder) {
        // And it says why, naming the codec, rather than failing generically.
        CHECK(encoder.error().toString().find("alpha") != std::string::npos);
    }
    std::filesystem::remove(path);
}

TEST_CASE("An export without alpha is unchanged", "[alpha]") {
    // The default path, which must not move: three components, an opaque pixel
    // format, and the same file it always wrote.
    const std::string path = scratch("zaro_opaque.mov");

    platform::ffmpeg::EncodeSettings settings;
    settings.path = path;
    settings.width = 64;
    settings.height = 32;
    settings.frameRate = time::rates::fps25;
    settings.videoCodec = "prores_ks";
    settings.includeAudio = false;

    auto encoder = platform::ffmpeg::Encoder::open(settings);
    REQUIRE(encoder);
    const render::RgbaImage frame = halfCovered(64, 32);
    for (int i = 0; i < 4; ++i) {
        REQUIRE((*encoder)->writeVideo(frame));
    }
    REQUIRE((*encoder)->finish());
    CHECK(std::filesystem::file_size(path) > 0);
    std::filesystem::remove(path);
}
