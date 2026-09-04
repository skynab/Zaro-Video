#include <algorithm>
#include <cstdint>
#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "zaro/core/edit/Operations.h"
#include "zaro/core/render/RenderGraph.h"
#include "zaro/core/render/TextRasterizer.h"

#include "ModelFixtures.h"
#include "TestSources.h"

using namespace zaro;
using Catch::Approx;
using zaro::testing::Fixture;
using zaro::testing::SolidFrameSource;

namespace {

/// A stand-in font engine: a solid block of coverage across the graphic's box,
/// with a soft edge. Enough to check everything this layer is responsible for,
/// which is what happens to coverage after the glyphs exist.
class BlockRasterizer final : public render::TextRasterizer {
public:
    int calls{0};
    bool fail{false};

    Status renderCoverage(const model::Graphic& graphic, render::RgbaImage& out) override {
        ++calls;
        if (fail) {
            return Error{ErrorCode::Internal, "no font"};
        }
        const auto left = static_cast<std::int32_t>((out.width() * 0.5) + graphic.centreX -
                                                    (graphic.width * 0.5));
        const auto top = static_cast<std::int32_t>((out.height() * 0.5) + graphic.centreY -
                                                   (graphic.height * 0.5));
        for (std::int32_t y = 0; y < out.height(); ++y) {
            for (std::int32_t x = 0; x < out.width(); ++x) {
                const bool inside =
                    x >= left && x < left + graphic.width && y >= top && y < top + graphic.height;
                // Half coverage down the left column, to stand in for an
                // antialiased edge.
                const float alpha = !inside ? 0.0F : (x == left ? 0.5F : 1.0F);
                out.at(x, y) = render::Rgba{1.0F, 1.0F, 1.0F, alpha};
            }
        }
        return {};
    }
};

model::Graphic textGraphic() {
    model::Graphic out;
    out.kind = model::GraphicKind::Text;
    out.text = "Zaro";
    out.width = 20;
    out.height = 10;
    return out;
}

}  // namespace

/// Remembers how long the string it was handed was, which is what a reveal
/// changes. Drawing nothing is fine here: what is under test is the cut, and
/// the glyphs themselves are the font engine's business.
class CountingRasterizer final : public zaro::render::TextRasterizer {
public:
    Status renderCoverage(const zaro::model::Graphic& graphic,
                          zaro::render::RgbaImage& coverage) override {
        lastLength = graphic.text.size();
        coverage.clear();
        return {};
    }

    std::size_t lastLength{0};
};

TEST_CASE("Coverage becomes premultiplied colour in linear light", "[render][text]") {
    // The font engine produces coverage, not colour. Asking it for coloured
    // text and converting from sRGB per pixel would convert the antialiased
    // edges as if they were colours, which is why text composited in a linear
    // pipeline so often comes out thin.
    model::Graphic graphic = textGraphic();
    graphic.red = 0.8;
    graphic.green = 0.2;
    graphic.blue = 0.1;

    BlockRasterizer engine;
    render::RgbaImage out{64, 64};
    REQUIRE(render::drawText(graphic, &engine, out));
    CHECK(engine.calls == 1);

    const render::Rgba& solid = out.at(32, 32);
    CHECK(solid.a == Approx(1.0F));
    CHECK(solid.r == Approx(0.8F));
    CHECK(solid.g == Approx(0.2F));
    CHECK(solid.b == Approx(0.1F));

    // The half-covered edge: half the alpha, and the colour scaled with it, so
    // the edge fades rather than turning grey.
    const auto left = static_cast<std::int32_t>(32 - (graphic.width * 0.5));
    const render::Rgba& edge = out.at(left, 32);
    CHECK(edge.a == Approx(0.5F));
    CHECK(edge.r == Approx(0.4F));
    CHECK(edge.r / edge.a == Approx(0.8F).margin(1e-5));
}

TEST_CASE("The graphic's alpha scales the whole layer", "[render][text]") {
    model::Graphic graphic = textGraphic();
    graphic.alpha = 0.25;

    BlockRasterizer engine;
    render::RgbaImage out{64, 64};
    REQUIRE(render::drawText(graphic, &engine, out));
    CHECK(out.at(32, 32).a == Approx(0.25F));
}

