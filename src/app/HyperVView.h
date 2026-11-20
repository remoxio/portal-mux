#pragma once

#include "app/HyperVHost.h"
#include "core/Connection.h"

#include <QWidget>

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
    void reloadServers(); // after Settings → SSH Servers changed

    static QString stateText(hyperv::State state);

signals:
    void openRequested(const rdc::Connection& console); // RDP to port 2179 through the server's SSH
    void message(const QString& text);

private:
    void save() const;
    void connectHost(const QString& id); // (again), with the server's saved password
    void addHost();
    void removeHost(HyperVHost* host);
    void rebuild();
    void updateActions();
    HyperVHost* selectedHost() const;
    const hyperv::Vm* selected() const;
    void act(hyperv::Action action);
    void open();
    void hostChanged(HyperVHost* host);
    rdc::Connection console(const HyperVHost* host, const hyperv::Vm& vm) const;

    QSettings* settings_;
    QString configDir_;
    QStringList ids_; // SSH server ids, in the list's order
    std::map<QString, HyperVHost*> hosts_;
    QTreeWidget* tree_;
    QLabel* note_;
    Folding* folding_;
    std::pair<HyperVHost*, std::string> pendingOpen_; // started by Open: its console once it runs
    QAction *open_, *start_, *pause_, *shutdown_, *reboot_, *save_, *turnOff_, *reset_, *addHost_, *removeHost_;
};
