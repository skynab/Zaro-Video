#include "CurveEditor.h"

#include <QComboBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

#include "zaro/core/render/ColorCurveTable.h"

namespace zaro::app {
namespace {

const QColor kBackground{16, 16, 20};
const QColor kGrid{60, 60, 70};
const QColor kDiagonal{80, 80, 92};
const QColor kMasterLine{226, 226, 236};
const QColor kRedLine{232, 96, 96};
const QColor kGreenLine{96, 216, 128};
const QColor kBlueLine{110, 150, 245};

/// How close a click has to be to grab a point. Generous, for the same reason
/// a trim handle is: one that needs pixel accuracy is one nobody uses.
constexpr double kGrabPixels = 8.0;

}  // namespace

CurveEditor::CurveEditor(QWidget* parent) : QWidget{parent} {
    chooser_ = new QComboBox(this);
    chooser_->addItem("Master", static_cast<int>(Channel::Master));
    chooser_->addItem("Red", static_cast<int>(Channel::Red));
    chooser_->addItem("Green", static_cast<int>(Channel::Green));
    chooser_->addItem("Blue", static_cast<int>(Channel::Blue));
    chooser_->addItem("Hue vs Sat", static_cast<int>(Channel::HueVsSat));
    chooser_->addItem("Luma vs Sat", static_cast<int>(Channel::LumaVsSat));
    chooser_->addItem("Hue vs Hue", static_cast<int>(Channel::HueVsHue));
    chooser_->setObjectName("curve-channel");

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(chooser_);
    layout->addStretch(1);

    connect(chooser_, &QComboBox::currentIndexChanged, this, [this] {
        channel_ = static_cast<Channel>(chooser_->currentData().toInt());
        update();
    });
    setMinimumHeight(150);
}

QSize CurveEditor::sizeHint() const {
    return {180, 200};
}

void CurveEditor::setCurves(const model::ToneCurves& curves) {
    curves_ = curves;
    update();
}

model::ToneCurve& CurveEditor::active() {
    switch (channel_) {
        case Channel::Red:
            return curves_.red;
        case Channel::Green:
            return curves_.green;
        case Channel::Blue:
            return curves_.blue;
        case Channel::HueVsSat:
            return hue_.againstHue;
        case Channel::LumaVsSat:
            return hue_.againstLuma;
        case Channel::HueVsHue:
            return hue_.hueShift;
        case Channel::Master:
            break;
    }
    return curves_.master;
}

void CurveEditor::setColorCurves(const model::ColorCurves& curves) {
    hue_ = curves;
    update();
}

void CurveEditor::announce(bool committed) {
    if (isSaturation(channel_)) {
        emit colorCurvesChanged(hue_, committed);
    } else {
        emit curvesChanged(curves_, committed);
    }
}

const model::ToneCurve& CurveEditor::active() const {
    return const_cast<CurveEditor*>(this)->active();
}

QRect CurveEditor::plotArea() const {
    // Square, and centred in whatever is left below the chooser. A tone curve
    // drawn on a rectangle misreads: the diagonal is the reference, and it is
    // only at 45 degrees when the axes have the same scale.
    const QRect below = rect().adjusted(2, chooser_->height() + 6, -2, -2);
    const int side = std::max(20, std::min(below.width(), below.height()));
    return {below.left() + ((below.width() - side) / 2),
            below.top() + ((below.height() - side) / 2), side, side};
}

QPointF CurveEditor::toWidget(const model::CurvePoint& point) const {
    const QRect area = plotArea();
    return {area.left() + (point.x * area.width()),
            // Zero at the bottom: the curve's origin is black, and black is not
            // at the top of anything.
            area.bottom() - (point.y * area.height())};
}

model::CurvePoint CurveEditor::toCurve(const QPointF& where) const {
    const QRect area = plotArea();
    const double x = area.width() > 0 ? (where.x() - area.left()) / area.width() : 0.0;
    const double y = area.height() > 0 ? (area.bottom() - where.y()) / area.height() : 0.0;
    return {std::clamp(x, 0.0, 1.0), std::clamp(y, 0.0, 1.0)};
}

std::optional<std::size_t> CurveEditor::pointAt(const QPointF& where) const {
    const auto& points = active().points();
    for (std::size_t i = 0; i < points.size(); ++i) {
        const QPointF at = toWidget(points[i]);
        if (std::hypot(at.x() - where.x(), at.y() - where.y()) <= kGrabPixels) {
            return i;
        }
    }
    return std::nullopt;
}

void CurveEditor::ensureEndpoints() {
    model::ToneCurve& curve = active();
    if (!curve.points().empty()) {
        return;
    }
    if (isSaturation(channel_)) {
        // A saturation curve's identity is a flat line half way up. What it
        // needs is anchors: a curve with one point is still the identity -- two
        // are the fewest that describe a mapping -- so a first click on an
        // empty curve would otherwise do nothing at all and look broken.
        //
        // Four of them, evenly spaced, which is also what makes an adjustment
        // local: dragging one leaves the far end where it was. The hue curve
        // stops at 0.75 because its axis wraps and a point at 1.0 would be the
        // one at 0.0 twice over; the luma curve has a real far end and gets it.
        for (const double x : {0.0, 0.25, 0.5, 0.75}) {
            curve.set({x, 0.5});
        }
        if (!isHue(channel_)) {
            curve.set({1.0, 0.5});
        }
        return;
    }
    // An empty tone curve is the identity, and the identity drawn is the
    // diagonal: its endpoints are where they already appear to be.
    curve.set({0.0, 0.0});
    curve.set({1.0, 1.0});
}

void CurveEditor::mousePressEvent(QMouseEvent* event) {
    if (!plotArea().contains(event->position().toPoint())) {
        return;
    }
    ensureEndpoints();

    if (const auto found = pointAt(event->position())) {
        // Alt removes, the same as a keyframe on the timeline. The two
        // endpoints stay: a curve with fewer than two points is the identity,
        // so removing them would silently discard the whole curve.
        if (event->modifiers().testFlag(Qt::AltModifier)) {
            const auto& points = active().points();
            if (*found != 0 && *found + 1 != points.size()) {
                active().removeAt(points[*found].x);
                announce(true);
                update();
            }
            return;
        }
        dragging_ = found;
        update();
        return;
    }

    const model::CurvePoint added = toCurve(event->position());
    active().set(added);
    dragging_ = pointAt(event->position());
    announce(false);
    update();
}

void CurveEditor::mouseMoveEvent(QMouseEvent* event) {
    if (!dragging_.has_value()) {
        return;
    }
    model::ToneCurve& curve = active();
    const auto& points = curve.points();
    if (*dragging_ >= points.size()) {
        dragging_.reset();
        return;
    }

    model::CurvePoint moved = toCurve(event->position());
    // The outermost points are pinned in x. Black and white are where the curve
    // starts and ends; moving them inward would leave the ends of the range
    // undefined, and the held value there is not what anyone means by dragging
    // the black point.
    const bool isFirst = *dragging_ == 0;
    const bool isLast = *dragging_ + 1 == points.size();
    if (isFirst) {
        moved.x = points.front().x;
    } else if (isLast) {
        moved.x = points.back().x;
    } else {
        // Never past a neighbour: two points at one x is a vertical segment,
        // and crossing one would reorder the curve under the pointer.
        const double lower = points[*dragging_ - 1].x + 1e-4;
        const double upper = points[*dragging_ + 1].x - 1e-4;
        moved.x = upper > lower ? std::clamp(moved.x, lower, upper) : points[*dragging_].x;
    }

    curve.removeAt(points[*dragging_].x);
    curve.set(moved);
    // The index can shift when a point is re-inserted, so it is looked up
    // again rather than assumed.
    dragging_ = pointAt(toWidget(moved));
    announce(false);
    update();
}

void CurveEditor::mouseReleaseEvent(QMouseEvent* /*event*/) {
    if (dragging_.has_value()) {
        dragging_.reset();
        announce(true);
        update();
    }
}

void CurveEditor::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    const QRect area = plotArea();
    painter.fillRect(area, kBackground);
    painter.setRenderHint(QPainter::Antialiasing, true);

