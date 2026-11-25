#include "app/AzureView.h"
#include "app/Prompts.h"

#include "app/AzureClient.h"
#include "app/Icons.h"
#include "app/PanelTree.h"
#include "app/Secrets.h"

#include <QAction>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>

using azure::Account;
using azure::Vm;
using Problem = AzureClient::Problem;

static constexpr int KeyRole = Qt::UserRole;
static constexpr int RowRole = Qt::UserRole + 1; // a GuestRow: subscriptions and VMs

static std::string keychainKey(const std::string& id) { return "azure/" + id; }

static const QColor green(0x27, 0xae, 0x60), orange(0xf6, 0x74, 0x00), red(0xda, 0x44, 0x53);

static QString powerText(const Vm& vm)
{
    if (vm.power == "running") return AzureView::tr("Running");
    if (vm.power == "deallocated") return AzureView::tr("Deallocated");
    if (vm.power == "stopped") return AzureView::tr("Stopped (allocated)");
    if (vm.power == "starting") return AzureView::tr("Starting");
    if (vm.power == "stopping") return AzureView::tr("Stopping");
    if (vm.power == "deallocating") return AzureView::tr("Deallocating");
    return vm.power.empty() ? AzureView::tr("State unknown") : QString::fromStdString(vm.power);
}

static std::string address(const Vm& vm) { return vm.publicIp.empty() ? vm.privateIp : vm.publicIp; }

static QString accountName(const Account& a)
{
    return QString::fromStdString(!a.name.empty() ? a.name : !a.user.empty() ? a.user : "Azure");
}

// RDP for Windows, SSH for Linux
static rdc::Connection connectionTo(const AzureClient* client, const Vm& vm)
{
    rdc::Connection c;
    c.protocol = vm.os == "Windows" ? rdc::Protocol::RDP : rdc::Protocol::SSH;
    c.name = vm.name;
    c.host = address(vm);
    c.username = vm.admin;
    c.options["address"] = QStringLiteral("%1 · %2").arg(accountName(client->account()), QString::fromStdString(address(vm))).toStdString();
    return c;
}

namespace {

struct AccountState {
    QString word, hint; // the hint row while nothing is listed
    QColor color;
};

AccountState accountState(const AzureClient* c, const QPalette& palette)
{
    const QColor grey = palette.color(QPalette::Disabled, QPalette::Text);
    if (c->signingIn()) return {AzureView::tr("Signing in"), AzureView::tr("Finish signing in in your browser (double-click: its page again)"), orange};
    if (!c->signedIn()) return {AzureView::tr("Sign in"), AzureView::tr("Double-click to sign in"), grey};
    switch (c->problem()) {
    case Problem::SignIn: return {AzureView::tr("Sign in"), AzureView::tr("Double-click to sign in again"), red};
    case Problem::Unreachable: return {AzureView::tr("Unreachable"), AzureView::tr("Double-click to retry"), red};
    case Problem::Other: return {AzureView::tr("Error"), AzureView::tr("Double-click to retry"), red};
    case Problem::None: break;
    }
    if (!c->loaded()) return {AzureView::tr("Connecting…"), {}, grey};
    if (c->subscriptions().empty()) return {{}, AzureView::tr("No subscriptions in this directory"), grey};
    return {};
}

class AzureDelegate : public PanelDelegate {
public:
    using PanelDelegate::PanelDelegate;

protected:
    std::optional<GuestRow> row(const QModelIndex& index) const override
    {
        const QVariant row = index.data(RowRole);
        return row.isValid() ? std::optional(row.value<GuestRow>()) : std::nullopt;
    }
};

} // namespace

