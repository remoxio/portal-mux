#pragma once

#include "app/HyperVHost.h"
#include "app/VmConsole.h"
#include "core/Connection.h"

#include <QWidget>

#include <functional>
#include <map>
#include <string>

class Folding;
class QAction;
class QLabel;
class QSettings;
class QTreeWidget;

// Sidebar view: Hyper-V hosts (saved SSH servers), their VMs, power actions and consoles
class HyperVView : public QWidget {
    Q_OBJECT

public:
    HyperVView(QSettings* settings, QString configDir, QWidget* parent = nullptr);

    QList<QAction*> titleActions() const;
    // its tab, with its power buttons; open: its console, through the server's SSH
    VmConsole* console(const QString& hostId, const std::string& vmId, std::function<void(VmConsole*, const rdc::Connection&)> open);
    void reloadServers(); // after Settings → SSH Servers changed

    static QString stateText(hyperv::State state);

signals:
    void openRequested(const QString& hostId, const std::string& vmId);
    void message(const QString& text);
    void changed(); // a host's VMs or status
    void hostFailed(const QString& hostId);

private:
    void save() const;
    void connectHost(const QString& id); // (again), with the server's saved password
    void addHost();
    void removeHost(HyperVHost* host);
    void rebuild();
    using Target = std::function<std::pair<HyperVHost*, const hyperv::Vm*>()>;
    struct Actions {
        QAction *open, *start, *pause, *shutdown, *reboot, *save, *turnOff, *reset;
    };
    Actions makeActions(QObject* owner, const Target& target); // for the selected row, or one VM's tab
    void updateActions();
    void updateActions(const Actions& a, const HyperVHost* host, const hyperv::Vm* vm) const;
    HyperVHost* selectedHost() const;
    const hyperv::Vm* selected() const;
    std::pair<HyperVHost*, const hyperv::Vm*> find(const QString& hostId, const std::string& vmId) const;
    void act(HyperVHost* host, const hyperv::Vm& vm, hyperv::Action action);
    void hostChanged(HyperVHost* host);
    rdc::Connection connection(const HyperVHost* host, const hyperv::Vm& vm) const; // RDP to port 2179 through the server's SSH

    QSettings* settings_;
    QString configDir_;
    QStringList ids_; // SSH server ids, in the list's order
    std::map<QString, HyperVHost*> hosts_;
    QTreeWidget* tree_;
    QLabel* note_;
    Folding* folding_;
    Actions actions_; // the selected row's
    QAction *addHost_, *removeHost_;
};
