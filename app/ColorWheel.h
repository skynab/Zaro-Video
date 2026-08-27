#pragma once

#include <QString>
#include <QWidget>

namespace zaro::app {

/// One grading wheel: a hue disc with a puck on it, and a master under it.
///
/// The widget knows nothing about colour correction. It reports a balance --
/// a point in the unit square, centre neutral -- and a master, and whoever owns
/// it decides what those mean. That is deliberate: the arithmetic behind these
/// is an ASC CDL (see `model::ColorWheels`), the mapping from a puck to a slope
/// or an offset is a decision about feel, and neither belongs in a control that
/// is otherwise a circle with a dot on it.
///
/// **Two gestures, not three.** Dragging inside the disc moves the balance;
/// dragging the bar underneath moves the master. A wheel that also took a
/// scroll or a modifier-drag for the master would be a control whose result
/// depends on how somebody happened to be holding the mouse.
class ColorWheel : public QWidget {
    Q_OBJECT

public:
    explicit ColorWheel(QString name, QWidget* parent = nullptr);

    /// Balance, each -1..1, centre neutral. Y is positive upwards, as the
    /// readout reads it and as nobody expects a colour wheel to be flipped.
    void setBalance(double x, double y);
    void setMaster(double master);
    [[nodiscard]] double balanceX() const noexcept { return x_; }
    [[nodiscard]] double balanceY() const noexcept { return y_; }
    [[nodiscard]] double master() const noexcept { return master_; }

    /// Put it back to neutral. Also what a double-click does.
    void reset();

    [[nodiscard]] QSize sizeHint() const override;

signals:
    /// Something moved. `committed` is false during a drag and true when the
    /// gesture ends, so the owner can merge a drag into one undo step.
    void changed(bool committed);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    /// What the two gestures are, so a drag that started in the disc stays in
    /// the disc even when the pointer leaves it.
    enum class Grab { None, Disc, Master };

    [[nodiscard]] QRect discRect() const;
    [[nodiscard]] QRect masterRect() const;
    void takeDisc(const QPoint& where);
    void takeMaster(const QPoint& where);

    QString name_;
    double x_{0.0};
    double y_{0.0};
    double master_{0.0};
    Grab grab_{Grab::None};
};

}  // namespace zaro::app
