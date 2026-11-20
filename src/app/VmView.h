#pragma once

#include "app/LibvirtVms.h"

#include <QWidget>

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
    void act(LibvirtVms::Action action);
    void editSettings();
    void editSnapshots();
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
    QAction *open_, *serial_, *vmSettings_, *snapshots_, *start_, *pause_, *shutdown_, *reboot_, *save_, *forceOff_, *reset_, *addHost_, *editHost_, *removeHost_;
};