    painter.setPen(kGrid);
    for (int i = 1; i < 4; ++i) {
        const int x = area.left() + ((area.width() * i) / 4);
        const int y = area.top() + ((area.height() * i) / 4);
        painter.drawLine(x, area.top(), x, area.bottom());
        painter.drawLine(area.left(), y, area.right(), y);
    }
    // The reference: where the curve would be if it did nothing. A hue curve's
    // is a flat line half way up, because its value is a multiplier and the
    // middle of the range is "leave it alone" -- drawing a diagonal there would
    // say the identity slopes, which is the opposite of true.
    painter.setPen(kDiagonal);
    if (isSaturation(channel_)) {
        const int middle = area.bottom() - (area.height() / 2);
        painter.drawLine(area.left(), middle, area.right(), middle);
        // The hues themselves, along the axis they are the axis of. A curve
        // over an unlabelled 0..1 leaves somebody counting degrees to find the
        // blues; over a spectrum they can see where they are. Only for the hue
        // curve -- the luma one's axis is brightness, which the grid already
        // reads as left-to-right dark-to-light.
        const int stripHeight = isHue(channel_) ? std::max(3, area.height() / 22) : 0;
        for (int x = 0; isHue(channel_) && x < area.width(); ++x) {
            const double turn = static_cast<double>(x) / area.width();
            painter.fillRect(
                area.left() + x, area.bottom() - stripHeight + 1, 1, stripHeight,
                QColor::fromHsvF(static_cast<float>(std::clamp(turn, 0.0, 0.9999)), 0.75F, 0.85F));
        }
    } else {
        painter.drawLine(area.bottomLeft(), area.topRight());
    }

