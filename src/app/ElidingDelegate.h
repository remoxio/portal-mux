#pragma once

#include <QApplication>
#include <QPainter>
#include <QStyledItemDelegate>

#include <algorithm>

inline constexpr int OnlineRole = Qt::UserRole + 10; // whether the host answers; unset: not known
inline constexpr int CountRole = Qt::UserRole + 11;  // a group's items, dimmed on the right

// measured as never fitting: Qt shifts fitting text a pixel; OnlineRole as a dot on the right, filled when it answers
class ElidingDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem opt = option;
        initStyleOption(&opt, index);
        const QWidget* widget = opt.widget;
        QStyle* style = widget ? widget->style() : QApplication::style();
        const QString text = opt.text;
        opt.text.clear();
        style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);
        if (text.isEmpty()) return;
        const int margin = style->pixelMetric(QStyle::PM_FocusFrameHMargin, nullptr, widget) + 1;
        opt.text = QString(500, QLatin1Char('x'));
        QRect rect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget).adjusted(margin, 0, -margin, 0);
        if (const QVariant online = index.data(OnlineRole); online.isValid()) {
            const int d = std::max(6, opt.fontMetrics.height() / 3);
            const QRectF dot(rect.right() - d - 2.5, rect.center().y() - d / 2.0 + 0.5, d, d);
            const QColor green = opt.palette.color(QPalette::Text).lightness() > 128 ? QColor(0x3f, 0xb9, 0x50) : QColor(0x1a, 0x7f, 0x37);
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing);
            if (online.toBool()) {
                painter->setPen(Qt::NoPen);
                painter->setBrush(green);
            } else {
                QColor dim = opt.palette.color(QPalette::Text);
                dim.setAlphaF(0.45);
                painter->setPen(QPen(dim, 1.2));
            }
            painter->drawEllipse(dot);
            painter->restore();
            rect.setRight(int(dot.left()) - 4);
        }
        if (const QVariant count = index.data(CountRole); count.isValid()) {
            QFont small = opt.font;
            small.setWeight(QFont::Normal);
            small.setPointSizeF(small.pointSizeF() * 0.85);
            const QString number = QString::number(count.toInt());
            const int width = QFontMetrics(small).horizontalAdvance(number);
            painter->save();
            painter->setFont(small);
            painter->setPen(opt.palette.color(opt.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::PlaceholderText));
            painter->drawText(QRect(rect.right() - width - 2, rect.top(), width + 2, rect.height()), Qt::AlignRight | Qt::AlignVCenter, number);
            painter->restore();
            rect.setRight(rect.right() - width - 8);
        }
        const QPalette::ColorGroup group = !(opt.state & QStyle::State_Enabled) ? QPalette::Disabled
                                           : opt.state & QStyle::State_Active   ? QPalette::Normal
                                                                                : QPalette::Inactive;
        painter->save();
        painter->setFont(opt.font);
        painter->setPen(opt.palette.color(group, opt.state & QStyle::State_Selected ? QPalette::HighlightedText : QPalette::Text));
        painter->drawText(rect, int(opt.displayAlignment), opt.fontMetrics.elidedText(text, opt.textElideMode, rect.width()));
        painter->restore();
    }
};
