#pragma once

#include "app/Azure.h"
#include "app/VmConsole.h"
#include "core/Connection.h"

#include <QTimer>
#include <QWidget>

#include <functional>
#include <vector>

class AzureClient;
class Folding;
class QAction;
class QLabel;
class QSettings;
class QTreeWidget;

// Sidebar view: Azure accounts, their subscriptions' VMs, power actions; consoles over RDP or SSH to their address
class AzureView : public QWidget {
    Q_OBJECT

public:
    explicit AzureView(QSettings* settings, QWidget* parent = nullptr);

    QList<QAction*> titleActions() const;
    // key: "<account id>/<the VM's key>"; open: its console, the serial console, from its websocket and token
    using Serial = std::function<void(const QString& name, const QString& address, const QUrl& url, const QString& token)>;
    VmConsole* console(const QString& key, std::function<void(VmConsole*, const QString& name, const QString& address, const QUrl& url,
                                                              const QString& token)> open);

signals:
    void openRequested(const QString& key);
    void connectRequested(const rdc::Connection& connection); // RDP or SSH to a VM, a session of its own
    void message(const QString& text);

protected:
    bool event(QEvent* event) override;

private:
    void load();
    void save() const;
    AzureClient* addClient(const azure::Account& account);
    void editAccount(AzureClient* client); // null: add
    void removeAccount(AzureClient* client);
    void fix(AzureClient* client); // what its row's state asks for: sign in, retry
    void rebuild();
    using Target = std::function<std::pair<AzureClient*, const azure::Vm*>()>;
    struct Actions {
        QAction *open, *remote, *start, *restart, *deallocate, *powerOff, *portal;
    };
    Actions makeActions(QObject* owner, const Target& target); // for the selected row, or one VM's tab
    void updateActions();
    void updateActions(const Actions& a, const azure::Vm* vm) const;
    AzureClient* selectedClient() const;
    const azure::Vm* selectedVm() const;
    std::pair<AzureClient*, const azure::Vm*> find(const QString& key) const;
    void act(AzureClient* client, const azure::Vm& vm, const QString& action);
    void serialConsole(const QString& key, Serial ready); // a failure is shown here

    QSettings* settings_;
    std::vector<AzureClient*> clients_;
    QTreeWidget* tree_;
    QLabel* note_;
    Folding* folding_;
    QTimer poll_;
    Actions actions_; // the selected row's
    QAction *add_, *signIn_, *refresh_, *edit_, *remove_;
    int following_ = 0; // open tabs: poll while the view is hidden too
};