    const QColor colour = channel_ == Channel::Red     ? kRedLine
                          : channel_ == Channel::Green ? kGreenLine
                          : channel_ == Channel::Blue  ? kBlueLine
                                                       : kMasterLine;
    const model::ToneCurve& curve = active();

    // Sampled per pixel through the same evaluator the render path bakes, so
    // what is drawn is what will happen rather than a smooth-looking sketch of
    // it. A hue curve is drawn through the wrapped evaluation the table bakes,
    // for the same reason: the seam at red is where a curve looks wrong if the
    // picture and the drawing disagree about it.
    const render::ColorCurveTable preview =
        channel_ == Channel::HueVsSat ? render::ColorCurveTable{hue_} : render::ColorCurveTable{};
    QPainterPath path;
    for (int x = 0; x <= area.width(); ++x) {
        const double u = area.width() > 0 ? static_cast<double>(x) / area.width() : 0.0;
        // Back out of the multiplier into the curve's own 0.5-is-neutral range,
        // so the line lands where the points are.
        // The saturation-against-hue curve is drawn through the baked table, so
        // the wrap at red is drawn the way it will be applied. The other two
        // are their own curve: the luma one does not wrap, and the shift one
        // wraps in its *output* rather than along the axis being drawn.
        const double v =
            channel_ == Channel::HueVsSat
                ? static_cast<double>(preview.saturationAt(static_cast<float>(u))) / 2.0
                : curve.valueAt(u);
        const QPointF at(area.left() + x,
                         area.bottom() - (std::clamp(v, 0.0, 1.0) * area.height()));
        if (x == 0) {
            path.moveTo(at);
        } else {
            path.lineTo(at);
        }
    }
    painter.setPen(QPen(colour, 1.5));
    painter.drawPath(path);

    painter.setBrush(colour);
    painter.setPen(QPen(kBackground, 1));
    for (const model::CurvePoint& point : curve.points()) {
        painter.drawEllipse(toWidget(point), 3.5, 3.5);
    }
}

}  // namespace zaro::app
