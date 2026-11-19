#include "app/PageRail.h"

#include <QListWidget>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

#include <algorithm>

namespace {

#if defined(Q_OS_MACOS)
constexpr int rowHeight = 28, padding = 6, panelRadius = 10;
#elif defined(Q_OS_WIN)
constexpr int rowHeight = 34, padding = 0, panelRadius = 0;
#else
constexpr int rowHeight = 34, padding = 6, panelRadius = 8;
#endif

QColor ink(const QPalette& palette, qreal alpha)
{
    QColor color = palette.color(QPalette::WindowText);
    color.setAlphaF(alpha);
    return color;
}

bool dark(const QPalette& palette) { return palette.color(QPalette::WindowText).lightness() > 128; }

class Panel : public QWidget {
protected:
    void paintEvent(QPaintEvent*) override
    {
        if (!panelRadius) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(ink(palette(), 0.03));
        p.drawRoundedRect(QRectF(rect()), panelRadius, panelRadius);
    }
};

class RowDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        return {QStyledItemDelegate::sizeHint(option, index).width() + 24, rowHeight};
    }

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem o = option;
        initStyleOption(&o, index);
        const QRectF r = o.rect;
        const bool selected = o.state & QStyle::State_Selected;
        const QColor subtle = ink(o.palette, dark(o.palette) ? 0.06 : 0.05);
        QColor text = o.palette.color(QPalette::WindowText);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
#ifdef Q_OS_WIN
        if (selected || (o.state & QStyle::State_MouseOver)) {
            p->setBrush(subtle);
            p->drawRoundedRect(r, 6, 6);
        }
        if (selected) {
            p->setBrush(o.palette.color(QPalette::Active, QPalette::Accent));
            p->drawRoundedRect(QRectF(r.left(), r.center().y() - 8, 3, 16), 1.5, 1.5);
        }
#else
        if (selected) { // the accent also when the window isn't active, as AppKit's sidebars
            p->setBrush(o.palette.color(QPalette::Active, QPalette::Accent));
            p->drawRoundedRect(r, 6, 6);
            text = o.palette.color(QPalette::Active, QPalette::HighlightedText);
        } else if (o.state & QStyle::State_MouseOver) {
            p->setBrush(subtle);
            p->drawRoundedRect(r, 6, 6);
        }
#endif
        const qreal dpr = p->device()->devicePixelRatioF();
        QPixmap icon = o.icon.pixmap(QSize(16, 16), dpr);
        if (!icon.isNull()) { // in the text's colour
            QPainter tint(&icon);
            tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tint.fillRect(icon.rect(), text);
        }
        p->drawPixmap(QPointF(r.left() + 8, r.center().y() - 8), icon);
        p->setPen(text);
        p->setFont(o.font);
        const QRectF label = r.adjusted(8 + 16 + 8, 0, -8, 0);
        p->drawText(label, Qt::AlignLeft | Qt::AlignVCenter, o.fontMetrics.elidedText(o.text, Qt::ElideRight, int(label.width())));
        p->restore();
    }
};

} // namespace

QWidget* pageRail(QListWidget* list)
{
    list->setItemDelegate(new RowDelegate(list));
    list->setFrameShape(QFrame::NoFrame);
    list->setAttribute(Qt::WA_MacShowFocusRect, false);
    list->setMouseTracking(true); // hover
    list->setSpacing(1);          // 2 between rows
    list->viewport()->setAutoFillBackground(false);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* panel = new Panel;
    auto* layout = new QVBoxLayout(panel);
    layout->setContentsMargins(padding, padding, padding, padding);
    layout->addWidget(list);
    panel->setFixedWidth(std::max(172, list->sizeHintForColumn(0) + 2 * padding + 2)); // as its longest item, if wider
    return panel;
}
