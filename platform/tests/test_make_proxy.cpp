#include <cmath>
#include <filesystem>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/platform/ffmpeg/FFmpegMedia.h"
#include "zaro/platform/ffmpeg/FFmpegRender.h"

#include "Fixtures.h"

using namespace zaro;
using zaro::testing::fixture;

namespace {

std::string scratch(const char* name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove(path);
    return path.string();
}

}  // namespace

TEST_CASE("a proxy has the same rate and the same number of frames", "[proxy]") {
    ZARO_REQUIRE_FIXTURE("shaky_texture.mov");
    const std::string source = fixture("shaky_texture.mov");
    const std::string destination = scratch("zaro-proxy-frames.mov");

    platform::ffmpeg::ProxySettings settings;
    settings.source = source;
    settings.destination = destination;
    settings.width = 160;

    auto made = platform::ffmpeg::makeProxy(settings);
    REQUIRE(made);
    CHECK(made->width == 160);
    CHECK(made->height == 120);  // the source is 320x240

    auto original = platform::ffmpeg::probe(source);
    auto proxy = platform::ffmpeg::probe(destination);
    REQUIRE(original);
    REQUIRE(proxy);
    const media::VideoStreamInfo* was = original->primaryVideo();
    const media::VideoStreamInfo* now = proxy->primaryVideo();
    REQUIRE(was != nullptr);
    REQUIRE(now != nullptr);

    // The whole contract of a proxy: same rate, same length. A proxy one frame
    // short would silently retime every cut made against it.
    CHECK(now->frameRate == was->frameRate);
    CHECK(now->durationInFrames().frames() == was->durationInFrames().frames());
    CHECK(made->frames == was->durationInFrames().frames());
    CHECK(now->width == 160);
    CHECK(now->height == 120);

    std::filesystem::remove(destination);
}

TEST_CASE("a proxy is smaller than what it stands in for", "[proxy]") {
    ZARO_REQUIRE_FIXTURE("shaky_texture.mov");
    const std::string destination = scratch("zaro-proxy-size.mov");

    platform::ffmpeg::ProxySettings settings;
    settings.source = fixture("shaky_texture.mov");
    settings.destination = destination;
    settings.width = 160;
    // No codec named: the default has to be one that makes a *smaller* file,
    // which the container's own default for .mov is not.
    auto made = platform::ffmpeg::makeProxy(settings);
    REQUIRE(made);
    CHECK(made->proxyBytes > 0);
    CHECK(made->proxyBytes < made->sourceBytes);

    std::filesystem::remove(destination);
}

TEST_CASE("a proxy carries the picture, not a blank frame", "[proxy]") {
    ZARO_REQUIRE_FIXTURE("shaky_texture.mov");
    const std::string destination = scratch("zaro-proxy-picture.mov");

    platform::ffmpeg::ProxySettings settings;
    settings.source = fixture("shaky_texture.mov");
    settings.destination = destination;
    settings.width = 160;
    REQUIRE(platform::ffmpeg::makeProxy(settings));

    // Decoded through the same path everything else uses, and compared against
    // the original at the same moment: a proxy that encoded grey would pass a
    // size check and fail here.
    const auto meanOf = [](const std::string& path, double& spread) {
        model::Project project;
        model::MediaRef ref;
        ref.id = project.ids().next<model::MediaRefTag>();
        ref.path = path;
        auto probed = platform::ffmpeg::probe(path);
        REQUIRE(probed);
        ref.info = *probed;
        const auto id = project.addMedia(ref);

        auto source = platform::ffmpeg::ProjectMediaSource::open(project);
        REQUIRE(source);
        auto frame = (*source)->imageFor(id, time::RationalTime{4, time::rates::fps25});
        REQUIRE(frame);
        double sum = 0.0;
        double squares = 0.0;
        const auto count = static_cast<double>((*frame)->width() * (*frame)->height());
        for (std::int32_t y = 0; y < (*frame)->height(); ++y) {
            const render::Rgba* row = (*frame)->row(y);
            for (std::int32_t x = 0; x < (*frame)->width(); ++x) {
                sum += static_cast<double>(row[x].g);
                squares += static_cast<double>(row[x].g) * static_cast<double>(row[x].g);
            }
        }
        const double mean = sum / count;
        spread = std::sqrt(std::max(0.0, (squares / count) - (mean * mean)));
        return mean;
    };

    double originalSpread = 0.0;
    double proxySpread = 0.0;
    const double originalMean = meanOf(fixture("shaky_texture.mov"), originalSpread);
    const double proxyMean = meanOf(destination, proxySpread);

    CHECK(proxyMean == Catch::Approx(originalMean).margin(0.05));
    // Detail survives the scale: a flat frame would have no spread at all, and
    // a badly resampled one would have noticeably less.
    CHECK(originalSpread > 0.05);
    CHECK(proxySpread > originalSpread * 0.6);

    std::filesystem::remove(destination);
}

TEST_CASE("a transcode keeps the size and changes only the codec", "[proxy][ingest]") {
    // Wider than the default proxy width on purpose: on a 320-pixel fixture,
    // "keep the source's size" and "shrink to 960" produce the same file and
    // the test proves nothing.
    ZARO_REQUIRE_FIXTURE("wide_texture.mp4");
    const std::string source = fixture("wide_texture.mp4");
    const std::string destination = scratch("zaro-ingest-transcode.mov");

    platform::ffmpeg::ProxySettings settings;
    settings.source = source;
    settings.destination = destination;
    settings.width = 0;  // the source's own size: an ingest transcode
    settings.videoCodec = "prores_ks";

    auto made = platform::ffmpeg::makeProxy(settings);
    REQUIRE(made);

    auto original = platform::ffmpeg::probe(source);
    auto ingested = platform::ffmpeg::probe(destination);
    REQUIRE(original);
    REQUIRE(ingested);
    const media::VideoStreamInfo* was = original->primaryVideo();
    const media::VideoStreamInfo* now = ingested->primaryVideo();
    REQUIRE(was != nullptr);
    REQUIRE(now != nullptr);

    CHECK(now->width == was->width);
    CHECK(now->height == was->height);
    CHECK(now->frameRate == was->frameRate);
    CHECK(now->durationInFrames().frames() == was->durationInFrames().frames());
    // The point of ingesting: an all-intra codec instead of whatever the
    // camera wrote.
    CHECK(now->width == 1280);
    CHECK(now->codecName == "prores");
    CHECK(was->codecName != "prores");

    std::filesystem::remove(destination);
}
