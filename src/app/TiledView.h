#pragma once

#include <QPointer>
#include <QWidget>

#include <vector>

class QGridLayout;
class QLabel;

// Session pages side by side in one tab, each under a title bar; the last one focused is current
class TiledView : public QWidget {
    Q_OBJECT

public:
    explicit TiledView(QWidget* parent = nullptr);
    void add(QWidget* page, const QString& title);
    std::vector<QWidget*> take(); // every page, in order, no longer in here
    QWidget* current() const;

signals:
    void currentChanged();
    void emptied(); // the last page closed

private:
    struct Tile {
        QWidget* frame;
        QLabel* title;
        QWidget* page;
    };
    void relayout();
    void highlight();

    QGridLayout* grid_;
    std::vector<Tile> tiles_;
    QPointer<QWidget> current_;
};
