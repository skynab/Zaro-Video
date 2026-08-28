#include "FlowLayout.h"

#include <QLayoutItem>
#include <QMargins>
#include <QPoint>
#include <QRect>
#include <QWidget>
#include <algorithm>

namespace zaro::app::chrome {

FlowLayout::FlowLayout(QWidget* parent, int margin, int gap) : QLayout{parent} {
    setContentsMargins(margin, margin, margin, margin);
    setSpacing(gap);
}

FlowLayout::~FlowLayout() {
    while (QLayoutItem* item = takeAt(0)) {
        delete item;
    }
}

QLayoutItem* FlowLayout::takeAt(int at) {
    return at >= 0 && at < items_.size() ? items_.takeAt(at) : nullptr;
}

int FlowLayout::heightForWidth(int width) const {
    return place(QRect{0, 0, width, 0}, true);
}

void FlowLayout::setGeometry(const QRect& rect) {
    QLayout::setGeometry(rect);
    place(rect, false);
}

QSize FlowLayout::minimumSize() const {
    QSize size;
    for (const QLayoutItem* item : items_) {
        size = size.expandedTo(item->minimumSize());
    }
    const QMargins margins = contentsMargins();
    return size + QSize{margins.left() + margins.right(), margins.top() + margins.bottom()};
}

int FlowLayout::place(const QRect& rect, bool measureOnly) const {
    const QMargins margins = contentsMargins();
    const QRect area =
        rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
    int x = area.x();
    int y = area.y();
    int rowHeight = 0;
    for (QLayoutItem* item : items_) {
        const QSize hint = item->sizeHint();
        int next = x + hint.width();
        if (next > area.right() + 1 && rowHeight > 0) {
            x = area.x();
            y += rowHeight + spacing();
            rowHeight = 0;
            next = x + hint.width();
        }
        if (!measureOnly) {
            item->setGeometry(QRect{QPoint{x, y}, hint});
        }
        x = next + spacing();
        rowHeight = std::max(rowHeight, hint.height());
    }
    return y + rowHeight - rect.y() + margins.bottom();
}

}  // namespace zaro::app::chrome