AzureView::AzureView(QSettings* settings, QWidget* parent) : QWidget(parent), settings_(settings)
{
    tree_ = new QTreeWidget;
    tree_->setHeaderHidden(true);
    tree_->setFrameShape(QFrame::NoFrame);
    setUpPanels(tree_, new AzureDelegate(tree_));
    folding_ = new Folding(tree_, settings_, QStringLiteral("folded/azure"), KeyRole);
    note_ = new QLabel;
    note_->setWordWrap(true);
    note_->setContentsMargins(10, 6, 10, 6);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(note_);
    layout->addWidget(tree_, 1);

    auto make = [this](const char* icon, const QString& text, std::function<void()> run) {
        auto* a = new QAction(icons::lucide(icon), text, this);
        connect(a, &QAction::triggered, this, run);
        return a;
    };
    add_ = make("plus", tr("&Add Account…"), [this] { editAccount(nullptr); });
    signIn_ = make("log-in", tr("&Sign In…"), [this] {
        if (AzureClient* c = selectedClient()) c->signIn();
    });
    signIn_->setToolTip(tr("Signs in in your web browser"));
    refresh_ = make("refresh-cw", tr("Re&fresh"), [this] {
        for (AzureClient* c : clients_)
            if (c->signedIn()) c->refresh();
    });
    edit_ = make("pencil", tr("&Edit Account…"), [this] {
        if (AzureClient* c = selectedClient()) editAccount(c);
    });
    remove_ = make("trash", tr("Re&move Account"), [this] {
        if (AzureClient* c = selectedClient()) removeAccount(c);
    });
    actions_ = makeActions(this, [this] { return std::pair(selectedClient(), selectedVm()); });
    const Actions& a = actions_;
    auto separator = [this] {
        auto* s = new QAction(this);
        s->setSeparator(true);
        return s;
    };
    rowMenus(tree_, [this] { return selectedVm() != nullptr; },
             {a.open, a.remote, separator(), a.start, a.restart, a.deallocate, a.powerOff, separator(), a.portal},
             {signIn_, refresh_, edit_, remove_, separator(), add_}, {refresh_, add_});
    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        if (selectedVm()) return actions_.open->trigger();
        if (AzureClient* c = selectedClient(); c && (c->signingIn() || !c->signedIn() || c->problem() != Problem::None)) return fix(c);
        foldLater(tree_, tree_->indexFromItem(item));
    });
    connect(tree_, &QTreeWidget::currentItemChanged, this, qOverload<>(&AzureView::updateActions));

    poll_.setInterval(15000);
    connect(&poll_, &QTimer::timeout, this, [this] {
        if (!isVisible() && !following_) return;
        for (AzureClient* c : clients_)
            if (c->signedIn() && c->problem() != Problem::SignIn) c->refresh();
    });
    poll_.start();
    load();
    rebuild();
}

QList<QAction*> AzureView::titleActions() const { return {add_}; }

AzureView::Actions AzureView::makeActions(QObject* owner, const Target& target)
{
    auto make = [this, owner, target](const char* icon, const QString& text, std::function<void(AzureClient*, const Vm&)> run) {
        auto* a = new QAction(icons::lucide(icon), text, owner);
        connect(a, &QAction::triggered, this, [target, run] {
            const auto [client, vm] = target();
            if (client && vm) run(client, *vm);
        });
        return a;
    };
    auto control = [this](const char* action) {
        return [this, action](AzureClient* client, const Vm& vm) { act(client, vm, QLatin1String(action)); };
    };
    Actions a;
    a.open = make("monitor", tr("&Console"), [this](AzureClient* client, const Vm& vm) {
        emit openRequested(QString::fromStdString(client->account().id + "/" + vm.key()));
    });
    a.open->setToolTip(tr("Its tab: its serial console while it runs or starts, as the Azure portal's, also when its network is broken "
                          "(needs boot diagnostics, and a password login in the VM); a power button while it is stopped"));
    a.remote = make("grid-2x2", tr("Remote &Desktop"), [this](AzureClient* client, const Vm& vm) { emit connectRequested(connectionTo(client, vm)); });
    a.remote->setToolTip(tr("Remote Desktop to a Windows VM, SSH to a Linux one, in a tab of its own: at its public address, or its "
                            "private one (through a VPN); Azure lets it in only where the VM's network security group allows it"));
    a.start = make("play", tr("&Start"), control("start"));
    a.restart = make("rotate-cw", tr("&Restart"), control("restart"));
    a.deallocate = make("power", tr("S&top"), control("deallocate"));
    a.deallocate->setToolTip(tr("Shuts it down and gives up its hardware, as the Azure portal's Stop: Azure no longer charges for its compute; "
                                "a dynamic public IP address goes"));
    a.powerOff = make("power-off", tr("Stop, &Keep Allocated"), control("powerOff"));
    a.powerOff->setToolTip(tr("Shuts it down but keeps its hardware and addresses: Azure still charges for it"));
    a.portal = make("globe", tr("Open in Azure &Portal"), [](AzureClient* client, const Vm& vm) {
        const std::string& tenant = client->account().tenantId;
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://portal.azure.com/#%1resource%2")
                                           .arg(tenant.empty() ? QString() : QStringLiteral("@%1/").arg(QString::fromStdString(tenant)),
                                                QString::fromStdString(vm.id))));
    });
    a.portal->setToolTip(tr("Its page in the Azure portal, in your web browser"));
    return a;
}

