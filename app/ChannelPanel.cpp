#include "ChannelPanel.h"

#include <QCheckBox>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "zaro/core/edit/Operations.h"

#include "ColorPalette.h"
#include "Theme.h"

namespace zaro::app {
namespace {

constexpr double kLowHz = 20.0;
constexpr double kHighHz = 20000.0;
constexpr double kRangeDb = 18.0;

const QColor kEqInk{0x8f, 0xc7, 0xd9};

double toFraction(double value, double low, double high) {
    return std::clamp((value - low) / (high - low), 0.0, 1.0);
}
double fromFraction(double fraction, double low, double high) {
    return low + (std::clamp(fraction, 0.0, 1.0) * (high - low));
}

/// Frequency sliders are logarithmic, because hearing is: the useful half of a
/// 20 Hz to 20 kHz control is the bottom fifth of it on a linear scale.
double toLogFraction(double hz, double low, double high) {
    if (hz <= 0.0) {
        return 0.0;
    }
    return std::clamp((std::log10(hz) - std::log10(low)) / (std::log10(high) - std::log10(low)),
                      0.0, 1.0);
}
double fromLogFraction(double fraction, double low, double high) {
    return std::pow(10.0, std::log10(low) +
                              (std::clamp(fraction, 0.0, 1.0) *
                               (std::log10(high) - std::log10(low))));
}

}  // namespace

// --- EqCurve --------------------------------------------------------------

EqCurve::EqCurve(QWidget* parent) : QWidget{parent} {
    setObjectName("eq-curve");
    setFixedHeight(130);
    setCursor(Qt::PointingHandCursor);
}

QSize EqCurve::sizeHint() const {
    return QSize{272, 130};
}

void EqCurve::setEq(const model::AudioEq& eq) {
    eq_ = eq;
    update();
}

QRect EqCurve::plotArea() const {
    return rect().adjusted(1, 1, -1, -1);
}

double EqCurve::xFor(double hz) const {
    const QRect plot = plotArea();
    return plot.left() + (toLogFraction(hz, kLowHz, kHighHz) * plot.width());
}

double EqCurve::hzFor(double x) const {
    const QRect plot = plotArea();
    return fromLogFraction((x - plot.left()) / std::max(1, plot.width()), kLowHz, kHighHz);
}

double EqCurve::yFor(double db) const {
    const QRect plot = plotArea();
    return plot.center().y() - ((std::clamp(db, -kRangeDb, kRangeDb) / kRangeDb) *
                                (plot.height() / 2.0));
}

double EqCurve::dbFor(double y) const {
    const QRect plot = plotArea();
    return std::clamp(((plot.center().y() - y) / (plot.height() / 2.0)) * kRangeDb, -kRangeDb,
                      kRangeDb);
}

/// The three sections summed, in decibels.
///
/// Slopes rather than a filter's real magnitude response: 12 dB per octave past
/// a corner is what a second-order filter does, and this display is here to
/// show the shape somebody is dialling, not to certify it.
double EqCurve::responseAt(double hz) const {
    double db = 0.0;
    if (eq_.highPassHz > 0.0 && hz < eq_.highPassHz) {
        db -= 12.0 * std::log2(eq_.highPassHz / std::max(kLowHz, hz));
    }
    if (eq_.lowPassHz > 0.0 && hz > eq_.lowPassHz) {
        db -= 12.0 * std::log2(hz / eq_.lowPassHz);
    }
    if (eq_.peakGainDb != 0.0) {
        const double octaves = std::log2(hz / std::max(kLowHz, eq_.peakHz));
        const double width = std::max(0.15, 1.0 / std::max(0.1, eq_.peakQ));
        db += eq_.peakGainDb * std::exp(-(octaves * octaves) / (2.0 * width * width));
    }
    return db;
}

void EqCurve::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter{this};
    painter.setRenderHint(QPainter::Antialiasing, true);

    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::well());
    painter.drawRoundedRect(rect().adjusted(0, 0, -1, -1), 6, 6);

    const QRect plot = plotArea();
    painter.setPen(QPen{theme::mix(theme::well(), theme::text(), 0.07), 1.0});
    for (const double decade : {100.0, 1000.0, 10000.0}) {
        const int x = static_cast<int>(xFor(decade));
        painter.drawLine(x, plot.top(), x, plot.bottom());
    }
    for (const double db : {-9.0, 9.0}) {
        const int y = static_cast<int>(yFor(db));
        painter.drawLine(plot.left(), y, plot.right(), y);
    }
    painter.setPen(QPen{theme::mix(theme::well(), theme::text(), 0.16), 1.0, Qt::DashLine});
    painter.drawLine(plot.left(), static_cast<int>(yFor(0.0)), plot.right(),
                     static_cast<int>(yFor(0.0)));

    if (!eq_.enabled) {
        QFont note = font();
        note.setPointSizeF(8.0);
        painter.setFont(note);
        painter.setPen(theme::textAt(0.30));
        painter.drawText(plot, Qt::AlignCenter, "Equaliser off");
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen{theme::divider(), 1.0});
        painter.drawRoundedRect(QRectF{rect()}.adjusted(0.5, 0.5, -1.5, -1.5), 6, 6);
        return;
    }

    QPainterPath curve;
    QPainterPath fill;
    fill.moveTo(plot.left(), yFor(0.0));
    for (int step = 0; step <= 68; ++step) {
        const double at = static_cast<double>(step) / 68.0;
        const double hz = fromLogFraction(at, kLowHz, kHighHz);
        const QPointF point{xFor(hz), yFor(responseAt(hz))};
        if (step == 0) {
            curve.moveTo(point);
        } else {
            curve.lineTo(point);
        }
        fill.lineTo(point);
    }
    fill.lineTo(plot.right(), yFor(0.0));
    fill.closeSubpath();

    QColor wash = kEqInk;
    wash.setAlpha(40);
    painter.setPen(Qt::NoPen);
    painter.setBrush(wash);
    painter.drawPath(fill);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen{kEqInk, 1.6});
    painter.drawPath(curve);

    // The bell's handle. The two filter corners are drawn as ticks rather than
    // grabbable dots: they are one number each, and a slider says that better
    // than a dot on a curve that can only be dragged in two directions at once.
    for (const auto& [hz, ink] : {std::pair{eq_.highPassHz, QColor{0xd9, 0x9c, 0x6a}},
                                  std::pair{eq_.lowPassHz, QColor{0xd9, 0x6a, 0x9c}}}) {
        if (hz <= 0.0) {
            continue;
        }
        const int x = static_cast<int>(xFor(hz));
        painter.setPen(QPen{ink, 1.0, Qt::DotLine});
        painter.drawLine(x, plot.top(), x, plot.bottom());
    }
    painter.setPen(QPen{theme::well(), 2.0});
    painter.setBrush(kEqInk);
    painter.drawEllipse(QPointF{xFor(eq_.peakHz), yFor(eq_.peakGainDb)}, 6.0, 6.0);

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen{theme::divider(), 1.0});
    painter.drawRoundedRect(QRectF{rect()}.adjusted(0.5, 0.5, -1.5, -1.5), 6, 6);
}

