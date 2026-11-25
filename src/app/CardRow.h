#pragma once

#include <QStringList>
#include <QStyledItemDelegate>

#include <functional>
#include <vector>

class QComboBox;

// a choice list's last items: a separator and `label`, which runs open(the box's window), then
// refill(the id that ids() gained meanwhile, else the one chosen before)
void offerNew(QComboBox* box, const QString& label, std::function<QStringList()> ids, std::function<void(QWidget*)> open,
              std::function<void(const QString& select)> refill);

// A settings list's row: icon, name over Qt::UserRole's line, a line between rows, icon buttons on the right
class CardRow : public QStyledItemDelegate {
public:
    struct Button {
        const char* icon; // Lucide
        QString tip;
        std::function<void(int row)> run;
    };
    CardRow(std::vector<Button> buttons, QObject* parent) : QStyledItemDelegate(parent), buttons_(std::move(buttons)) {}

    bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option, const QModelIndex& index) override;
    bool helpEvent(QHelpEvent* event, QAbstractItemView* view, const QStyleOptionViewItem& option, const QModelIndex& index) override;
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override;
    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override;

private:
    int buttonAt(const QRectF& r, QPointF pos) const;

    std::vector<Button> buttons_;
};