std::pair<AzureClient*, const Vm*> AzureView::find(const QString& key) const
{
    const std::string id = key.section(QLatin1Char('/'), 0, 0).toStdString();
    const auto client = std::find_if(clients_.begin(), clients_.end(), [&](AzureClient* c) { return c->account().id == id; });
    if (client == clients_.end()) return {nullptr, nullptr};
    return {*client, (*client)->vm(key.section(QLatin1Char('/'), 1).toStdString())};
}

VmConsole* AzureView::console(const QString& key, std::function<void(VmConsole*, const QString& name, const QString& address, const QUrl& url,
                                                                       const QString& token)> open)
{
    AzureClient* client = find(key).first;
    if (!client) return nullptr;
    auto status = [this, key]() -> std::optional<VmConsole::Status> {
        const auto [client, vm] = find(key);
        VmConsole::Status s;
        if (!client) return std::nullopt; // its account was removed
        if (!vm) { // deleted, or not listed yet
            if (client->loaded()) return std::nullopt;
            s.known = false;
            return s;
        }
        s.name = QString::fromStdString(vm->name);
        s.state = powerText(*vm);
        s.startable = vm->power == "deallocated" || vm->power == "stopped";
        const bool on = vm->power == "running" || vm->power == "starting"; // the serial console shows it boot
        s.text = vm->power == "deallocated" ? tr("Deallocated: Azure doesn't charge for its compute. The button starts it.")
                 : vm->power == "stopped"   ? tr("Stopped but allocated: Azure still charges for it. The button starts it.")
                 : on && !vm->bootDiagnostics
                     ? tr("%1. Its serial console needs boot diagnostics: the Azure portal → the VM → Boot diagnostics → Settings. "
                          "%2: on the toolbar.").arg(s.state, vm->os == "Windows" ? tr("Remote Desktop") : tr("SSH"))
                     : s.state;
        if (on && vm->bootDiagnostics) s.display = QStringLiteral("serial");
        return s;
    };
    auto self = std::make_shared<QPointer<VmConsole>>();
    auto start = [this, key, self] {
        const auto [client, vm] = find(key);
        if (!client || !vm) return;
        client->control(*vm, QStringLiteral("start"), [this, self, name = vm->name](const QString& error) {
            emit message(tr("%1: %2").arg(QString::fromStdString(name), error));
            if (*self) (*self)->startFailed();
        });
    };
    auto* console = new VmConsole(status, start, [this, key, open](VmConsole* c) {
        serialConsole(key, [c = QPointer<VmConsole>(c), open](const QString& name, const QString& address, const QUrl& url, const QString& token) {
            if (c) open(c, name, address, url, token);
        });
    });
    *self = console;
    if (const Vm* vm = find(key).second) console->setWindowTitle(QString::fromStdString(vm->name));
    connect(client, &AzureClient::changed, console, &VmConsole::follow);
    connect(client, &QObject::destroyed, console, &VmConsole::closeRequested);
    const Actions a = makeActions(console, [this, key] { return find(key); });
    auto* separator = new QAction(console);
    separator->setSeparator(true);
    console->addActions({a.start, a.restart, a.deallocate, a.powerOff, separator, a.remote, a.portal});
    auto update = [this, a, key] { updateActions(a, find(key).second); };
    connect(client, &AzureClient::changed, console, update);
    update();
    ++following_;
    connect(console, &QObject::destroyed, this, [this] { --following_; });
    return console;
}

