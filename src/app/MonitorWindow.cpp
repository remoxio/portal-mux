#include "app/MonitorWindow.h"

#include "app/RemoteView.h"

#include <QApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QScreen>

MonitorWindow::MonitorWindow(RemoteView* view, QRect part, QScreen* screen, QKeySequence leaveKey, std::function<void()> leave,
                             QWidget* parent)
    : QWidget(parent, Qt::Window | Qt::FramelessWindowHint), view_(view), part_(part), leaveKey_(std::move(leaveKey)),
      leave_(std::move(leave))
{
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setWindowTitle(view->window()->windowTitle());
    setScreen(screen); // Wayland: full screen on this output
    setGeometry(screen->geometry());
    connect(view, &RemoteView::imageUpdated, this, [this](QRect dirty) {
        const QRect visible = dirty & part_;
        if (visible.isEmpty()) return;
        const double sx = double(width()) / part_.width(), sy = double(height()) / part_.height();
        update(QRectF((visible.x() - part_.x()) * sx, (visible.y() - part_.y()) * sy, visible.width() * sx, visible.height() * sy).toAlignedRect().adjusted(-1, -1, 1, 1));
    });
}

bool MonitorWindow::event(QEvent* event)
{
    if (event->type() == QEvent::ShortcutOverride) { // every key goes remote but the one that leaves
        if (QKeySequence(static_cast<QKeyEvent*>(event)->keyCombination()) != leaveKey_) {
            event->accept();
            return true;
        }
    }
    return QWidget::event(event);
}

void MonitorWindow::paintEvent(QPaintEvent* event)
{
    QPainter painter(this);
    painter.fillRect(event->rect(), Qt::black);
    if (!view_) return;
    const QImage& image = view_->image();
    const QRect source = part_ & image.rect(); // the server may have given a smaller desktop
    if (source.isEmpty()) return;
    const double sx = double(width()) / part_.width(), sy = double(height()) / part_.height();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, !qFuzzyCompare(sx * devicePixelRatioF(), 1.0));
    painter.drawImage(QRectF((source.x() - part_.x()) * sx, (source.y() - part_.y()) * sy, source.width() * sx, source.height() * sy),
                      image, source);
}

QPoint MonitorWindow::toRemote(QPointF local) const
{
    return part_.topLeft() + QPoint(int(local.x() * part_.width() / std::max(1, width())), int(local.y() * part_.height() / std::max(1, height())));
}

void MonitorWindow::pointer(QMouseEvent* event)
{
    if (!view_) return;
    if (cursor().shape() != view_->cursor().shape() || view_->cursor().shape() == Qt::BitmapCursor) setCursor(view_->cursor());
    view_->pointerAt(toRemote(event->position()), event->buttons());
}

void MonitorWindow::wheelEvent(QWheelEvent* event)
{
    if (view_ && event->angleDelta().y() != 0) view_->pointerAt(toRemote(event->position()), event->buttons(), event->angleDelta().y());
}

void MonitorWindow::keyPressEvent(QKeyEvent* event)
{
    if (QKeySequence(event->keyCombination()) == leaveKey_) return leave_();
    if (view_) QApplication::sendEvent(view_, event);
}

void MonitorWindow::keyReleaseEvent(QKeyEvent* event)
{
    if (view_) QApplication::sendEvent(view_, event);
}
