#include "app/VmConsole.h"

#include "app/Icons.h"
#include "app/RemoteView.h"
#include "app/VmView.h"

#include <QLabel>
#include <QStackedLayout>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

using Action = LibvirtVms::Action;
using State = LibvirtVms::State;
using Vm = LibvirtVms::Vm;

VmConsole::VmConsole(LibvirtVms* vms, std::string uuid, MakeDisplay makeDisplay, QWidget* parent)
    : QWidget(parent), vms_(vms), uuid_(std::move(uuid)), makeDisplay_(std::move(makeDisplay)), stack_(new QStackedLayout(this))
{
    power_ = new QWidget;
    button_ = new QToolButton;
    button_->setIcon(icons::lucide("power"));
    button_->setIconSize(QSize(56, 56));
    button_->setFixedSize(112, 112);
    button_->setToolTip(tr("Start"));
    button_->setAccessibleName(tr("Start"));
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
        starting_ = true;
        vms_->control(uuid_, Action::Start);
        follow();
    });
    connect(vms_, &LibvirtVms::changed, this, &VmConsole::follow);
    connect(vms_, &QObject::destroyed, this, &VmConsole::closeRequested); // its host was removed
    connect(vms_, &LibvirtVms::failed, this, [this](const QString& uuid) { // the error itself: VmView's dialog
        if (uuid.toStdString() != uuid_) return;
        starting_ = false;
        follow();
    });
    follow();
}

void VmConsole::follow()
{
    const auto vm = std::find_if(vms_->vms().begin(), vms_->vms().end(), [this](const Vm& v) { return v.uuid == uuid_; });
    if (vm == vms_->vms().end()) {
        if (vms_->connected()) emit closeRequested();
        return;
    }
    if (const auto& d = vm->display) {
        starting_ = false;
        if (!display_ || d->host != shown_.host || d->port != shown_.port || d->option("tlsPort") != shown_.option("tlsPort") ||
            d->password != shown_.password)
            showDisplay(*d);
        return;
    }
    dropDisplay();
    showPower(*vm);
}

void VmConsole::showDisplay(const rdc::Connection& display)
{
    dropDisplay();
    display_ = makeDisplay_(display);
    if (!display_) return;
    shown_ = display;
    stack_->addWidget(display_);
    stack_->setCurrentWidget(display_);
    if (auto* view = display_->findChild<RemoteView*>()) {
        connect(view, &RemoteView::closeRequested, this, &VmConsole::closeRequested);
        connect(view, &RemoteView::reconnectRequested, this, [this] {
            shown_ = {};
            follow();
        });
        view->setFocus();
    }
    emit displayChanged(display.protocol);
}

void VmConsole::dropDisplay()
{
    if (!display_) return;
    stack_->removeWidget(display_);
    display_->hide();
    display_->setParent(nullptr); // MainWindow::currentView() must not find it until it is gone
    display_->deleteLater();
    display_ = nullptr;
    const rdc::Protocol protocol = shown_.protocol;
    shown_ = {};
    emit displayChanged(protocol);
}

void VmConsole::showPower(const Vm& vm)
{
    const bool control = vms_->connected() && !vms_->readOnly();
    QString status;
    switch (vm.state) {
    case State::ShutOff: status = tr("Shut off. The button starts it."); break;
    case State::Saved: status = tr("Saved. The button starts it where it was."); break;
    case State::Crashed: status = tr("Crashed. The button starts it again."); break;
    case State::ShuttingDown: status = tr("Shutting down…"); break;
    default: status = tr("%1, without a SPICE or VNC display on the network.").arg(VmView::stateText(vm.state)); break;
    }
    if (starting_) status = tr("Starting…");
    else if (!control) status = tr("Read-only access to libvirt: it cannot be started from here.");
    name_->setText(QString::fromStdString(vm.name));
    status_->setText(status);
    button_->setEnabled(control && !starting_ && LibvirtVms::allowed(Action::Start, vm.state));
    stack_->setCurrentWidget(power_);
}
