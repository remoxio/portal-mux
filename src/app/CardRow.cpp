#include "app/CardRow.h"

#include "app/Icons.h"
#include "app/Look.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

namespace {

constexpr int iconSize = 20, gap = 10, editSize = 16, editBox = 28;

QRectF buttonRect(const QRectF& r, size_t i) // from the right
{
    return {r.right() - 6 - editBox * qreal(i + 1), r.center().y() - editBox / 2.0, qreal(editBox), qreal(editBox)};
}

QFont small(QFont font)
{
    font.setPointSizeF(font.pointSizeF() * 0.85);
    return font;
}

constexpr int NewItemRole = Qt::UserRole + 100; // the "New …" item; its data stays empty

} // namespace

void offerNew(QComboBox* box, const QString& label, std::function<QStringList()> ids, std::function<void(QWidget*)> open,
              std::function<void(const QString& select)> refill)
{
    if (box->count() > 0) box->insertSeparator(box->count());
    box->addItem(icons::lucide("plus"), label);
    box->setItemData(box->count() - 1, true, NewItemRole);
    if (box->property("offersNew").toBool()) return; // connected when first offered
    box->setProperty("offersNew", true);
    QObject::connect(box, &QComboBox::currentIndexChanged, box, [box](int index) {
        if (!box->itemData(index, NewItemRole).toBool()) box->setProperty("chosenItem", box->itemData(index));
    });
    QObject::connect(box, &QComboBox::activated, box, [box, ids, open, refill](int index) {
        if (!box->itemData(index, NewItemRole).toBool()) return;
        const QStringList before = ids();
        QString select = box->property("chosenItem").toString();
        open(box->window());
        for (const QString& id : ids())
            if (!before.contains(id)) select = id;
        refill(select);
    });
}

int CardRow::buttonAt(const QRectF& r, QPointF pos) const
{
    for (size_t i = 0; i < buttons_.size(); ++i)
        if (buttonRect(r, i).contains(pos)) return int(i);
    return -1;
}

bool CardRow::editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option, const QModelIndex& index)
{
    const auto type = event->type();
    if (type == QEvent::MouseButtonRelease || type == QEvent::MouseButtonDblClick) {
        auto* mouse = static_cast<QMouseEvent*>(event);
        if (const int b = buttonAt(option.rect, mouse->position()); mouse->button() == Qt::LeftButton && b >= 0) {
            if (type == QEvent::MouseButtonRelease) buttons_[size_t(b)].run(index.row());
            return true; // not activated as well
        }
    }
    return QStyledItemDelegate::editorEvent(event, model, option, index);
}

bool CardRow::helpEvent(QHelpEvent* event, QAbstractItemView* view, const QStyleOptionViewItem& option, const QModelIndex& index)
{
    const int b = event->type() == QEvent::ToolTip ? buttonAt(option.rect, event->pos()) : -1;
    if (b < 0) return QStyledItemDelegate::helpEvent(event, view, option, index);
    QToolTip::showText(event->globalPos(), buttons_[size_t(b)].tip, view, buttonRect(option.rect, size_t(b)).toRect());
    return true;
}

QSize CardRow::sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const
{
    return {0, option.fontMetrics.height() + QFontMetrics(small(option.font)).height() + 14};
}

void CardRow::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    QStyleOptionViewItem o = option;
    initStyleOption(&o, index);
    const QRectF r = o.rect;
    QColor text = o.palette.color(QPalette::WindowText), dim = text;
    dim.setAlphaF(0.55);
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    const qreal textLeft = r.left() + 8 + iconSize + gap;
    if (o.state & QStyle::State_Selected) {
        look::paintSelection(p, r.adjusted(0, 1, 0, -1), o.palette, 6);
    } else if (index.row() > 0) {
        QColor line = text;
        line.setAlphaF(0.1);
        p->fillRect(QRectF(textLeft, r.top(), r.right() - textLeft - 8, 1), line);
    }
    QPixmap icon = o.icon.pixmap(QSize(iconSize, iconSize), p->device()->devicePixelRatioF());
    if (!icon.isNull()) { // in the text's colour
        QPainter tint(&icon);
        tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tint.fillRect(icon.rect(), text);
    }
    p->drawPixmap(QPointF(r.left() + 8, r.center().y() - iconSize / 2.0), icon);
    const QFont smallFont = small(o.font);
    const int nameHeight = o.fontMetrics.height(), lineHeight = QFontMetrics(smallFont).height();
    const qreal top = r.center().y() - (nameHeight + lineHeight) / 2.0, width = buttonRect(r, buttons_.size() - 1).left() - gap - textLeft;
    p->setPen(text);
    p->setFont(o.font);
    p->drawText(QRectF(textLeft, top, width, nameHeight), Qt::AlignLeft | Qt::AlignVCenter,
                o.fontMetrics.elidedText(o.text, Qt::ElideRight, int(width)));
    p->setPen(dim);
    p->setFont(smallFont);
    p->drawText(QRectF(textLeft, top + nameHeight, width, lineHeight), Qt::AlignLeft | Qt::AlignVCenter,
                QFontMetrics(smallFont).elidedText(index.data(Qt::UserRole).toString(), Qt::ElideRight, int(width)));
    for (size_t i = 0; i < buttons_.size(); ++i) {
        QPixmap glyph = icons::lucide(buttons_[i].icon).pixmap(QSize(editSize, editSize), p->device()->devicePixelRatioF());
        QPainter tint(&glyph);
        tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
        tint.fillRect(glyph.rect(), dim);
        tint.end();
        p->drawPixmap(buttonRect(r, i).center() - QPointF(editSize, editSize) / 2, glyph);
    }
    p->restore();
}
