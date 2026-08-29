#include "AudioStrip.h"

#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <numbers>

#include "Theme.h"

namespace zaro::app {
namespace {

// The design's strip, in logical pixels.
constexpr int kWidth = 106;
constexpr int kHeaderHeight = 22;
constexpr int kInsertsTop = 28;
constexpr int kInsertHeight = 15;
constexpr int kEqHeight = 36;
constexpr int kPanSize = 30;
constexpr int kMeterWidth = 12;
constexpr int kButtonHeight = 16;

/// The fader law: -96..+12 dB over the travel, with 0 dB three quarters up.
///
/// Not linear in decibels. A console fader gives most of its travel to the
/// range a mix actually lives in -- the top twenty decibels -- and compresses
/// the bottom, where the difference between -60 and -70 is "off" either way.
constexpr double kMinDb = -96.0;
constexpr double kMaxDb = 12.0;

double faderFraction(double db) {
    const double clamped = std::clamp(db, kMinDb, kMaxDb);
    // A square curve on the normalised decibel, which puts 0 dB at about 0.79.
    const double linear = (clamped - kMinDb) / (kMaxDb - kMinDb);
    return std::pow(linear, 1.0 / 2.2);
}

double faderDb(double fraction) {
    const double linear = std::pow(std::clamp(fraction, 0.0, 1.0), 2.2);
    return kMinDb + (linear * (kMaxDb - kMinDb));
}

/// A meter's height, on the same 48 dB window the design uses.
double meterPosition(float peak) {
    if (peak <= 0.004F) {
        return 0.0;
    }
    const double db = 20.0 * std::log10(static_cast<double>(peak));
    return std::clamp((db + 48.0) / 48.0, 0.0, 1.0);
}

QString dbLabel(double db) {
    if (db <= kMinDb + 0.01) {
        return QStringLiteral("-∞");
    }
    return QString("%1%2").arg(db >= 0 ? "+" : "").arg(db, 0, 'f', 1);
}

QString panLabel(double pan) {
    if (std::abs(pan) < 0.02) {
        return QStringLiteral("C");
    }
    return QString("%1%2").arg(pan < 0 ? "L" : "R").arg(std::lround(std::abs(pan) * 100));
}

}  // namespace

AudioStrip::AudioStrip(model::TrackId track, Kind kind, QWidget* parent)
    : QWidget{parent}, track_{track}, kind_{kind} {
    setFixedWidth(kWidth);
    setMinimumHeight(300);
    setMouseTracking(false);
}

QSize AudioStrip::sizeHint() const {
    return QSize{kWidth, 340};
}

void AudioStrip::setName(const QString& name) {
    name_ = name;
    update();
}
void AudioStrip::setGainDb(double gainDb) {
    gainDb_ = gainDb;
    update();
}
void AudioStrip::setPan(double pan) {
    pan_ = std::clamp(pan, -1.0, 1.0);
    update();
}
void AudioStrip::setMuted(bool muted) {
    muted_ = muted;
    update();
}
void AudioStrip::setSoloed(bool soloed) {
    soloed_ = soloed;
    update();
}
void AudioStrip::setProcessing(const model::AudioEq& eq, const model::Compressor& compressor) {
    eq_ = eq;
    compressor_ = compressor;
    update();
}
void AudioStrip::setPicked(bool picked) {
    picked_ = picked;
    update();
}
void AudioStrip::setReduction(float reductionDb) {
    reductionDb_ = reductionDb;
    update();
}

/// The peak, with a hold that falls back slowly.
///
/// The hold is not decoration: what a meter is for is catching the transient
/// that went over, and a transient is by definition too brief to see.
void AudioStrip::setLevel(float peak) {
    level_ = peak;
    if (peak >= hold_) {
        hold_ = peak;
        held_ = 0;
    } else if (++held_ > 12) {
        hold_ = std::max(peak, hold_ - 0.02F);
    }
    update();
}

void AudioStrip::resetHold() {
    hold_ = level_;
    held_ = 0;
    update();
}

QRect AudioStrip::eqRect() const {
    return QRect{6, kInsertsTop + (kInsertHeight * 2) + 10, kWidth - 12, kEqHeight};
}

QRect AudioStrip::panRect() const {
    return QRect{8, eqRect().bottom() + 8, kPanSize, kPanSize};
}

QRect AudioStrip::muteRect() const {
    const int top = height() - kButtonHeight - 22;
    return QRect{7, top, ((kWidth - 14) / 3) - 2, kButtonHeight};
}

QRect AudioStrip::soloRect() const {
    const QRect mute = muteRect();
    return mute.translated(mute.width() + 3, 0);
}

QRect AudioStrip::faderRect() const {
    const int top = panRect().bottom() + 10;
    return QRect{10, top, kWidth - 20 - kMeterWidth - 7, muteRect().top() - top - 8};
}

QRect AudioStrip::meterRect() const {
    const QRect fader = faderRect();
    return QRect{fader.right() + 7, fader.top(), kMeterWidth, fader.height()};
}

void AudioStrip::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);

    // The strip's ground. A picked strip is washed with the accent, which is
    // how it says the channel panel on the right is showing this one.
    painter.fillRect(rect(),
                     picked_ ? theme::mix(theme::bg(), theme::accent(), 0.07) : theme::bg());
    painter.setPen(QPen{theme::divider(), 1.0});
    painter.drawLine(width() - 1, 0, width() - 1, height());

    // --- header ---------------------------------------------------------
    const QRect header{0, 0, width(), kHeaderHeight};
    const QColor accentInk =
        kind_ == Kind::Master ? QColor{0xd9, 0xc7, 0x6a} : QColor{0x8f, 0xc7, 0xd9};
    painter.setPen(Qt::NoPen);
    painter.fillRect(header, kind_ == Kind::Master ? theme::mix(theme::surface(), accentInk, 0.16)
                                                   : theme::surface());
    painter.setBrush(accentInk);
    painter.drawEllipse(QPointF{11.0, kHeaderHeight / 2.0}, 3.0, 3.0);

    QFont label = font();
    label.setPointSizeF(8.0);
    painter.setFont(label);
    painter.setPen(picked_ ? theme::text() : theme::textAt(0.68));
    const QFontMetrics labelMetrics{label};
    painter.drawText(QRect{19, 0, width() - 25, kHeaderHeight}, Qt::AlignLeft | Qt::AlignVCenter,
                     labelMetrics.elidedText(name_, Qt::ElideRight, width() - 25));

    // --- inserts: what is actually in the chain --------------------------
    //
    // The design lists named plugins. This project's chain is an equaliser and
    // a compressor, both part of the track rather than things loaded into it,
    // so the slots say what those two are set to and whether they are on.
    QFont tiny = font();
    tiny.setPointSizeF(6.5);
    painter.setFont(tiny);
    painter.setPen(theme::textAt(0.30));
    painter.drawText(QRect{7, kInsertsTop - 12, width() - 14, 11}, Qt::AlignLeft | Qt::AlignVCenter,
                     "INSERTS");

    const struct {
        bool on;
        QString text;
    } chain[] = {
        {eq_.enabled, eq_.enabled ? QString("EQ · %1 Hz").arg(std::lround(eq_.peakHz))
                                  : QStringLiteral("EQ · off")},
        {compressor_.enabled, compressor_.enabled
                                  ? QString("Comp %1:1").arg(compressor_.ratio, 0, 'f', 1)
                                  : QStringLiteral("Comp · off")},
    };
    for (int at = 0; at < 2; ++at) {
        const QRect slot{7, kInsertsTop + (at * kInsertHeight), width() - 14, kInsertHeight - 3};
        painter.setPen(Qt::NoPen);
        painter.setBrush(chain[at].on ? theme::mix(theme::bg(), accentInk, 0.16)
                                      : theme::mix(theme::bg(), theme::text(), 0.06));
        painter.drawRoundedRect(slot, 4, 4);
        painter.setPen(chain[at].on ? theme::textAt(0.80) : theme::textAt(0.42));
        painter.drawText(slot.adjusted(5, 0, -4, 0), Qt::AlignLeft | Qt::AlignVCenter,
                         chain[at].text);
    }

    // --- the equaliser's shape -------------------------------------------
    const QRect eq = eqRect();
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor{8, 9, 16});
    painter.drawRoundedRect(eq, 4, 4);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen{theme::divider(), 1.0});
    painter.drawRoundedRect(QRectF{eq}.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    paintEqCurve(painter, eq);

    // --- pan --------------------------------------------------------------
    const QRect pan = panRect();
    QLinearGradient pot{pan.topLeft(), pan.bottomLeft()};
    pot.setColorAt(0.0, theme::neutral(700));
    pot.setColorAt(1.0, theme::neutral(900));
    painter.setPen(Qt::NoPen);
    painter.setBrush(pot);
    painter.drawEllipse(pan);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen{theme::mix(theme::bg(), theme::text(), 0.12), 1.0});
    painter.drawEllipse(QRectF{pan}.adjusted(0.5, 0.5, -0.5, -0.5));
    // The pointer sweeps 270 degrees, centre up, as every pan pot does.
    const double sweep = pan_ * 135.0;
    const QPointF centre = QRectF{pan}.center();
    const double radians = (sweep - 90.0) * std::numbers::pi / 180.0;
    painter.setPen(QPen{accentInk, 1.5});
    painter.drawLine(centre, centre + QPointF{std::cos(radians) * 11.0, std::sin(radians) * 11.0});

    painter.setPen(theme::textAt(0.30));
    painter.setFont(tiny);
    painter.drawText(QRect{pan.right() + 7, pan.top() + 2, width() - pan.right() - 12, 10},
                     Qt::AlignLeft | Qt::AlignVCenter, "PAN");
    QFont mono{QStringLiteral("Menlo")};
    mono.setStyleHint(QFont::Monospace);
    mono.setPointSizeF(7.5);
    painter.setFont(mono);
    painter.setPen(theme::textAt(0.60));
    painter.drawText(QRect{pan.right() + 7, pan.top() + 13, width() - pan.right() - 12, 12},
                     Qt::AlignLeft | Qt::AlignVCenter, panLabel(pan_));

    // --- fader --------------------------------------------------------------
    const QRect fader = faderRect();
    const int trackX = fader.center().x();
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::mix(theme::bg(), theme::text(), 0.09));
    painter.drawRoundedRect(QRect{trackX - 1, fader.top() + 4, 3, fader.height() - 8}, 2, 2);

    const double at = 1.0 - faderFraction(gainDb_);
    const int capY = fader.top() + 4 + static_cast<int>(at * (fader.height() - 8));
    const QRect cap{trackX - 13, capY - 6, 26, 13};
    QLinearGradient capWash{cap.topLeft(), cap.bottomLeft()};
    capWash.setColorAt(0.0, theme::neutral(600));
    capWash.setColorAt(1.0, theme::neutral(800));
    painter.setBrush(capWash);
    painter.drawRoundedRect(cap, 3, 3);
    painter.setPen(QPen{accentInk, 1.0});
    painter.drawLine(cap.left() + 3, cap.center().y(), cap.right() - 3, cap.center().y());

    // --- meter --------------------------------------------------------------
    const QRect meter = meterRect();
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::mix(theme::bg(), theme::text(), 0.07));
    painter.drawRoundedRect(meter, 2, 2);
    if (const double filled = meterPosition(level_); filled > 0.0) {
        const int tall = static_cast<int>(filled * meter.height());
        const QRect lit{meter.left(), meter.bottom() - tall + 1, meter.width(), tall};
        QLinearGradient scale{QPointF{0, static_cast<double>(meter.bottom())},
                              QPointF{0, static_cast<double>(meter.top())}};
        scale.setColorAt(0.0, QColor{0x6a, 0xc7, 0xd9});
        scale.setColorAt(0.60, QColor{0x8f, 0xd9, 0xc7});
        scale.setColorAt(0.82, QColor{0xd9, 0xc7, 0x6a});
        scale.setColorAt(1.0, QColor{0xd9, 0x6a, 0x6a});
        painter.setBrush(scale);
        painter.drawRect(lit);
    }
    if (hold_ > 0.0F) {
        const int y = meter.bottom() - static_cast<int>(meterPosition(hold_) * meter.height());
        painter.setPen(hold_ >= 1.0F ? QColor{0xd9, 0x6a, 0x6a} : QColor{0xe6, 0xf2, 0xf6});
        painter.drawLine(meter.left(), y, meter.right(), y);
    }
    // Gain reduction, hanging down from the top of the meter: the one number a
    // compressor has that a level meter cannot show.
    if (reductionDb_ < -0.1F) {
        const int deep = static_cast<int>(std::min(1.0, -static_cast<double>(reductionDb_) / 24.0) *
                                          meter.height());
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor{0xd9, 0xc7, 0x6a, 150});
        painter.drawRect(QRect{meter.left(), meter.top(), 3, deep});
    }

    // --- mute, solo, and the readout ---------------------------------------
    painter.setFont(tiny);
    const struct {
        QRect box;
        bool on;
        QString text;
        QColor lit;
    } buttons[] = {
        {muteRect(), muted_, QStringLiteral("M"), QColor{0xd9, 0x6a, 0x6a}},
        {soloRect(), soloed_, QStringLiteral("S"), QColor{0xd9, 0xc7, 0x6a}},
    };
    for (const auto& button : buttons) {
        if (kind_ == Kind::Master) {
            break;
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(button.on ? button.lit : theme::mix(theme::bg(), theme::text(), 0.06));
        painter.drawRoundedRect(button.box, 4, 4);
        painter.setPen(button.on ? QColor{0x14, 0x15, 0x20} : theme::textAt(0.50));
        painter.drawText(button.box, Qt::AlignCenter, button.text);
    }

    painter.setFont(mono);
    painter.setPen(hold_ >= 1.0F ? QColor{0xd9, 0x6a, 0x6a} : theme::textAt(0.62));
    painter.drawText(QRect{0, height() - 20, width(), 18}, Qt::AlignCenter, dbLabel(gainDb_));
}

