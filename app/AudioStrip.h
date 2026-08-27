#pragma once

#include <QString>
#include <QWidget>

#include "zaro/core/model/AudioProcessing.h"
#include "zaro/core/model/Ids.h"

namespace zaro::app {

/// One channel of the mixer, drawn as a console strip.
///
/// **Painted rather than assembled.** A strip is a fader, a pan pot, two
/// meters, three buttons and a readout in 106 pixels; built from widgets that
/// is eight children and a layout per channel, and a mixer is ten channels
/// wide. Painted it is one widget, and the fader and the pot have to be drawn
/// by hand either way -- Qt has no control shaped like either.
///
/// The strip holds no truth. It is told what the track says and reports what
/// somebody did to it; the panel above it writes that through the command
/// stack and tells it again.
class AudioStrip : public QWidget {
    Q_OBJECT

public:
    /// What the strip is a channel of. The master is drawn with a warm header
    /// and takes no mute or solo, because muting the master is a thing nobody
    /// means and every console refuses.
    enum class Kind { Track, Master };

    explicit AudioStrip(model::TrackId track, Kind kind, QWidget* parent = nullptr);

    void setName(const QString& name);
    /// Decibels, as the model keeps them.
    void setGainDb(double gainDb);
    /// -1 hard left, +1 hard right.
    void setPan(double pan);
    void setMuted(bool muted);
    void setSoloed(bool soloed);
    void setProcessing(const model::AudioEq& eq, const model::Compressor& compressor);
    /// A linear peak from the mixer, 0..1 and beyond.
    void setLevel(float peak);
    /// How much the compressor is pulling this channel down, in decibels.
    void setReduction(float reductionDb);
    void setPicked(bool picked);

    [[nodiscard]] model::TrackId track() const noexcept { return track_; }
    [[nodiscard]] double gainDb() const noexcept { return gainDb_; }
    [[nodiscard]] double pan() const noexcept { return pan_; }
    [[nodiscard]] bool muted() const noexcept { return muted_; }
    [[nodiscard]] bool soloed() const noexcept { return soloed_; }

    [[nodiscard]] QSize sizeHint() const override;

signals:
    /// The fader or the pot moved. `committed` is false during a drag.
    void moved(bool committed);
    /// A button was pressed. The panel re-reads `muted()` and `soloed()`.
    void switched();
    /// This strip was clicked, so the channel panel should follow it.
    void picked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    enum class Grab { None, Fader, Pan };

    [[nodiscard]] QRect faderRect() const;
    [[nodiscard]] QRect meterRect() const;
    [[nodiscard]] QRect panRect() const;
    [[nodiscard]] QRect eqRect() const;
    [[nodiscard]] QRect muteRect() const;
    [[nodiscard]] QRect soloRect() const;
    void takeFader(const QPoint& where);
    void takePan(const QPoint& where);
    void paintEqCurve(QPainter& painter, const QRect& box) const;

    model::TrackId track_;
    Kind kind_;
    QString name_;
    double gainDb_{0.0};
    double pan_{0.0};
    bool muted_{false};
    bool soloed_{false};
    bool picked_{false};
    model::AudioEq eq_;
    model::Compressor compressor_;
    float level_{0.0F};
    float hold_{0.0F};
    int held_{0};
    float reductionDb_{0.0F};
    Grab grab_{Grab::None};
};

}  // namespace zaro::app
