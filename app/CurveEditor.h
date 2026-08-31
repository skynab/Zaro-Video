#pragma once

#include <QWidget>
#include <optional>

#include "zaro/core/model/ToneCurve.h"

class QComboBox;

namespace zaro::app {

/// A tone curve, drawn and edited directly.
///
/// The widget owns no model state beyond a copy of the curves it was given: it
/// emits a whole `ToneCurves` on every change and is told the new value back.
/// That is what keeps undo working — the panel cannot hold a version of the
/// curve that the command stack has never seen.
class CurveEditor : public QWidget {
    Q_OBJECT

public:
    explicit CurveEditor(QWidget* parent = nullptr);

    void setCurves(const model::ToneCurves& curves);
    [[nodiscard]] const model::ToneCurves& curves() const noexcept { return curves_; }

    /// The hue curve this editor also draws. Held beside the tone curves rather
    /// than folded into them because they are different things that happen to
    /// share a shape: one maps brightness to brightness, the other maps hue to
    /// a saturation multiplier, and a single struct carrying both would have to
    /// explain which of its members obeyed which rules.
    void setColorCurves(const model::ColorCurves& curves);
    [[nodiscard]] const model::ColorCurves& colorCurves() const noexcept { return hue_; }

    /// Where the curve is drawn, inside the widget. Public so a caller aiming
    /// at a point aims at the same square the widget does.
    [[nodiscard]] QRect plotArea() const;

    /// Which curve is being edited. Exposed so a self-test can drive the
    /// editor the way a person would rather than by reaching into the model.
    enum class Channel { Master, Red, Green, Blue, HueVsSat, LumaVsSat, HueVsHue };
    /// Whether the channel on show is a hue curve, which changes what the
    /// background means, where the neutral line sits, and which signal a change
    /// comes out of.
    [[nodiscard]] static bool isHue(Channel channel) noexcept {
        return channel == Channel::HueVsSat || channel == Channel::HueVsHue;
    }
    /// Whether the channel on show scales saturation rather than mapping
    /// brightness. Both saturation channels share a neutral half way up; only
    /// the hue one wraps.
    [[nodiscard]] static bool isSaturation(Channel channel) noexcept {
        return channel == Channel::HueVsSat || channel == Channel::LumaVsSat ||
               channel == Channel::HueVsHue;
    }
    [[nodiscard]] Channel channel() const noexcept { return channel_; }

signals:
    /// The curves changed. `committed` is false while a point is being dragged
    /// and true when the gesture ends, so the caller can coalesce a drag into
    /// one undo step and then break the merge.
    void curvesChanged(const zaro::model::ToneCurves& curves, bool committed);

    /// The same, for the hue curve. A separate signal rather than one carrying
    /// both, so a tone edit does not push a hue curve the model already has and
    /// land a no-op command on the undo stack.
    void colorCurvesChanged(const zaro::model::ColorCurves& curves, bool committed);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    [[nodiscard]] QSize sizeHint() const override;

private:
    [[nodiscard]] model::ToneCurve& active();
    [[nodiscard]] const model::ToneCurve& active() const;
    [[nodiscard]] QPointF toWidget(const model::CurvePoint& point) const;
    [[nodiscard]] model::CurvePoint toCurve(const QPointF& where) const;
    /// The index of the point near a widget position, if there is one.
    [[nodiscard]] std::optional<std::size_t> pointAt(const QPointF& where) const;
    /// Give an untouched curve its endpoints, so there is something to drag.
    void ensureEndpoints();

    /// Emit whichever signal the channel on show belongs to.
    void announce(bool committed);

    model::ToneCurves curves_;
    model::ColorCurves hue_;
    Channel channel_{Channel::Master};
    QComboBox* chooser_{nullptr};
    std::optional<std::size_t> dragging_;
};

}  // namespace zaro::app