/// The equaliser's response, sketched.
///
/// Not a measured curve: a three-section EQ's shape is a high pass, a low pass
/// and a bell, and drawing each as its own contribution is both cheap and
/// honest about what the chain does. A strip this size shows the gesture, not
/// the arithmetic -- the numbers are in the channel panel.
void AudioStrip::paintEqCurve(QPainter& painter, const QRect& box) const {
    const QRect plot = box.adjusted(2, 2, -2, -2);
    painter.setPen(QPen{theme::mix(QColor{8, 9, 16}, theme::text(), 0.10), 1.0});
    painter.drawLine(plot.left(), plot.center().y(), plot.right(), plot.center().y());
    if (!eq_.enabled) {
        return;
    }

    // Log frequency across the box, 20 Hz to 20 kHz.
    const auto xFor = [&plot](double hz) {
        const double at = (std::log10(std::clamp(hz, 20.0, 20000.0)) - std::log10(20.0)) /
                          (std::log10(20000.0) - std::log10(20.0));
        return plot.left() + (at * plot.width());
    };

    QPainterPath curve;
    for (int step = 0; step <= 40; ++step) {
        const double at = static_cast<double>(step) / 40.0;
        const double hz =
            std::pow(10.0, std::log10(20.0) + (at * (std::log10(20000.0) - std::log10(20.0))));
        double db = 0.0;
        if (eq_.highPassHz > 0.0 && hz < eq_.highPassHz) {
            db -= 12.0 * std::log10(eq_.highPassHz / std::max(20.0, hz));
        }
        if (eq_.lowPassHz > 0.0 && hz > eq_.lowPassHz) {
            db -= 12.0 * std::log10(hz / eq_.lowPassHz);
        }
        if (eq_.peakGainDb != 0.0) {
            const double octaves = std::log2(hz / std::max(20.0, eq_.peakHz));
            const double width = std::max(0.2, 1.0 / std::max(0.1, eq_.peakQ));
            db += eq_.peakGainDb * std::exp(-(octaves * octaves) / (2.0 * width * width));
        }
        // +-18 dB across the box, which is the range a track EQ works in.
        const double y =
            plot.center().y() - (std::clamp(db, -18.0, 18.0) / 18.0) * (plot.height() / 2.0);
        const QPointF point{xFor(hz), y};
        if (step == 0) {
            curve.moveTo(point);
        } else {
            curve.lineTo(point);
        }
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen{QColor{0x8f, 0xc7, 0xd9}, 1.4});
    painter.drawPath(curve);
}

