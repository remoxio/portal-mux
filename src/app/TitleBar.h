#pragma once

#include <QWidget>

class QHBoxLayout;
class QToolBar;

// macOS: the work area's top as the window's title bar, as spark-commander's: `bar` in it, clear of the traffic
// lights; its empty part moves the window, a double-click zooms or minimises as the system's would
class TitleBar : public QWidget {
public:
    explicit TitleBar(QToolBar* bar, QWidget* parent = nullptr);

    // A press there moves the window; true if it did
    static bool movesWindow(QWidget* window, QEvent* event);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    void placeBar();

    QToolBar* bar_;
    QHBoxLayout* layout_;
};
