#include "app/TitleBar.h"

#include "app/WindowEffects.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QToolBar>
#include <QWindow>

#include <utility>

TitleBar::TitleBar(QToolBar* bar, QWidget* parent) : QWidget(parent), bar_(bar), layout_(new QHBoxLayout(this))
{
    setAutoFillBackground(true);
    bar->installEventFilter(this); // its buttons move: the capsules follow
    layout_->setContentsMargins(0, 0, 8, 0);
    layout_->addWidget(bar, 0, Qt::AlignVCenter);
    layout_->addStretch();
}

bool TitleBar::movesWindow(QEvent* event)
{
    if (event->type() == QEvent::MouseButtonDblClick && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        windowEffects::titleBarDoubleClicked(window());
        return true;
    }
    if (event->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
        return window()->windowHandle()->startSystemMove();
    return false;
}

void TitleBar::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    QMargins margins = layout_->contentsMargins();
    margins.setLeft(windowEffects::titleBarButtonsEnd(window()) + 12);
    layout_->setContentsMargins(margins);
}

bool TitleBar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == bar_ && (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Resize || event->type() == QEvent::Move))
        update();
    return QWidget::eventFilter(watched, event);
}

// a capsule behind each run of buttons between separators, as spark-commander's toolbar
void TitleBar::paintEvent(QPaintEvent*)
{
    if (!bar_->isVisible()) return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QColor ink = palette().color(QPalette::WindowText);
    auto capsule = [&](QRect run) {
        if (run.isNull()) return;
        const QRectF r = QRectF(run).adjusted(-4, -3, 4, 3);
        ink.setAlphaF(0.05);
        p.setBrush(ink);
        ink.setAlphaF(0.1);
        p.setPen(QPen(ink, 1));
        p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), r.height() / 2, r.height() / 2);
    };
    QRect run;
    for (QAction* action : bar_->actions()) {
        if (action->isSeparator()) {
            capsule(std::exchange(run, QRect()));
        } else if (QWidget* button = bar_->widgetForAction(action); button && button->isVisible()) {
            run |= QRect(button->mapTo(this, QPoint()), button->size());
        }
    }
    capsule(run);
}

void TitleBar::mousePressEvent(QMouseEvent* event)
{
    if (!movesWindow(event)) QWidget::mousePressEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (!movesWindow(event)) QWidget::mouseDoubleClickEvent(event);
}
