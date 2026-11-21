#include "app/VmConsole.h"

#include "app/Icons.h"
#include "app/RemoteView.h"
#include "terminal/TerminalView.h"

#include <QLabel>
#include <QStackedLayout>
#include <QToolButton>
#include <QVBoxLayout>

VmConsole::VmConsole(Follow status, std::function<void()> start, Open openDisplay, QWidget* parent)
    : QWidget(parent), statusOf_(std::move(status)), start_(std::move(start)), openDisplay_(std::move(openDisplay)),
      stack_(new QStackedLayout(this))
{
    power_ = new QWidget;
    button_ = new QToolButton;
    button_->setIcon(icons::lucide("power"));
    button_->setIconSize(QSize(56, 56));
    button_->setFixedSize(112, 112);
    button_->setStyleSheet(QStringLiteral(
        "QToolButton { border: 2px solid palette(mid); border-radius: 56px; background: transparent; }"
        "QToolButton:hover { border-color: palette(highlight); }"
        "QToolButton:disabled { border-color: palette(window); }"));
    name_ = new QLabel;
    QFont big; // weight and size only: the family follows the main window font
    big.setBold(true);
    big.setPointSizeF(QFont().pointSizeF() * 1.4);
    name_->setFont(big);
    status_ = new QLabel;
    status_->setWordWrap(true);
    for (QLabel* label : {name_, status_}) label->setAlignment(Qt::AlignHCenter);
    auto* layout = new QVBoxLayout(power_);
    layout->addStretch();
    layout->addWidget(button_, 0, Qt::AlignHCenter);
    layout->addSpacing(16);
    layout->addWidget(name_);
    layout->addWidget(status_);
    layout->addStretch();
    stack_->addWidget(power_);

    connect(button_, &QToolButton::clicked, this, [this] {
        if (!shown_.isEmpty()) { // it runs, its console did not open: again
            shown_.clear();
            return follow();
        }
        starting_ = true;
        start_();
        follow();
    });
}

void VmConsole::follow()
{
    const std::optional<Status> s = statusOf_();
    if (!s) return emit closeRequested();
    if (!s->known) return;
    if (s->state != state_) {
        state_ = s->state;
        emit changed();
    }
    if (!s->display.isEmpty()) {
        starting_ = false;
        if (s->display != shown_) {
            dropDisplay();
            shown_ = s->display;
            openDisplay_(this);
        }
        if (display_) return;
    } else {
        dropDisplay();
        shown_.clear();
    }
    name_->setText(s->name);
    status_->setText(starting_ ? tr("Starting…") : !shown_.isEmpty() ? tr("Running. Opening its console…") : s->text);
    button_->setEnabled(!starting_ && (s->startable || !shown_.isEmpty()));
    button_->setToolTip(shown_.isEmpty() ? tr("Start") : tr("Open its console"));
    button_->setAccessibleName(button_->toolTip());
    stack_->setCurrentWidget(power_);
}

void VmConsole::startFailed()
{
    starting_ = false;
    follow();
}

void VmConsole::setDisplay(QWidget* display)
{
    if (!display) return;
    dropDisplay();
    display_ = display;
    stack_->addWidget(display_);
    stack_->setCurrentWidget(display_);
    auto again = [this] {
        shown_.clear();
        follow();
    };
    if (auto* view = display_->findChild<RemoteView*>()) {
        connect(view, &RemoteView::closeRequested, this, &VmConsole::closeRequested);
        connect(view, &RemoteView::reconnectRequested, this, again);
        view->setFocus();
    } else if (auto* terminal = qobject_cast<TerminalView*>(display_)) {
        connect(terminal, &TerminalView::closeRequested, this, &VmConsole::closeRequested);
        connect(terminal, &TerminalView::reconnectRequested, this, again);
        terminal->setFocus();
    }
    emit changed();
}

void VmConsole::dropDisplay()
{
    if (!display_) return;
    stack_->removeWidget(display_);
    display_->hide();
    display_->setParent(nullptr); // MainWindow::currentView() must not find it until it is gone
    display_->deleteLater();
    display_ = nullptr;
    emit changed();
}
