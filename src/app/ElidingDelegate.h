#pragma once

#include <QApplication>
#include <QPainter>
#include <QStyledItemDelegate>

// Qt's item layout moves the text a pixel when it fits; measuring it as never fitting keeps rows aligned.
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
        const QRect rect = style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget).adjusted(margin, 0, -margin, 0);
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
