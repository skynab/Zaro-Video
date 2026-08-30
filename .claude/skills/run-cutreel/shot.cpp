// Render one panel to a PNG, offscreen, so a visual change can be looked at
// without a window or desktop permissions. See ../SKILL.md for the build steps.
//
// THROWAWAY. Copy to a scratchpad and edit; do not commit a zaro_shot target.
//
// Usage: QT_QPA_PLATFORM=offscreen zaro_shot <out.png>
#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <QThread>

#include "ModelFixtures.h"
#include "Theme.h"
#include "ThumbnailCache.h"
#include "TimelineWidget.h"
#include "zaro/core/model/Effect.h"

using namespace zaro;
using namespace zaro::app;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fputs("usage: zaro_shot <out.png>\n", stderr);
        return 2;
    }
    QApplication application(argc, argv);
    theme::apply(application);

    testing::Fixture fx;
    for (auto& m : const_cast<std::vector<model::MediaRef>&>(fx.project.media())) {
        m.path = std::string{ZARO_TESTDATA_DIR} + "/shaky_texture.mov";
    }
    auto& seq = fx.sequence();

    // A third video track for titles, and two more audio tracks, so every
    // family the design names is on screen at once.
    const auto v3 = fx.project.ids().next<model::TrackTag>();
    const auto a2 = fx.project.ids().next<model::TrackTag>();
    seq.addTrack(v3, model::TrackKind::Video, "Titles");
    seq.findTrack(fx.v1)->setName("Main");
    seq.findTrack(fx.a1)->setName("Dialogue");
    seq.findTrack(fx.v2)->setName("B-roll");
    seq.findTrack(fx.v2)->setMuted(true);
    seq.findTrack(fx.a1)->setLocked(true);
    seq.addTrack(a2, model::TrackKind::Audio, "Score");

    auto add = [&](model::TrackId t, std::int64_t s, std::int64_t d, const char* name,
                   bool fxOn = false, bool linked = false, bool graphic = false) {
        model::Clip c = fx.clip(s, d);
        c.name = name;
        if (fxOn) c.effects.push_back(model::Effect{});
        if (linked) c.link = model::LinkId{1};
        if (graphic) c.graphic.kind = model::GraphicKind::Text;
        seq.findTrack(t)->insert(std::move(c));
    };

    add(v3, 50, 150, "Title - Kestrel Bay", false, false, true);
    add(v3, 850, 200, "End card", false, false, true);
    add(fx.v2, 150, 325, "DJI_0417_harbour_drone", true);
    add(fx.v2, 675, 300, "harbour_dusk_wide", true);
    add(fx.v1, 0, 675, "INT_marisa_A", false, true);
    add(fx.v1, 675, 450, "boat_wide_02");
    add(fx.a1, 0, 675, "INT_marisa_A.wav", false, true);
    add(fx.a1, 700, 425, "INT_marisa_C.wav");
    add(a2, 0, 1125, "score_bed_v3 - Low Tide", true);

    edit::CommandStack stack;
    TimelineWidget timeline;
    ThumbnailCache cache;
    timeline.setThumbnailCache(&cache);
    timeline.bind(ui::SequenceBinding{&fx.project, fx.sequenceId, &stack});
    timeline.resize(1240, 330);
    timeline.setPlayhead(time::RationalTime{380, time::rates::fps25});
    timeline.selectOnly(fx.v1, seq.findTrack(fx.v1)->clips().front().id);
    timeline.zoomToFit();

    // Anything that decodes -- filmstrips here, waveforms elsewhere -- arrives
    // asynchronously. Paint to queue the requests, let the worker run, and keep
    // painting so newly-wanted cells are asked for too. Drop this loop entirely
    // for a panel that draws only from the model.
    for (int i = 0; i < 40; ++i) {
        QPixmap warm(timeline.size());
        timeline.render(&warm);
        QApplication::processEvents();
        QThread::msleep(100);
    }
    QPixmap shot(timeline.size() * 2);
    shot.setDevicePixelRatio(2.0);
    timeline.render(&shot);
    shot.toImage().save(argv[1]);
    return 0;
}