void AudioStrip::takeFader(const QPoint& where) {
    const QRect fader = faderRect();
    const double travel = std::max(1, fader.height() - 8);
    const double at = 1.0 - std::clamp((where.y() - (fader.top() + 4)) / travel, 0.0, 1.0);
    gainDb_ = faderDb(at);
    update();
}

void AudioStrip::takePan(const QPoint& where) {
    // Horizontal, because a pot that answers to up and down is a pot nobody can
    // aim: left and right is what the control means.
    const QRect pan = panRect();
    const double from = pan.center().x();
    pan_ = std::clamp((where.x() - from) / 40.0 + pan_, -1.0, 1.0);
    update();
}

void AudioStrip::mousePressEvent(QMouseEvent* event) {
    emit picked();
    if (kind_ == Kind::Track && muteRect().contains(event->pos())) {
        muted_ = !muted_;
        update();
        emit switched();
        return;
    }
    if (kind_ == Kind::Track && soloRect().contains(event->pos())) {
        soloed_ = !soloed_;
        update();
        emit switched();
        return;
    }
    if (panRect().adjusted(-4, -4, 4, 4).contains(event->pos())) {
        grab_ = Grab::Pan;
        return;
    }
    if (faderRect().adjusted(-6, 0, 6, 0).contains(event->pos())) {
        grab_ = Grab::Fader;
        takeFader(event->pos());
        emit moved(false);
    }
}

void AudioStrip::mouseMoveEvent(QMouseEvent* event) {
    if (grab_ == Grab::Fader) {
        takeFader(event->pos());
    } else if (grab_ == Grab::Pan) {
        takePan(event->pos());
    } else {
        return;
    }
    emit moved(false);
}

void AudioStrip::mouseReleaseEvent(QMouseEvent* /*event*/) {
    if (grab_ == Grab::None) {
        return;
    }
    grab_ = Grab::None;
    emit moved(true);
}

void AudioStrip::mouseDoubleClickEvent(QMouseEvent* event) {
    // Unity and centre, which is what a double-click means on every fader and
    // every pot.
    if (faderRect().adjusted(-6, 0, 6, 0).contains(event->pos())) {
        gainDb_ = 0.0;
    } else if (panRect().adjusted(-4, -4, 4, 4).contains(event->pos())) {
        pan_ = 0.0;
    } else {
        return;
    }
    update();
    emit moved(true);
}

}  // namespace zaro::app