bool AzureView::event(QEvent* event)
{
    if (event->type() == QEvent::Show)
        for (AzureClient* c : clients_)
            if (c->signedIn()) c->refresh();
    return QWidget::event(event);
}

void AzureView::load()
{
    const int n = settings_->beginReadArray(QStringLiteral("azure"));
    for (int i = 0; i < n; ++i) {
        settings_->setArrayIndex(i);
        Account a;
        a.id = settings_->value(QStringLiteral("id")).toString().toStdString();
        a.name = settings_->value(QStringLiteral("name")).toString().toStdString();
        a.tenant = settings_->value(QStringLiteral("tenant"), QStringLiteral("organizations")).toString().toStdString();
        a.clientId = settings_->value(QStringLiteral("clientId")).toString().toStdString();
        a.user = settings_->value(QStringLiteral("user")).toString().toStdString();
        a.tenantId = settings_->value(QStringLiteral("tenantId")).toString().toStdString();
        if (a.id.empty()) continue;
        AzureClient* client = addClient(a);
        secrets::read(keychainKey(a.id), client, [this, client](QString token) {
            if (token.isEmpty() || client->signedIn()) return;
            client->setRefreshToken(token);
            client->refresh();
            rebuild();
        });
    }
    settings_->endArray();
}

void AzureView::save() const
{
    settings_->remove(QStringLiteral("azure"));
    settings_->beginWriteArray(QStringLiteral("azure"), int(clients_.size()));
    for (size_t i = 0; i < clients_.size(); ++i) {
        settings_->setArrayIndex(int(i));
        const Account& a = clients_[i]->account();
        settings_->setValue(QStringLiteral("id"), QString::fromStdString(a.id));
        settings_->setValue(QStringLiteral("name"), QString::fromStdString(a.name));
        settings_->setValue(QStringLiteral("tenant"), QString::fromStdString(a.tenant));
        settings_->setValue(QStringLiteral("clientId"), QString::fromStdString(a.clientId));
        settings_->setValue(QStringLiteral("user"), QString::fromStdString(a.user));
        settings_->setValue(QStringLiteral("tenantId"), QString::fromStdString(a.tenantId));
    }
    settings_->endArray();
}

AzureClient* AzureView::addClient(const Account& account)
{
    auto* client = new AzureClient(account, this);
    clients_.push_back(client);
    connect(client, &AzureClient::changed, this, &AzureView::rebuild);
    connect(client, &AzureClient::tokenChanged, this, [this, client](const QString& token) {
        const std::string key = keychainKey(client->account().id);
        save(); // who signed in
        if (token.isEmpty()) return secrets::remove(key);
        secrets::write(key, token, this, [this](QString error) {
            if (!error.isEmpty()) emit message(tr("Azure: the sign-in is not kept: %1").arg(error));
        });
    });
    return client;
}

