#pragma once

#include "app/LibvirtVms.h"
#include "app/VmConsole.h"

#include <QWidget>

#include <functional>
#include <vector>

class Folding;
class QAction;
class QLabel;
class QSettings;
class QTreeWidget;

// KVM view: local libvirt VMs with state and CPU graph
class VmView : public QWidget {
    Q_OBJECT

public:
    explicit VmView(QSettings* settings, QWidget* parent = nullptr);

    QList<QAction*> titleActions() const;
    VmConsole* console(LibvirtVms* host, const std::string& uuid, VmConsole::Open openDisplay); // its tab, with its power buttons

    static QString stateText(LibvirtVms::State state);

signals:
    void openRequested(LibvirtVms* host, const std::string& uuid);
    void serialRequested(LibvirtVms* host, const std::string& uuid);
    void message(const QString& text);
    void vmsChanged();

private:
    void rebuild();
    void updateActions();
    LibvirtVms* selectedHost() const; // of the selected VM or host row
    const LibvirtVms::Vm* selected() const;
    using Target = std::function<std::pair<LibvirtVms*, const LibvirtVms::Vm*>()>;
    struct Actions {
        QAction *open, *serial, *settings, *snapshots, *start, *pause, *shutdown, *reboot, *save, *forceOff, *reset;
    };
    Actions makeActions(QObject* owner, const Target& target); // for the selected row, or one VM's tab
    void updateActions(const Actions& a, LibvirtVms* host, const LibvirtVms::Vm* vm) const;
    void act(LibvirtVms* host, const LibvirtVms::Vm& vm, LibvirtVms::Action action);
    void editSettings(LibvirtVms* host, const std::string& uuid);
    void editSnapshots(LibvirtVms* host, const std::string& uuid);
    void addHost(const QString& name, const std::string& uri);
    void editHost(LibvirtVms* host); // null: a new one
    void removeHost(LibvirtVms* host, bool ask = true);
    void save() const;

    QSettings* settings_;
    std::vector<LibvirtVms*> hosts_; // this computer first, if local_
    bool local_ = true;
    QTreeWidget* tree_;
    QLabel* note_; // no hosts yet
    Folding* folding_;
    Actions actions_; // the selected row's
    QAction *addHost_, *editHost_, *removeHost_;
};
