#include "app/GuestRow.h"

#include "app/Icons.h"

#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionViewItem>

#include <algorithm>

static constexpr int samples = 40;

QSize guestRowSize(const QStyleOptionViewItem& option) { return {160, std::max(44, 2 * option.fontMetrics.height() + 12)}; }

void paintGuestRow(QPainter* p, const QStyleOptionViewItem& opt, const GuestRow& row)
{
    const bool selected = opt.state & QStyle::State_Selected;
    const QColor text = opt.palette.color(QPalette::Normal, selected ? QPalette::HighlightedText : QPalette::Text);
    QColor dim = text;
    dim.setAlphaF(0.6);
    const QRect r = opt.rect.adjusted(6, 4, -6, -4);

    const QRect icon(r.left(), r.center().y() - 16, 32, 32);
    icons::tabler(row.icon, row.active ? text : dim).paint(p, icon);
    if (row.glyph) icons::tabler(row.glyph, row.glyphColor).paint(p, QRect(icon.left() + 10, icon.top() + 7, 12, 12));

    const int graphWidth = std::min(84, r.right() / 3); // not by the row's width: the same at every depth
    const QRect graph(r.right() - graphWidth + 1, r.top() + 2, graphWidth, r.height() - 4);
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->setPen(QPen(dim, 1));
    p->setBrush(Qt::NoBrush);
    p->drawRect(QRectF(graph).adjusted(0.5, 0.5, -0.5, -0.5));
    if (row.cpu.size() > 1) {
        const QRectF area = QRectF(graph).adjusted(1.5, 1.5, -1.5, -1.5);
        const double step = area.width() / (samples - 1); // the newest on the right
        QPainterPath line;
        for (size_t i = 0; i < row.cpu.size(); ++i) {
            const QPointF point(area.right() - step * double(row.cpu.size() - 1 - i), area.bottom() - area.height() * row.cpu[i] / 100.0);
            if (i == 0) line.moveTo(point);
            else line.lineTo(point);
        }
        QPainterPath under = line;
        under.lineTo(area.right(), area.bottom());
        under.lineTo(line.elementAt(0).x, area.bottom());
        under.closeSubpath();
        const QColor accent = selected ? text : opt.palette.color(QPalette::Highlight);
        QColor shade = accent;
        shade.setAlphaF(0.25);
        p->fillPath(under, shade);
        p->setPen(QPen(accent, 1.5));
        p->drawPath(line);
    }
    p->restore();

    const QRect textRect(icon.right() + 9, r.top(), std::max(0, graph.left() - icon.right() - 17), r.height());
    QFont bold = opt.font;
    bold.setBold(true);
    const QFontMetrics boldMetrics(bold);
    const int top = r.center().y() - boldMetrics.height();
    p->save();
    p->setFont(bold);
    p->setPen(text);
    p->drawText(QRect(textRect.left(), top, textRect.width(), boldMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                boldMetrics.elidedText(row.name, Qt::ElideRight, textRect.width()));
    p->setFont(opt.font);
    p->setPen(dim);
    p->drawText(QRect(textRect.left(), top + boldMetrics.height(), textRect.width(), opt.fontMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                opt.fontMetrics.elidedText(row.detail, Qt::ElideRight, textRect.width()));
    p->restore();
}
