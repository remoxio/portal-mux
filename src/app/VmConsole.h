#pragma once

#include <QPointer>
#include <QWidget>

#include <functional>
#include <optional>

class QLabel;
class QStackedLayout;
class QToolButton;

// A VM's tab, from any hypervisor: its console while it runs, a power button and its state while not.
// Its actions() are its toolbar's power buttons; its windowTitle() the VM's name.
class VmConsole : public QWidget {
    Q_OBJECT

public:
    struct Status {
        QString name, state, text; // state: a word or two for the status bar; text: under the power button
        bool startable = false;
        QString display;    // what its console connects to, another one reconnects; empty: none
        bool known = true;  // false: its host is not reachable, keep what is shown
    };
    using Follow = std::function<std::optional<Status>()>; // nullopt: the VM is gone
    using Open = std::function<void(VmConsole*)>;         // makes its console and hands it to setDisplay(), now or later
    VmConsole(Follow status, std::function<void()> start, Open openDisplay, QWidget* parent = nullptr);

    void follow(); // its state may have changed
    void setDisplay(QWidget* display);
    void startFailed();
    QWidget* display() const { return display_; }
    const QString& state() const { return state_; }

signals:
    void closeRequested(); // the VM was deleted, or an error on its console was dismissed
    void changed(); // its console or its state

private:
    void dropDisplay();

    Follow statusOf_;
    std::function<void()> start_;
    Open openDisplay_;
    QStackedLayout* stack_;
    QWidget* power_;
    QToolButton* button_;
    QLabel *name_, *status_;
    QPointer<QWidget> display_; // a terminal may end itself
    QString shown_;         // the display asked for
    QString state_;
    bool starting_ = false; // the button was pressed: until it runs, or an error
};
