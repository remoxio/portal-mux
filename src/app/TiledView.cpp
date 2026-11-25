#include "app/TiledView.h"

#include <QApplication>
#include <QGridLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

TiledView::TiledView(QWidget* parent) : QWidget(parent), grid_(new QGridLayout(this))
{
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setSpacing(2);
    connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
        for (const Tile& t : tiles_)
            if (now && (now == t.page || t.page->isAncestorOf(now)) && t.page != current_) {
                current_ = t.page;
                highlight();
                emit currentChanged();
            }
    });
}

void TiledView::add(QWidget* page, const QString& title)
{
    auto* frame = new QWidget;
    auto* label = new QLabel(QString(title).remove(QLatin1Char('&')));
    label->setContentsMargins(8, 3, 8, 3);
    label->setAutoFillBackground(true);
    auto* box = new QVBoxLayout(frame);
    box->setContentsMargins(0, 0, 0, 0);
    box->setSpacing(0);
    box->addWidget(label);
    box->addWidget(page, 1);
    page->show(); // the tab widget hid it
    tiles_.push_back({frame, label, page});
    connect(page, &QObject::destroyed, this, [this, page] { // the session ended or was disconnected
        const auto it = std::find_if(tiles_.begin(), tiles_.end(), [page](const Tile& t) { return t.page == page; });
        if (it == tiles_.end()) return;
        it->frame->deleteLater();
        tiles_.erase(it);
        if (current_ == page) current_ = nullptr; // QPointer clears only after destroyed()
        relayout();
        if (tiles_.empty()) return emit emptied();
        current()->setFocus();
        emit currentChanged();
    });
    if (!current_) current_ = page;
    relayout();
}

std::vector<QWidget*> TiledView::take()
{
    std::vector<QWidget*> pages;
    for (const Tile& t : tiles_) {
        disconnect(t.page, &QObject::destroyed, this, nullptr);
        t.page->setParent(nullptr);
        pages.push_back(t.page);
        delete t.frame;
    }
    tiles_.clear();
    return pages;
}

QWidget* TiledView::current() const
{
    return current_ ? current_.data() : tiles_.empty() ? nullptr : tiles_.front().page;
}

// as square as it gets: 2 side by side, 3 and 4 in two rows, the last row's last tile as wide as what is left
void TiledView::relayout()
{
    while (QLayoutItem* item = grid_->takeAt(0)) delete item; // not its widget
    const int n = int(tiles_.size());
    const int cols = std::max(1, int(std::ceil(std::sqrt(double(n)))));
    for (int i = 0; i < n; ++i) {
        const int row = i / cols, col = i % cols;
        grid_->addWidget(tiles_[size_t(i)].frame, row, col, 1, i == n - 1 ? cols - col : 1);
    }
    for (int c = 0; c < grid_->columnCount(); ++c) grid_->setColumnStretch(c, c < cols ? 1 : 0);
    for (int r = 0; r < grid_->rowCount(); ++r) grid_->setRowStretch(r, r < (n + cols - 1) / cols ? 1 : 0);
    highlight();
}

void TiledView::highlight()
{
    for (const Tile& t : tiles_) {
        const bool on = t.page == current();
        t.title->setBackgroundRole(on ? QPalette::Highlight : QPalette::Window);
        t.title->setForegroundRole(on ? QPalette::HighlightedText : QPalette::WindowText);
    }
}