TEST_CASE("Text without a rasteriser is left out rather than drawn blank", "[render][text]") {
    // A headless tool that was never given a font engine should render the rest
    // of the sequence and say it left the text out. An empty frame would look
    // like a bug in the text itself.
    render::RgbaImage out{32, 32};
    CHECK_FALSE(render::drawText(textGraphic(), nullptr, out));

    BlockRasterizer broken;
    broken.fail = true;
    CHECK_FALSE(render::drawText(textGraphic(), &broken, out));
}

TEST_CASE("Empty text succeeds and draws nothing", "[render][text]") {
    // Not the same as having no engine: there is simply nothing to say. A clip
    // whose text has been cleared should not report a failure.
    model::Graphic graphic = textGraphic();
    graphic.text.clear();

    BlockRasterizer engine;
    render::RgbaImage out{32, 32};
    CHECK(render::drawText(graphic, &engine, out));
    CHECK(engine.calls == 0);
    CHECK(out.at(16, 16).a == 0.0F);

    // And with no engine at all, which is the case a title that has been
    // emptied would otherwise fail in.
    CHECK(render::drawText(graphic, nullptr, out));
}

TEST_CASE("A shape graphic is not text", "[render][text]") {
    model::Graphic graphic = textGraphic();
    graphic.kind = model::GraphicKind::Rectangle;
    BlockRasterizer engine;
    render::RgbaImage out{32, 32};
    CHECK(render::drawText(graphic, &engine, out));
    CHECK(engine.calls == 0);
}

TEST_CASE("A caption graphic sits above the bottom margin", "[render][text][captions]") {
    // Built by shared code so the CPU and GPU paths put captions in the same
    // place, which is the whole of what a burned-in caption has to get right.
    model::CaptionStyle style;
    style.pointSize = 40.0;
    style.bottomMargin = 100.0;
    style.widthFraction = 0.5;

    const model::Graphic graphic = render::captionGraphic(style, "Hello", 1920, 1080);
    CHECK(graphic.kind == model::GraphicKind::Text);
    CHECK(graphic.text == "Hello");
    CHECK(graphic.width == Approx(960.0));
    CHECK(graphic.centreX == Approx(0.0));

    // The box's bottom edge is the margin above the bottom of the frame.
    const double bottomEdge = graphic.centreY + (graphic.height * 0.5);
    CHECK(bottomEdge == Approx((1080 * 0.5) - 100.0));

    // Tall enough for several lines, since captions grow upwards from the
    // margin and a two-line caption must not push itself off the frame.
    CHECK(graphic.height > style.pointSize * 2.0);
}

TEST_CASE("Captions are drawn over the picture", "[render][graph][captions]") {
    Fixture f;
    f.sequence().setSize(64, 64);
    SolidFrameSource source{64, 64};
    source.define(f.longMedia, render::Rgba{0.0F, 0.0F, 0.0F, 1.0F});
    render::RenderGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 100, 500))));

    model::Caption caption;
    caption.range =
        time::TimeRange::fromStartEnd(time::RationalTime{0, time::Rational{1000, 1}},
                                      time::RationalTime{2000, time::Rational{1000, 1}});
    caption.text = "Subtitle";
    f.sequence().captions().add(caption);

    // Off by default: a sidecar file is the normal delivery, and burning in is
    // a decision with no way back once the file is written.
    CHECK_FALSE(f.sequence().captions().isBurnedIn());
    const auto plain = graph.composite(f.sequence(), f.at(10));
    REQUIRE(plain);

    f.sequence().captions().setBurnedIn(true);
    BlockRasterizer engine;
    graph.setTextRasterizer(&engine);
    const auto burned = graph.composite(f.sequence(), f.at(10));
    REQUIRE(burned);
    CHECK(engine.calls == 1);

    // Past the caption's end there is nothing to draw.
    engine.calls = 0;
    REQUIRE(graph.composite(f.sequence(), f.at(80)));
    CHECK(engine.calls == 0);
}

