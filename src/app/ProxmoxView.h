#pragma once

#include "app/Proxmox.h"
#include "app/VmConsole.h"
#include "core/Session.h"

#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

class Folding;
class ProxmoxClient;
class QAction;
class QLabel;
class QSettings;
class QTreeWidget;

// Sidebar view: Proxmox servers, their guests, power actions and consoles
class ProxmoxView : public QWidget {
    Q_OBJECT

public:
    ProxmoxView(QSettings* settings, QString configDir, QWidget* parent = nullptr);

    QList<QAction*> titleActions() const;
    VmConsole* console(const QString& key); // its tab, with its power buttons; null if its server is gone
    // key: "<server id>/<node>/<type>/<vmid>"
    void requestConsole(const QString& key, QWidget* replace);

signals:
    void openRequested(const QString& key);
    void message(const QString& text);
    // tunnel: null for SPICE; the tab takes ownership.
    void consoleReady(const rdc::Connection& connection, const QString& key, QWidget* replace, const QString& address, QObject* tunnel);
    void terminalReady(const QString& key, QWidget* replace, const QString& name, const QString& address,
                       std::function<std::unique_ptr<rdc::TerminalSession>(rdc::TerminalListener&)> make);

protected:
    bool event(QEvent* event) override;

private:
    void load();
    void save() const;
    void addClient(const proxmox::Server& server);
    void editServer(ProxmoxClient* client); // null: add
    void removeServer(ProxmoxClient* client);
    void storeSecret(ProxmoxClient* client, const QString& secret);
    void fix(ProxmoxClient* client); // what its row's state asks for: log in, check the certificate, retry
    void rebuild();
    using Target = std::function<std::pair<ProxmoxClient*, std::optional<proxmox::Guest>>()>;
    struct Actions {
        QAction *console, *start, *shutdown, *stop, *reboot, *reset, *suspend, *resume;
    };
    Actions makeActions(QObject* owner, const Target& target); // for the selected row, or one guest's tab
    void updateActions();
    void updateActions(const Actions& a, const std::optional<proxmox::Guest>& g) const;
    ProxmoxClient* selectedClient() const;
    std::optional<proxmox::Guest> selectedGuest() const;
    std::pair<ProxmoxClient*, std::optional<proxmox::Guest>> find(const QString& key) const;
    void act(ProxmoxClient* client, const proxmox::Guest& guest, const QString& action);
    void refreshAll();
    struct Console {
        QPointer<ProxmoxClient> client;
        proxmox::Guest guest;
        QString key;
        QPointer<QWidget> replace;
        bool reconnect = false;
    };
    void openSpice(const Console& console);
    void openVnc(const Console& console);
    void openTerminal(const Console& console);
    void consoleFailed(const Console& console, const QString& message);
    QString consoleAddress(const Console& console) const;

    QSettings* settings_;
    QString configDir_;
    std::vector<ProxmoxClient*> clients_;
    QTreeWidget* tree_;
    QLabel* note_;
    Folding* folding_;
    QTimer poll_;
    Actions actions_; // the selected row's
    QAction *add_, *refresh_, *edit_, *remove_;
    int following_ = 0; // open tabs: poll while the view is hidden too
};