void EqCurve::take(const QPoint& where) {
    eq_.peakHz = std::clamp(hzFor(where.x()), kLowHz, kHighHz);
    eq_.peakGainDb = dbFor(where.y());
    update();
}

void EqCurve::mousePressEvent(QMouseEvent* event) {
    if (!eq_.enabled) {
        return;
    }
    dragging_ = true;
    take(event->pos());
    emit changed(false);
}

void EqCurve::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_) {
        return;
    }
    take(event->pos());
    emit changed(false);
}

void EqCurve::mouseReleaseEvent(QMouseEvent* /*event*/) {
    if (!dragging_) {
        return;
    }
    dragging_ = false;
    emit changed(true);
}

// --- ChannelPanel ---------------------------------------------------------

namespace {

/// A labelled slider on a plain track, for the rows that are numbers rather
/// than colours. Reuses the Color workspace's control so the two panels do not
/// grow separate ideas of what a slider looks like.
GradientSlider* makeRow(const QString& label, QWidget* parent) {
    auto* slider = new GradientSlider{label, theme::neutral(800), QColor{},
                                      QColor{0x6a, 0xa8, 0xc7}, parent};
    slider->setFixedHeight(24);
    return slider;
}

QLabel* sectionHeading(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setObjectName("channel-heading");
    return label;
}

}  // namespace

