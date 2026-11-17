#pragma once

#include <QPointer>
#include <QWidget>

#include <functional>

class RemoteView;

// One local monitor's part of a multi-monitor remote desktop, full screen on that monitor.
class MonitorWindow : public QWidget {
public:
    // part: in the remote's pixels; leave: the full-screen shortcut, typed here
    MonitorWindow(RemoteView* view, QRect part, QScreen* screen, QKeySequence leaveKey, std::function<void()> leave, QWidget* parent);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override { pointer(event); }
    void mousePressEvent(QMouseEvent* event) override { pointer(event); }
    void mouseReleaseEvent(QMouseEvent* event) override { pointer(event); }
    void wheelEvent(QWheelEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void keyReleaseEvent(QKeyEvent* event) override;

private:
    QPoint toRemote(QPointF local) const;
    void pointer(QMouseEvent* event);

    QPointer<RemoteView> view_;
    QRect part_;
    QKeySequence leaveKey_;
    std::function<void()> leave_;
};
