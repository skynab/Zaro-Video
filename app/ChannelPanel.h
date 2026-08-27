#pragma once

#include <QWidget>
#include <vector>

#include "zaro/core/edit/CommandStack.h"
#include "zaro/core/model/AudioProcessing.h"
#include "zaro/core/model/Project.h"

class QCheckBox;
class QEvent;
class QLabel;

#include "zaro/ui/SequenceBinding.h"

namespace zaro::app {

class GradientSlider;

/// The equaliser's response, drawn and draggable.
///
/// Three sections -- a high pass, a low pass and one bell -- which is what this
/// project's `model::AudioEq` is. Dragging the bell's handle moves its
/// frequency and its gain together, because that is the one gesture a curve
/// display is better at than a pair of number boxes; the width, and the two
/// filters' corners, stay as numbers underneath, where they read more clearly
/// than as another thing to grab.
class EqCurve : public QWidget {
    Q_OBJECT

public:
    explicit EqCurve(QWidget* parent = nullptr);

    void setEq(const model::AudioEq& eq);
    [[nodiscard]] const model::AudioEq& eq() const noexcept { return eq_; }

    [[nodiscard]] QSize sizeHint() const override;

signals:
    void changed(bool committed);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    [[nodiscard]] QRect plotArea() const;
    [[nodiscard]] double xFor(double hz) const;
    [[nodiscard]] double hzFor(double x) const;
    [[nodiscard]] double yFor(double db) const;
    [[nodiscard]] double dbFor(double y) const;
    [[nodiscard]] double responseAt(double hz) const;
    void take(const QPoint& where);

    model::AudioEq eq_;
    bool dragging_{false};
};

/// Everything about the channel the mixer has picked.
///
/// The right-hand column of the Audio workspace: what its equaliser is doing,
/// what its compressor is doing, and how hard the compressor is working. All of
/// it writes through `makeSetTrackProcessing`, so it undoes as one thing and
/// the mixer strip's own sketch of the curve stays in step.
class ChannelPanel : public QWidget, public ui::SequenceBound {
    Q_OBJECT

public:
    explicit ChannelPanel(QWidget* parent = nullptr);

    void bind(const ui::SequenceBinding& binding) override;
    /// Which channel to show. An invalid id empties the panel.
    void setTrack(model::TrackId track);
    void refresh();
    /// How hard the compressor is pulling, from the mixer's own meters.
    void setReduction(float reductionDb);

signals:
    void edited();

protected:
    /// The gain-reduction bar is one rectangle; a filter is cheaper than a
    /// class for it.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    [[nodiscard]] const model::Track* selected() const;
    void push(bool committed);

    model::Project* project_{nullptr};
    model::SequenceId sequenceId_;
    model::TrackId track_;
    edit::CommandStack* commands_{nullptr};
    bool updating_{false};
    float reductionDb_{0.0F};

    QLabel* name_{nullptr};
    QLabel* route_{nullptr};
    QCheckBox* eqOn_{nullptr};
    QCheckBox* compOn_{nullptr};
    EqCurve* curve_{nullptr};
    GradientSlider* highPass_{nullptr};
    GradientSlider* lowPass_{nullptr};
    GradientSlider* peakQ_{nullptr};
    GradientSlider* threshold_{nullptr};
    GradientSlider* ratio_{nullptr};
    GradientSlider* attack_{nullptr};
    GradientSlider* release_{nullptr};
    GradientSlider* makeup_{nullptr};
    QWidget* reduction_{nullptr};
    QWidget* body_{nullptr};
    QLabel* empty_{nullptr};
};

}  // namespace zaro::app
