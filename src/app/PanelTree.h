#pragma once

#include "app/GuestRow.h"

#include <QSet>
#include <QStyledItemDelegate>

#include <functional>
#include <optional>

class QAction;
class QSettings;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;

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
// right-click menus by row: a guest's own actions, a host's or server's, and on empty space only `empty`
void rowMenus(QTreeWidget* tree, std::function<bool()> onGuest, QList<QAction*> guest, QList<QAction*> host, QList<QAction*> empty);

// Folded rows kept in the settings across restarts; a row's id is its data(0, role), empty: not kept
class Folding : public QObject {
public:
    Folding(QTreeWidget* tree, QSettings* settings, QString key, int role);
    void apply(QTreeWidgetItem* item) const; // as the user last left it, open by default

private:
    void set(QTreeWidgetItem* item, bool folded);

    QSettings* settings_;
    QString key_;
    int role_;
    QSet<QString> folded_;
};

// While a view's rows are made again: nothing painted in between, no fold animation, the scroll position kept
class Rebuild {
public:
    explicit Rebuild(QTreeView* view);
    ~Rebuild();
    Rebuild(const Rebuild&) = delete;
    Rebuild& operator=(const Rebuild&) = delete;

private:
    QTreeView* view_;
    bool animated_;
    int scroll_;
};
