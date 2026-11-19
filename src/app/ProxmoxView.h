#pragma once

#include "app/Proxmox.h"
#include "core/Session.h"

#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>
#include <vector>

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
    void updateActions();
    ProxmoxClient* selectedClient() const;
    std::optional<proxmox::Guest> selectedGuest() const;
    void act(const QString& action);
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
    QTimer poll_;
    QAction *add_, *refresh_, *console_, *start_, *shutdown_, *stop_, *reboot_, *reset_, *suspend_, *resume_, *edit_, *remove_;
};
