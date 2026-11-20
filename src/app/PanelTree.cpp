#include "app/PanelTree.h"

#include "app/Icons.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QSettings>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>

#include <utility>

static constexpr int gutter = 22; // the chevrons' column, kept by every row

static QRect chevronRect(const QRect& row) { return {row.right() - gutter + 1, row.center().y() - 8, 16, 16}; }

static bool keepsGutter(const QModelIndex& index)
{
    return index.parent().isValid() || index.data(PanelDelegate::HeaderRole).isValid() || index.model()->hasChildren(index);
}

QSize PanelDelegate::sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    return row(index) ? guestRowSize(option) : QSize(160, option.fontMetrics.height() + 12);
}

void PanelDelegate::paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const
{
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    const bool selected = opt.state & QStyle::State_Selected, header = index.data(HeaderRole).isValid();
    if (header && !selected) {
        QColor band = opt.palette.color(QPalette::Text);
        band.setAlphaF(0.06);
        p->fillRect(opt.rect, band);
    }
    const QWidget* widget = opt.widget;
    (widget ? widget->style() : QApplication::style())->drawPrimitive(QStyle::PE_PanelItemViewItem, &opt, p, widget);
    const QIcon::Mode mode = selected ? QIcon::Selected : QIcon::Normal;
    if (const auto* view = qobject_cast<const QTreeView*>(parent()); view && index.model()->hasChildren(index)) {
        p->save();
        p->setOpacity(selected ? 1.0 : 0.7);
        icons::lucide(view->isExpanded(index) ? "chevron-up" : "chevron-down").paint(p, chevronRect(opt.rect), Qt::AlignCenter, mode);
        p->restore();
    }
    const int right = keepsGutter(index) ? gutter : 0;
    if (const std::optional<GuestRow> guest = row(index)) {
        opt.rect.setRight(opt.rect.right() - right);
        return paintGuestRow(p, opt, *guest);
    }
    QRect r = opt.rect.adjusted(6, 0, -right - 6, 0); // ends where the graphs end
    QColor text = opt.palette.color(QPalette::Normal, selected ? QPalette::HighlightedText : QPalette::Text);
    p->save();
    if (header) {
        opt.icon.paint(p, QRect(r.left(), r.center().y() - 8, 16, 16), Qt::AlignCenter, mode);
        const QString status = index.data(HeaderRole).toString();
        p->setPen(selected ? text : index.data(HeaderColorRole).value<QColor>());
        p->drawText(r, Qt::AlignRight | Qt::AlignVCenter, status);
        r.adjust(24, 0, status.isEmpty() ? 0 : -opt.fontMetrics.horizontalAdvance(status) - 8, 0);
    } else { // a hint
        if (!selected) text.setAlphaF(0.6);
        r.adjust(24, 0, 0, 0);
    }
    p->setPen(text);
    p->drawText(r, Qt::AlignLeft | Qt::AlignVCenter, opt.fontMetrics.elidedText(opt.text, Qt::ElideRight, r.width()));
    p->restore();
}

void foldLater(QTreeView* view, const QModelIndex& index)
{
    QTimer::singleShot(QGuiApplication::mouseButtons() == Qt::NoButton ? 0 : 10, view, [view, index = QPersistentModelIndex(index)] {
        if (QGuiApplication::mouseButtons() != Qt::NoButton) return foldLater(view, index);
        if (index.isValid()) view->setExpanded(index, !view->isExpanded(index));
    });
}

bool PanelDelegate::editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option, const QModelIndex& index)
{
    auto* view = qobject_cast<QTreeView*>(parent());
    const bool release = event->type() == QEvent::MouseButtonRelease, twice = event->type() == QEvent::MouseButtonDblClick;
    if (!view || !(release || twice || event->type() == QEvent::MouseButtonPress) || !model->hasChildren(index) ||
        !chevronRect(option.rect).adjusted(-3, -8, 3, 8).contains(static_cast<QMouseEvent*>(event)->position().toPoint()))
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    if (twice) afterDoubleClick_ = true; // Qt sends that click's release too
    else if (release && !std::exchange(afterDoubleClick_, false) && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
        foldLater(view, index);
    return true; // a double click on it folds once and opens nothing
}

namespace {

// keys fold at once: the animation is for the pointer
class InstantKeys : public QObject {
public:
    using QObject::QObject;

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        auto* view = static_cast<QTreeView*>(watched);
        if (event->type() == QEvent::KeyPress && view->isAnimated()) {
            view->setAnimated(false);
            QTimer::singleShot(0, view, [view] { view->setAnimated(true); });
        }
        return false;
    }
};

} // namespace

void setUpPanels(QTreeView* view, PanelDelegate* delegate)
{
    view->setItemDelegate(delegate);
    view->setIndentation(0);
    view->setRootIsDecorated(false);
    view->setExpandsOnDoubleClick(false); // Qt's folds before the release, cutting the animation
    view->setAnimated(view->style()->styleHint(QStyle::SH_Widget_Animation_Duration, nullptr, view) > 0); // 0: KDE's animations set to instant
    view->installEventFilter(new InstantKeys(view));
}

Folding::Folding(QTreeWidget* tree, QSettings* settings, QString key, int role)
    : QObject(tree), settings_(settings), key_(std::move(key)), role_(role)
{
    for (const QString& id : settings_->value(key_).toStringList()) folded_.insert(id);
    connect(tree, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem* item) { set(item, true); });
    connect(tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem* item) { set(item, false); });
}

void Folding::apply(QTreeWidgetItem* item) const { item->setExpanded(!folded_.contains(item->data(0, role_).toString())); }

void Folding::set(QTreeWidgetItem* item, bool folded)
{
    const QString id = item->data(0, role_).toString();
    if (id.isEmpty() || folded_.contains(id) == folded) return; // rebuilds open what is open anyway
    if (folded) folded_.insert(id);
    else folded_.remove(id);
    QStringList list(folded_.begin(), folded_.end());
    list.sort();
    settings_->setValue(key_, list);
}

Rebuild::Rebuild(QTreeView* view) : view_(view), animated_(view->isAnimated()), scroll_(view->verticalScrollBar()->value())
{
    view_->setUpdatesEnabled(false);
    view_->setAnimated(false);
}

Rebuild::~Rebuild()
{
    view_->doItemsLayout(); // the scroll range of the new rows
    view_->verticalScrollBar()->setValue(scroll_);
    view_->setAnimated(animated_);
    view_->setUpdatesEnabled(true);
}