ChannelPanel::ChannelPanel(QWidget* parent) : QWidget{parent} {
    setObjectName("channel-panel");
    setAttribute(Qt::WA_StyledBackground, true);

    auto* header = new QWidget(this);
    header->setObjectName("channel-header");
    header->setFixedHeight(32);
    auto* headerRow = new QHBoxLayout(header);
    headerRow->setContentsMargins(12, 0, 12, 0);
    headerRow->setSpacing(8);
    name_ = new QLabel(header);
    name_->setObjectName("gallery-title");
    route_ = new QLabel(header);
    route_->setProperty("muted", true);
    headerRow->addWidget(name_);
    headerRow->addStretch(1);
    headerRow->addWidget(route_);

    body_ = new QWidget(this);
    auto* column = new QVBoxLayout(body_);
    column->setContentsMargins(12, 12, 12, 12);
    column->setSpacing(9);

    auto* eqHead = new QHBoxLayout;
    eqHead->addWidget(sectionHeading("Equaliser", body_));
    eqHead->addStretch(1);
    eqOn_ = new QCheckBox("On", body_);
    eqHead->addWidget(eqOn_);
    column->addLayout(eqHead);

    curve_ = new EqCurve(body_);
    column->addWidget(curve_);
    connect(curve_, &EqCurve::changed, this, [this](bool committed) {
        if (updating_) {
            return;
        }
        push(committed);
    });

    highPass_ = makeRow("High pass", body_);
    lowPass_ = makeRow("Low pass", body_);
    peakQ_ = makeRow("Bell width", body_);
    for (GradientSlider* slider : {highPass_, lowPass_, peakQ_}) {
        column->addWidget(slider);
        connect(slider, &GradientSlider::changed, this, [this](bool committed) {
            if (!updating_) {
                push(committed);
            }
        });
    }

    auto* compHead = new QHBoxLayout;
    compHead->addWidget(sectionHeading("Compressor", body_));
    compHead->addStretch(1);
    compOn_ = new QCheckBox("On", body_);
    compHead->addWidget(compOn_);
    column->addLayout(compHead);

    // The gain-reduction bar, painted by a tiny anonymous widget: it is one
    // rectangle, and giving it a class of its own would be three files for a
    // fill.
    reduction_ = new QWidget(body_);
    reduction_->setFixedHeight(8);
    reduction_->installEventFilter(this);
    column->addWidget(reduction_);

    threshold_ = makeRow("Threshold", body_);
    ratio_ = makeRow("Ratio", body_);
    attack_ = makeRow("Attack", body_);
    release_ = makeRow("Release", body_);
    makeup_ = makeRow("Make-up", body_);
    for (GradientSlider* slider : {threshold_, ratio_, attack_, release_, makeup_}) {
        column->addWidget(slider);
        connect(slider, &GradientSlider::changed, this, [this](bool committed) {
            if (!updating_) {
                push(committed);
            }
        });
    }
    for (QCheckBox* box : {eqOn_, compOn_}) {
        connect(box, &QCheckBox::toggled, this, [this] {
            if (!updating_) {
                push(true);
            }
        });
    }
    column->addStretch(1);

    empty_ = new QLabel("No audio track picked", this);
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setProperty("muted", true);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(header);
    outer->addWidget(body_, 1);
    outer->addWidget(empty_, 1);

    refresh();
}

void ChannelPanel::setProject(model::Project* project, model::SequenceId sequence,
                              edit::CommandStack* commands) {
    project_ = project;
    sequenceId_ = sequence;
    commands_ = commands;
    refresh();
}

void ChannelPanel::setTrack(model::TrackId track) {
    track_ = track;
    refresh();
}

void ChannelPanel::setReduction(float reductionDb) {
    reductionDb_ = reductionDb;
    reduction_->update();
}

const model::Track* ChannelPanel::selected() const {
    if (project_ == nullptr || !track_.isValid()) {
        return nullptr;
    }
    const model::Sequence* sequence = project_->findSequence(sequenceId_);
    return sequence != nullptr ? sequence->findTrack(track_) : nullptr;
}