void AzureView::editAccount(AzureClient* client)
{
    const Account old = client ? client->account() : Account{};
    QDialog dialog(this);
    dialog.setWindowTitle(client ? tr("Edit Azure Account") : tr("Add Azure Account"));
    auto* name = new QLineEdit(QString::fromStdString(old.name));
    name->setPlaceholderText(tr("Who signs in"));
    auto* tenant = new QLineEdit(QString::fromStdString(old.tenant));
    auto* app = new QLineEdit(QString::fromStdString(old.clientId));
    app->setPlaceholderText(tr("Azure CLI's"));
    auto* hint = new QLabel(tr("You sign in in your web browser. Directory: \"organizations\" for your work account's own, or a directory's "
                               "ID or domain (guest and personal accounts). Application ID: empty for Azure CLI's, or your own "
                               "(a public client, redirect URI http://localhost)."));
    hint->setWordWrap(true);
    auto* form = new QFormLayout;
    form->addRow(tr("&Name:"), name);
    form->addRow(tr("&Directory:"), tenant);
    form->addRow(tr("&Application ID:"), app);
    form->addRow(hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttons->button(QDialogButtonBox::Ok)->setText(client ? tr("OK") : tr("Sign In"));
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(tenant, &QLineEdit::textChanged, &dialog, [&] { buttons->button(QDialogButtonBox::Ok)->setEnabled(!tenant->text().trimmed().isEmpty()); });
    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(buttons);
    dialog.resize(520, std::max(dialog.sizeHint().height(), dialog.heightForWidth(520))); // the hint wrapped
    if (dialog.exec() != QDialog::Accepted) return;

    Account a = old;
    if (a.id.empty()) a.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    a.name = name->text().trimmed().toStdString();
    a.tenant = tenant->text().trimmed().toStdString();
    a.clientId = app->text().trimmed().toStdString();
    if (!client) client = addClient(a);
    else client->setAccount(a);
    save();
    if (!client->signedIn()) client->signIn();
    rebuild();
}

void AzureView::removeAccount(AzureClient* client)
{
    if (!prompts::confirm(this, tr("Remove Account"), tr("Remove \"%1\" from the list? Nothing changes in Azure.").arg(accountName(client->account())),
                          tr("&Remove")))
        return;
    secrets::remove(keychainKey(client->account().id));
    std::erase(clients_, client);
    client->deleteLater();
    save();
    rebuild();
}

void AzureView::fix(AzureClient* client)
{
    if (client->signingIn() || !client->signedIn() || client->problem() == Problem::SignIn) return client->signIn();
    client->refresh();
}

void AzureView::rebuild()
{
    const Rebuild quiet(tree_);
    const QTreeWidgetItem* current = tree_->currentItem();
    const QString keep = current ? current->data(0, KeyRole).toString() : QString();
    tree_->clear();
    for (AzureClient* client : clients_) {
        const Account& a = client->account();
        const QString id = QString::fromStdString(a.id);
        const AccountState state = accountState(client, palette());
        auto* account = new QTreeWidgetItem(tree_, {accountName(a)});
        account->setData(0, KeyRole, QStringLiteral("account:") + id);
        account->setIcon(0, icons::brand("azure-line"));
        account->setData(0, PanelDelegate::HeaderRole, state.word);
        account->setData(0, PanelDelegate::HeaderColorRole, state.color);
        QString tip = a.user.empty() ? tr("Not signed in") : QString::fromStdString(a.user);
        tip += QStringLiteral(" · ") + QString::fromStdString(a.tenantId.empty() ? a.tenant : a.tenantId);
        if (!client->lastError().isEmpty()) tip += QLatin1Char('\n') + client->lastError();
        account->setToolTip(0, tip);
        if (!state.hint.isEmpty()) {
            auto* hint = new QTreeWidgetItem(account, {state.hint});
            hint->setData(0, KeyRole, QStringLiteral("hint:") + id);
            hint->setToolTip(0, tip);
        }
        if (client->problem() != Problem::None || client->signingIn()) continue;
        for (const azure::Subscription& s : client->subscriptions()) {
            const auto count = std::count_if(client->vms().begin(), client->vms().end(), [&](const Vm& vm) {
                return QString::fromStdString(vm.subscription).compare(QString::fromStdString(s.id), Qt::CaseInsensitive) == 0;
            });
            const bool enabled = s.state == "Enabled";
            auto* subscription = new QTreeWidgetItem(account, {QString::fromStdString(s.name)});
            subscription->setData(0, KeyRole, QStringLiteral("subscription:%1/%2").arg(id, QString::fromStdString(s.id)));
            subscription->setData(0, RowRole, QVariant::fromValue(GuestRow{"key-round", enabled, nullptr, {}, QString::fromStdString(s.name),
                                                                           enabled ? tr("%n VM(s)", nullptr, int(count)) : QString::fromStdString(s.state), {}, false}));
            subscription->setToolTip(0, tr("Subscription %1 · %2").arg(QString::fromStdString(s.id), QString::fromStdString(s.state)));
            for (const Vm& vm : client->vms()) {
                if (QString::fromStdString(vm.subscription).compare(QString::fromStdString(s.id), Qt::CaseInsensitive) != 0) continue;
                const bool running = vm.power == "running", busy = vm.power == "starting" || vm.power == "stopping" || vm.power == "deallocating";
                auto* row = new QTreeWidgetItem(subscription, {QString::fromStdString(vm.name)});
                row->setData(0, KeyRole, id + QLatin1Char('/') + QString::fromStdString(vm.key()));
                row->setData(0, RowRole, QVariant::fromValue(GuestRow{"monitor", running || busy, running ? "play" : busy ? "power" : nullptr,
                                                                      running ? green : orange, QString::fromStdString(vm.name),
                                                                      QStringLiteral("%1 · %2").arg(powerText(vm), QString::fromStdString(vm.size)), {}, false}));
                QString vmTip = tr("%1 · resource group %2 · %3").arg(QString::fromStdString(vm.os), QString::fromStdString(vm.resourceGroup),
                                                                       QString::fromStdString(vm.location));
                if (!vm.admin.empty()) vmTip += tr("\nAdministrator: %1").arg(QString::fromStdString(vm.admin));
                if (!vm.publicIp.empty()) vmTip += tr("\nPublic IP: %1").arg(QString::fromStdString(vm.publicIp));
                if (!vm.privateIp.empty()) vmTip += tr("\nPrivate IP: %1").arg(QString::fromStdString(vm.privateIp));
                row->setToolTip(0, vmTip);
            }
        }
    }
    for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
        folding_->apply(*it);
        if ((*it)->data(0, KeyRole).toString() == keep) tree_->setCurrentItem(*it);
    }
    note_->setText(clients_.empty() ? tr("Add an Azure account with + to see its VMs here. You sign in in your web browser.") : QString());
    note_->setVisible(clients_.empty());
    updateActions();
}

