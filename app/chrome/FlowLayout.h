// A layout that wraps its items the way a line of text wraps.
#pragma once

#include <QLayout>
#include <QList>
#include <QSize>

class QLayoutItem;
class QRect;
class QWidget;

namespace zaro::app::chrome {

/// Left to right, wrapping when the row runs out of panel.
///
/// No Qt box layout does this: a horizontal one squeezes six chips into the
/// width of four, and a grid needs to be told how many fit before it can know
/// how many fit. This is Qt's own flow-layout example, kept to what the panels
/// here use.
///
/// It lived in an anonymous namespace inside ProjectBin.cpp, where the media
/// pane's filter chips are laid out with it and nothing else could reach it.
/// Nothing about it is about a media pane.
class FlowLayout : public QLayout {
public:
    FlowLayout(QWidget* parent, int margin, int gap);
    ~FlowLayout() override;

    FlowLayout(const FlowLayout&) = delete;
    FlowLayout& operator=(const FlowLayout&) = delete;

    void addItem(QLayoutItem* item) override { items_.append(item); }
    [[nodiscard]] int count() const override { return static_cast<int>(items_.size()); }
    [[nodiscard]] QLayoutItem* itemAt(int at) const override { return items_.value(at); }
    QLayoutItem* takeAt(int at) override;

    [[nodiscard]] Qt::Orientations expandingDirections() const override { return {}; }
    [[nodiscard]] bool hasHeightForWidth() const override { return true; }
    [[nodiscard]] int heightForWidth(int width) const override;
    void setGeometry(const QRect& rect) override;
    [[nodiscard]] QSize sizeHint() const override { return minimumSize(); }
    [[nodiscard]] QSize minimumSize() const override;

private:
    /// Returns the height the rows came to, so `heightForWidth` and
    /// `setGeometry` cannot disagree about where anything went.
    int place(const QRect& rect, bool measureOnly) const;

    QList<QLayoutItem*> items_;
};

}  // namespace zaro::app::chrome
