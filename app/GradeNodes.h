#pragma once

#include <QWidget>
#include <array>

#include "Icons.h"

namespace zaro::app {

/// The grade chain, drawn.
///
/// **These are the stages the renderer actually runs**, in the order it runs
/// them: the primary correction and the wheels, then the tone curves, then the
/// secondary's qualified window, then the look LUT. Not a node graph -- this
/// program does not have one, and drawing boxes somebody can rewire when the
/// chain behind them is fixed would be a picture that lies about what happens
/// to the picture.
///
/// What it is for is navigation and orientation: where in the chain am I
/// working, and which stages on this clip have anything in them. A stage with
/// something in it is drawn lit, which makes "what has been done to this shot"
/// a glance rather than four panels to open.
class GradeNodes : public QWidget {
    Q_OBJECT

public:
    /// The stages, in render order.
    enum class Stage { Primary, Curves, Secondary, Look };
    static constexpr int kStageCount = 4;

    explicit GradeNodes(QWidget* parent = nullptr);

    [[nodiscard]] Stage stage() const noexcept { return stage_; }
    void setStage(Stage stage);
    /// Which stages have anything in them on the clip being shown.
    void setOccupied(std::array<bool, kStageCount> occupied);
    /// No clip selected: draw the chain greyed rather than empty, so the panel
    /// does not change shape when a selection comes and goes.
    void setEnabledChain(bool enabled);

    [[nodiscard]] static QString nameOf(Stage stage);

signals:
    void stageChosen(int stage);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    [[nodiscard]] QSize sizeHint() const override;

private:
    [[nodiscard]] QRect nodeRect(int at) const;

    Stage stage_{Stage::Primary};
    std::array<bool, kStageCount> occupied_{};
    bool enabled_{false};
};

}  // namespace zaro::app