AzureClient* AzureView::selectedClient() const
{
    const QTreeWidgetItem* item = tree_->currentItem();
    while (item && item->parent()) item = item->parent();
    if (!item) return nullptr;
    const QString id = item->data(0, KeyRole).toString().mid(int(qstrlen("account:")));
    for (AzureClient* c : clients_)
        if (QString::fromStdString(c->account().id) == id) return c;
    return nullptr;
}

const Vm* AzureView::selectedVm() const
{
    const QTreeWidgetItem* item = tree_->currentItem();
    if (!item || !item->parent() || !item->parent()->parent()) return nullptr;
    return find(item->data(0, KeyRole).toString()).second;
}

void AzureView::updateActions()
{
    updateActions(actions_, selectedVm());
    AzureClient* c = selectedClient();
    for (QAction* a : {edit_, remove_, signIn_}) a->setEnabled(c);
    refresh_->setEnabled(!clients_.empty());
}

void AzureView::updateActions(const Actions& a, const Vm* vm) const
{
    const bool running = vm && vm->power == "running", off = vm && (vm->power == "deallocated" || vm->power == "stopped");
    a.open->setEnabled(vm);
    const bool windows = !vm || vm->os == "Windows";
    a.remote->setText(windows ? tr("Remote &Desktop") : tr("&SSH"));
    a.remote->setIcon(icons::lucide(windows ? "grid-2x2" : "square-terminal"));
    a.remote->setEnabled(running && !address(*vm).empty());
    a.start->setEnabled(off);
    a.restart->setEnabled(running);
    a.deallocate->setEnabled(vm && vm->power != "deallocated" && vm->power != "deallocating");
    a.powerOff->setEnabled(running || (vm && vm->power == "starting"));
    a.portal->setEnabled(vm);
}

void AzureView::serialConsole(const QString& key, Serial ready)
{
    const auto [client, vm] = find(key);
    if (!client || !vm) return;
    const QString name = QString::fromStdString(vm->name);
    auto failed = [this, name](const QString& error) { QMessageBox::warning(this, tr("Serial Console"), tr("No serial console for %1: %2").arg(name, error)); };
    client->serialConsole(*vm, [ready, name, account = accountName(client->account())](const QUrl& url, const QString& token) {
        ready(name, tr("%1 · %2 · serial console").arg(account, name), url, token);
    }, failed);
}

void AzureView::act(AzureClient* client, const Vm& vm, const QString& action)
{
    client->control(vm, action, [this, name = vm.name](const QString& error) { emit message(tr("%1: %2").arg(QString::fromStdString(name), error)); });
}
