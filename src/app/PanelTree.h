#pragma once

#include "app/GuestRow.h"

#include <QStyledItemDelegate>

#include <optional>

class QTreeView;

// Folding panels instead of an indented tree: header rows, chevrons on the right
class PanelDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    static constexpr int HeaderRole = Qt::UserRole + 10; // marks a header: its state word, may be empty
    static constexpr int HeaderColorRole = Qt::UserRole + 11;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override;
    bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option, const QModelIndex& index) override;

protected:
    virtual std::optional<GuestRow> row(const QModelIndex& index) const = 0;

private:
    bool afterDoubleClick_ = false;
};

// No indentation, animated folding; headers fold through foldLater()
void setUpPanels(QTreeView* view, PanelDelegate* delegate);
// once the mouse button is up: a release after the fold ends its animation at once
void foldLater(QTreeView* view, const QModelIndex& index);
