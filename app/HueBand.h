#pragma once

#include <QWidget>

namespace zaro::app {

/// The hue circle laid out flat, with the selected window shaded.
///
/// Three numbers — centre, width, softness — describe a window on a circle, and
/// three spin boxes showing them describe nothing anyone can picture. The band
/// exists so the numbers can be read as a selection rather than as arithmetic.
class HueBand : public QWidget {
    Q_OBJECT

public:
    explicit HueBand(QWidget* parent = nullptr);

    void setWindow(double centre, double width, double softness);

signals:
    /// The centre was dragged. Width and softness stay where they were: they
    /// are separate decisions, and a single drag cannot mean three things.
    void centreChanged(double centre);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    [[nodiscard]] QSize sizeHint() const override;

private:
    [[nodiscard]] double hueAt(int x) const;

    double centre_{0.0};
    double width_{360.0};
    double softness_{15.0};
};

}  // namespace zaro::app
