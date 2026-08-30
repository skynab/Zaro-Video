#pragma once

#include <QPushButton>

class QTimer;

namespace zaro::app {

/// The Donate button.
///
/// A hairline of spectrum around a button that is otherwise the ground it sits
/// on. It is the one place in the interface that is allowed to be colourful:
/// asking for money should look like an invitation rather than another
/// control, and it should look like it exactly once.
///
/// Painted rather than styled. A Qt stylesheet can put a gradient *on* a
/// border, but not a gradient ring around a fill of a different colour, and
/// faking it with a nested widget would put a second thing in the layout that
/// has to be kept the same size as the first.
///
/// The spectrum travels around the ring rather than sitting still, because a
/// still one stops being seen after a week. Slowly, and only while the button
/// is actually on screen: this is the one ornament in the interface, and an
/// ornament that repaints behind a hidden tab is just a warm laptop.
class SupportButton : public QPushButton {
    Q_OBJECT

public:
    explicit SupportButton(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;
    [[nodiscard]] QSize sizeHint() const override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    /// How far around the ring the spectrum has travelled, 0 to 1.
    double phase_{0.0};
    QTimer* clock_{nullptr};
};

}  // namespace zaro::app
