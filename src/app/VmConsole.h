#pragma once

#include "app/LibvirtVms.h"

#include <QWidget>

#include <functional>

class QLabel;
class QStackedLayout;
class QToolButton;

// A VM's console tab: its display while running, a power button while off.
// Follows the VM: reopens the display on start (or a new port), removes it on stop.
class VmConsole : public QWidget {
    Q_OBJECT

public:
    using MakeDisplay = std::function<QWidget*(const rdc::Connection&)>;
    VmConsole(LibvirtVms* vms, std::string uuid, MakeDisplay makeDisplay, QWidget* parent = nullptr);

    const std::string& uuid() const { return uuid_; }
    LibvirtVms* vms() const { return vms_; }

signals:
    void closeRequested(); // the VM was deleted, or an error on its display was dismissed
    void displayChanged(rdc::Protocol protocol);

private:
    void follow();
    void showDisplay(const rdc::Connection& display);
    void dropDisplay();
    void showPower(const LibvirtVms::Vm& vm);

    LibvirtVms* vms_;
    std::string uuid_;
    MakeDisplay makeDisplay_;
    QStackedLayout* stack_;
    QWidget* power_;
    QToolButton* button_;
    QLabel *name_, *status_;
    QWidget* display_ = nullptr;
    rdc::Connection shown_; // what display_ connects to
    bool starting_ = false; // the button was pressed; until libvirt reports it running, or an error
};