void ChannelPanel::refresh() {
    const model::Track* track = selected();
    body_->setVisible(track != nullptr);
    empty_->setVisible(track == nullptr);
    if (track == nullptr) {
        name_->setText("—");
        route_->setText({});
        return;
    }

    updating_ = true;
    name_->setText(QString::fromStdString(track->name()));
    route_->setText("→ Master");

    const model::AudioEq& eq = track->eq();
    eqOn_->setChecked(eq.enabled);
    curve_->setEq(eq);
    // Zero means the section is off, and the slider's floor is what says so.
    highPass_->setFraction(toLogFraction(eq.highPassHz, kLowHz, kHighHz));
    highPass_->setReadout(eq.highPassHz <= kLowHz ? QStringLiteral("off")
                                                  : QString("%1 Hz").arg(eq.highPassHz, 0, 'f', 0));
    lowPass_->setFraction(eq.lowPassHz <= 0.0 ? 1.0
                                              : toLogFraction(eq.lowPassHz, kLowHz, kHighHz));
    lowPass_->setReadout(eq.lowPassHz <= 0.0 || eq.lowPassHz >= kHighHz
                             ? QStringLiteral("off")
                             : QString("%1 Hz").arg(eq.lowPassHz, 0, 'f', 0));
    peakQ_->setFraction(toFraction(eq.peakQ, 0.3, 6.0));
    peakQ_->setReadout(QString("Q %1").arg(eq.peakQ, 0, 'f', 2));

    const model::Compressor& comp = track->compressor();
    compOn_->setChecked(comp.enabled);
    threshold_->setFraction(toFraction(comp.thresholdDb, -60.0, 0.0));
    threshold_->setReadout(QString("%1 dB").arg(comp.thresholdDb, 0, 'f', 1));
    ratio_->setFraction(toFraction(comp.ratio, 1.0, 20.0));
    ratio_->setReadout(QString("%1:1").arg(comp.ratio, 0, 'f', 1));
    attack_->setFraction(toFraction(comp.attackMs, 0.5, 120.0));
    attack_->setReadout(QString("%1 ms").arg(comp.attackMs, 0, 'f', 1));
    release_->setFraction(toFraction(comp.releaseMs, 20.0, 1000.0));
    release_->setReadout(QString("%1 ms").arg(comp.releaseMs, 0, 'f', 0));
    makeup_->setFraction(toFraction(comp.makeupDb, 0.0, 24.0));
    makeup_->setReadout(QString("%1 dB").arg(comp.makeupDb, 0, 'f', 1));
    updating_ = false;
}

bool ChannelPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched != reduction_ || event->type() != QEvent::Paint) {
        return QWidget::eventFilter(watched, event);
    }
    QPainter painter{reduction_};
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRect box = reduction_->rect();
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme::mix(theme::surface(), theme::text(), 0.07));
    painter.drawRoundedRect(box, 4, 4);
    // Drawn from the right, because reduction is something taken away.
    const double deep = std::min(1.0, -static_cast<double>(reductionDb_) / 24.0);
    if (deep > 0.004) {
        const int wide = static_cast<int>(deep * box.width());
        painter.setBrush(QColor{0xd9, 0xc7, 0x6a, 190});
        painter.drawRoundedRect(QRect{box.right() - wide, box.top(), wide, box.height()}, 4, 4);
    }
    QFont tiny = font();
    tiny.setPointSizeF(6.5);
    painter.setFont(tiny);
    painter.setPen(theme::textAt(0.55));
    painter.drawText(box.adjusted(5, 0, -5, 0), Qt::AlignLeft | Qt::AlignVCenter,
                     QString("GR %1 dB").arg(-reductionDb_, 0, 'f', 1));
    return true;
}

void ChannelPanel::push(bool committed) {
    const model::Track* track = selected();
    if (track == nullptr || commands_ == nullptr || project_ == nullptr) {
        return;
    }
    model::AudioEq eq = track->eq();
    eq.enabled = eqOn_->isChecked();
    eq.peakHz = curve_->eq().peakHz;
    eq.peakGainDb = curve_->eq().peakGainDb;
    const double hp = fromLogFraction(highPass_->fraction(), kLowHz, kHighHz);
    eq.highPassHz = hp <= kLowHz * 1.02 ? 0.0 : hp;
    const double lp = fromLogFraction(lowPass_->fraction(), kLowHz, kHighHz);
    eq.lowPassHz = lp >= kHighHz * 0.98 ? 0.0 : lp;
    eq.peakQ = fromFraction(peakQ_->fraction(), 0.3, 6.0);

    model::Compressor comp = track->compressor();
    comp.enabled = compOn_->isChecked();
    comp.thresholdDb = fromFraction(threshold_->fraction(), -60.0, 0.0);
    comp.ratio = fromFraction(ratio_->fraction(), 1.0, 20.0);
    comp.attackMs = fromFraction(attack_->fraction(), 0.5, 120.0);
    comp.releaseMs = fromFraction(release_->fraction(), 20.0, 1000.0);
    comp.makeupDb = fromFraction(makeup_->fraction(), 0.0, 24.0);

    auto built = edit::makeSetTrackProcessing(*project_, sequenceId_, track_, eq, comp);
    if (!built) {
        return;
    }
    commands_->execute(*project_, std::move(*built));
    if (committed) {
        commands_->breakMerge();
    }
    refresh();
    emit edited();
}

}  // namespace zaro::app
