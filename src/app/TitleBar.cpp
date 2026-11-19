#include "app/TitleBar.h"

#include "app/WindowEffects.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QToolBar>
#include <QWindow>

#include <algorithm>
#include <utility>

TitleBar::TitleBar(QToolBar* bar, QWidget* parent) : QWidget(parent), bar_(bar), layout_(new QHBoxLayout(this))
{
    bar->installEventFilter(this); // its buttons move: the capsules follow
    layout_->setContentsMargins(0, 0, 8, 0);
    layout_->addWidget(bar, 0, Qt::AlignVCenter);
    layout_->addStretch();
}

bool TitleBar::movesWindow(QWidget* window, QEvent* event)
{
    if (event->type() == QEvent::MouseButtonDblClick && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton) {
        windowEffects::titleBarDoubleClicked(window);
        return true;
    }
    if (event->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent*>(event)->button() == Qt::LeftButton)
        return window->windowHandle()->startSystemMove();
    return false;
}

void TitleBar::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // its place in the window changes with the sidebar and the splitter
    for (QWidget* w = parentWidget(); w && !w->isWindow(); w = w->parentWidget()) w->installEventFilter(this);
    installEventFilter(this);
    placeBar();
}

bool TitleBar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == bar_) {
        if (event->type() == QEvent::LayoutRequest || event->type() == QEvent::Resize || event->type() == QEvent::Move) update();
        return QWidget::eventFilter(watched, event);
    }
    if (event->type() == QEvent::Move || event->type() == QEvent::Resize || event->type() == QEvent::Show) placeBar();
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

void TitleBar::placeBar()
{
    if (!window()->windowHandle()) return;
    const int clear = windowEffects::titleBarButtonsEnd(window()) + 12 - mapTo(window(), QPoint()).x();
    QMargins margins = layout_->contentsMargins();
    margins.setLeft(std::max(8, clear));
    if (margins != layout_->contentsMargins()) layout_->setContentsMargins(margins);
}

void TitleBar::mousePressEvent(QMouseEvent* event)
{
    if (!movesWindow(window(), event)) QWidget::mousePressEvent(event);
}

void TitleBar::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (!movesWindow(window(), event)) QWidget::mouseDoubleClickEvent(event);
}
