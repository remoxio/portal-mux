#pragma once

#include <QWidget>

class QHBoxLayout;
class QToolBar;

// macOS: the window's top, opaque, as ForkLift's title bar: `bar` in it, clear of the traffic lights; its empty part
// moves the window, a double-click zooms or minimises as the system's would
class TitleBar : public QWidget {
public:
    explicit TitleBar(QToolBar* bar, QWidget* parent = nullptr);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    bool movesWindow(QEvent* event);

    QToolBar* bar_;
    QHBoxLayout* layout_;
};