TEST_CASE("Burned-in captions with no font engine are counted", "[render][graph][captions]") {
    // A delivered file quietly missing its captions is the worst outcome
    // available, so the number is on the record.
    Fixture f;
    f.sequence().setSize(32, 32);
    SolidFrameSource source{32, 32};
    source.define(f.longMedia, render::Rgba{0.0F, 0.0F, 0.0F, 1.0F});
    render::RenderGraph graph{source};
    REQUIRE(f.run(edit::makeOverwrite(f.project, f.on(f.v1), f.clip(0, 100, 500))));

    model::Caption caption;
    caption.range =
        time::TimeRange::fromStartEnd(time::RationalTime{0, time::Rational{1000, 1}},
                                      time::RationalTime{2000, time::Rational{1000, 1}});
    caption.text = "Subtitle";
    f.sequence().captions().add(caption);
    f.sequence().captions().setBurnedIn(true);

    REQUIRE(graph.composite(f.sequence(), f.at(10)));
    CHECK(graph.lastSkippedTextCount() == 1);
}

TEST_CASE("A reveal cuts the line by characters, not by bytes", "[render][text]") {
    using zaro::render::revealedText;

    CHECK(revealedText("Kestrel Bay", 1.0) == "Kestrel Bay");
    CHECK(revealedText("Kestrel Bay", 0.0).empty());
    // Eleven characters: a reveal of a bit over half is six of them.
    CHECK(revealedText("Kestrel Bay", 0.6) == "Kestre");
    // Anything past the end is the whole line rather than a read off it.
    CHECK(revealedText("Kestrel Bay", 2.0) == "Kestrel Bay");
    CHECK(revealedText("", 0.5).empty());

    // Multi-byte characters count as one each, and the cut never lands inside
    // one: a half-written character is something no font can draw.
    const std::string accented = "\u00e9\u00e9\u00e9\u00e9";  // four characters, eight bytes
    CHECK(revealedText(accented, 0.5) == "\u00e9\u00e9");
    CHECK(revealedText(accented, 0.25) == "\u00e9");
    for (double reveal = 0.0; reveal <= 1.0; reveal += 0.05) {
        const std::string shown = revealedText(accented, reveal);
        CHECK(shown.size() % 2 == 0);  // never half of a two-byte character
    }
}

TEST_CASE("A revealed title draws less of itself", "[render][text]") {
    zaro::model::Graphic title;
    title.kind = zaro::model::GraphicKind::Text;
    title.text = "MMMMMMMMMM";
    title.pointSize = 40.0;
    title.width = 600.0;
    title.height = 100.0;
    title.alignment = -1;  // left, so the line grows rightwards as it is typed
    title.red = 1.0;
    title.green = 1.0;
    title.blue = 1.0;

    CountingRasterizer rasterizer;
    zaro::render::RgbaImage out{200, 80};

    const auto covered = [&](double reveal) {
        REQUIRE(zaro::render::drawText(title, &rasterizer, out, reveal));
        return rasterizer.lastLength;
    };

    // What the engine was asked to draw grows with the reveal, and the ends are
    // the whole line and none of it.
    const std::size_t none = covered(0.0);
    const std::size_t third = covered(0.34);
    const std::size_t most = covered(0.7);
    const std::size_t all = covered(1.0);
    CHECK(none == 0);
    CHECK(third > none);
    CHECK(most > third);
    CHECK(all == title.text.size());
}

TEST_CASE("The reveal survives a trip through a project file", "[render][text][io]") {
    // A parameter that cannot be saved is a parameter somebody loses when they
    // close the project. The name is the only thing the file carries, so the
    // round trip is what proves the pair of conversions agree.
    zaro::model::Param decoded{};
    REQUIRE(zaro::model::paramFromString("textReveal", decoded));
    CHECK(decoded == zaro::model::Param::TextReveal);
    CHECK(std::string{zaro::model::toString(zaro::model::Param::TextReveal)} == "textReveal");

    // And it is in the list everything that visits every parameter walks, which
    // is what makes it appear in the file, the keyframe panel and the curve
    // editor without any of them naming it.
    const auto all = zaro::model::allParams();
    CHECK(std::find(all.begin(), all.end(), zaro::model::Param::TextReveal) != all.end());
}
