#include "app/ProxmoxView.h"

#include "app/ConnectionStore.h"
#include "app/PanelTree.h"
#include "app/Icons.h"
#include "app/Prompts.h"
#include "app/ProxmoxClient.h"
#include "app/ProxmoxTerminal.h"
#include "app/WebSocketTunnel.h"

#include <QAction>
#include <QDialog>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFormLayout>
#include <QInputDialog>
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
#include <functional>

using proxmox::Guest;
using proxmox::Server;

using Problem = ProxmoxClient::Problem;

static constexpr int KeyRole = Qt::UserRole;
static constexpr int RowRole = Qt::UserRole + 1; // a GuestRow: nodes and guests

static std::string keychainKey(const std::string& id) { return "proxmox/" + id; }

static const QColor green(0x27, 0xae, 0x60), orange(0xf6, 0x74, 0x00), red(0xda, 0x44, 0x53);

static QString guestState(const Guest& g)
{
    if (g.isTemplate) return ProxmoxView::tr("Template");
    if (g.status == "running") return ProxmoxView::tr("Running");
    if (g.status == "stopped") return ProxmoxView::tr("Stopped");
    if (g.status == "paused") return ProxmoxView::tr("Paused");
    if (g.status == "suspended") return ProxmoxView::tr("Suspended");
    if (g.status == "prelaunch") return ProxmoxView::tr("Starting");
    return QString::fromStdString(g.status);
}

static void openWebUi(const ProxmoxClient* client)
{
    if (!client) return;
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setHost(QString::fromStdString(client->server().host));
    url.setPort(client->server().port);
    url.setPath(QStringLiteral("/"));
    QDesktopServices::openUrl(url);
}

static QString gib(long long bytes) { return QString::number(double(bytes) / (1 << 30), 'f', 1); }

static QString uptimeText(long long s)
{
    return s >= 86400 ? ProxmoxView::tr("%1 d %2 h").arg(s / 86400).arg(s % 86400 / 3600)
                      : ProxmoxView::tr("%1 h %2 min").arg(s / 3600).arg(s % 3600 / 60);
}

namespace {

struct ServerState {
    QString word, hint; // the hint row while nothing is listed
    QColor color;
};

ServerState serverState(const ProxmoxClient* c, const QPalette& palette)
{
    const QColor grey = palette.color(QPalette::Disabled, QPalette::Text);
    switch (c->problem()) {
    case Problem::NoSecret: return {ProxmoxView::tr("Log in"), ProxmoxView::tr("Double-click to log in"), grey};
    case Problem::Refused: return {ProxmoxView::tr("Login failed"), ProxmoxView::tr("Double-click to log in again"), red};
    case Problem::Certificate: return {ProxmoxView::tr("Certificate"), ProxmoxView::tr("Double-click to check its certificate"), orange};
    case Problem::Unreachable: return {ProxmoxView::tr("Unreachable"), ProxmoxView::tr("Double-click to retry"), red};
    case Problem::Other: return {ProxmoxView::tr("Error"), ProxmoxView::tr("Double-click to retry"), red};
    case Problem::None: break;
    }
    return {c->loaded() ? QString() : ProxmoxView::tr("Connecting…"), {}, grey};
}

class ProxmoxDelegate : public PanelDelegate {
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

ProxmoxView::ProxmoxView(QSettings* settings, QString configDir, QWidget* parent)
    : QWidget(parent), settings_(settings), configDir_(std::move(configDir))
{
    tree_ = new QTreeWidget;
    tree_->setHeaderHidden(true);
    tree_->setFrameShape(QFrame::NoFrame);
    setUpPanels(tree_, new ProxmoxDelegate(tree_));
    folding_ = new Folding(tree_, settings_, QStringLiteral("folded/proxmox"), KeyRole);
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
    add_ = make("plus", tr("&Add Server…"), [this] { editServer(nullptr); });
    refresh_ = make("refresh-cw", tr("Re&fresh"), [this] { refreshAll(); });
    actions_ = makeActions(this, [this] { return std::pair(selectedClient(), selectedGuest()); });
    const Actions& a = actions_;
    edit_ = make("pencil", tr("&Edit Server…"), [this] {
        if (ProxmoxClient* c = selectedClient()) editServer(c);
    });
    web_ = make("globe", tr("Open &Web UI"), [this] { openWebUi(selectedClient() ? selectedClient() : clients_.size() == 1 ? clients_[0] : nullptr); });
    web_->setToolTip(tr("The selected server's web interface, in your web browser"));
    remove_ = make("trash", tr("Re&move Server"), [this] {
        if (ProxmoxClient* c = selectedClient()) removeServer(c);
    });
    auto separator = [this] {
        auto* s = new QAction(this);
        s->setSeparator(true);
        return s;
    };
    rowMenus(tree_, [this] { return selectedGuest().has_value(); },
             {a.console, separator(), a.start, a.suspend, a.resume, separator(), a.shutdown, a.reboot, separator(), a.stop, a.reset},
             {web_, refresh_, edit_, remove_, separator(), add_}, {refresh_, add_});
    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        if (selectedGuest()) return actions_.console->trigger();
        if (ProxmoxClient* c = selectedClient(); c && c->problem() != Problem::None) return fix(c);
        foldLater(tree_, tree_->indexFromItem(item));
    });
    connect(tree_, &QTreeWidget::currentItemChanged, this, qOverload<>(&ProxmoxView::updateActions));

    poll_.setInterval(5000);
    connect(&poll_, &QTimer::timeout, this, [this] {
        if (!isVisible() && !following_) return;
        for (ProxmoxClient* c : clients_)
            if (c->hasSecret() && !c->rejected()) c->refresh();
    });
    poll_.start();
    load();
    rebuild();
}

QList<QAction*> ProxmoxView::titleActions() const { return {add_, refresh_, web_}; }

ProxmoxView::Actions ProxmoxView::makeActions(QObject* owner, const Target& target)
{
    auto make = [this, owner, target](const char* icon, const QString& text, std::function<void(ProxmoxClient*, const Guest&)> run) {
        auto* a = new QAction(icons::lucide(icon), text, owner);
        connect(a, &QAction::triggered, this, [target, run] {
            const auto [client, g] = target();
            if (client && g) run(client, *g);
        });
        return a;
    };
    auto control = [this](const char* action) {
        return [this, action](ProxmoxClient* client, const Guest& g) { act(client, g, QLatin1String(action)); };
    };
    Actions a;
    a.console = make("monitor", tr("&Console"), [this](ProxmoxClient* client, const Guest& g) {
        emit openRequested(QString::fromStdString(client->server().id + "/" + g.key()));
    });
    a.console->setToolTip(tr("Its tab: a VM's screen over SPICE when its display is SPICE, otherwise over VNC as the web console; "
                             "a container's terminal; a power button while it is stopped"));
    a.start = make("play", tr("&Start"), control("start"));
    a.shutdown = make("power", tr("Shut &Down"), control("shutdown"));
    a.shutdown->setToolTip(tr("Asks the guest system to shut down"));
    a.stop = make("power-off", tr("S&top"), control("stop"));
    a.reboot = make("rotate-cw", tr("&Reboot"), control("reboot"));
    a.reset = make("rotate-ccw", tr("R&eset"), control("reset"));
    a.suspend = make("pause", tr("&Pause"), control("suspend"));
    a.resume = make("play", tr("Res&ume"), control("resume"));
    return a;
}

std::pair<ProxmoxClient*, std::optional<Guest>> ProxmoxView::find(const QString& key) const
{
    const std::string serverId = key.section(QLatin1Char('/'), 0, 0).toStdString();
    const QString guestKey = key.section(QLatin1Char('/'), 1);
    const auto client = std::find_if(clients_.begin(), clients_.end(), [&](ProxmoxClient* c) { return c->server().id == serverId; });
    if (client == clients_.end()) return {nullptr, std::nullopt};
    for (const Guest& g : (*client)->resources().guests)
        if (QString::fromStdString(g.key()) == guestKey) return {*client, g};
    return {*client, std::nullopt};
}

VmConsole* ProxmoxView::console(const QString& key)
{
    ProxmoxClient* client = find(key).first;
    if (!client) return nullptr;
    auto status = [this, key]() -> std::optional<VmConsole::Status> {
        const auto [client, g] = find(key);
        if (!client) return std::nullopt; // its server was removed
        if (!g) // deleted, or not listed yet
            return client->problem() == Problem::None && !client->resources().guests.empty() ? std::nullopt
                                                                                           : std::optional(VmConsole::Status{.known = false});
        VmConsole::Status s;
        s.name = QString::fromStdString(g->name);
        s.state = QString::fromStdString(g->status);
        if (!s.state.isEmpty()) s.state[0] = s.state[0].toUpper();
        const bool stopped = g->status == "stopped";
        s.startable = stopped && !g->isTemplate;
        s.text = g->isTemplate ? tr("A template: it does not run.") : stopped ? tr("Stopped. The button starts it.") : s.state;
        if (g->status == "running") s.display = QStringLiteral("running");
        return s;
    };
    auto self = std::make_shared<QPointer<VmConsole>>();
    auto start = [this, key, self] {
        const auto [client, g] = find(key);
        if (!client || !g) return;
        client->control(*g, QStringLiteral("start"), [this, self, name = g->name](const QString& error) {
            emit message(tr("%1: %2").arg(QString::fromStdString(name), error));
            if (*self) (*self)->startFailed();
        });
    };
    auto* console = new VmConsole(status, start, [this, key](VmConsole* c) { requestConsole(key, c); });
    *self = console;
    if (const auto g = find(key).second) console->setWindowTitle(QString::fromStdString(g->name));
    connect(client, &ProxmoxClient::resourcesChanged, console, &VmConsole::follow);
    connect(client, &QObject::destroyed, console, &VmConsole::closeRequested);
    const Actions a = makeActions(console, [this, key] { return find(key); });
    console->addActions({a.start, a.suspend, a.resume, a.shutdown, a.reboot, a.stop});
    auto update = [this, a, key] { updateActions(a, find(key).second); };
    connect(client, &ProxmoxClient::resourcesChanged, console, update);
    update();
    ++following_;
    connect(console, &QObject::destroyed, this, [this] { --following_; });
    return console;
}

bool ProxmoxView::event(QEvent* event)
{
    if (event->type() == QEvent::Show) refreshAll();
    return QWidget::event(event);
}

void ProxmoxView::load()
{
    const int n = settings_->beginReadArray(QStringLiteral("proxmox"));
    for (int i = 0; i < n; ++i) {
        settings_->setArrayIndex(i);
        Server s;
        s.id = settings_->value(QStringLiteral("id")).toString().toStdString();
        s.name = settings_->value(QStringLiteral("name")).toString().toStdString();
        s.host = settings_->value(QStringLiteral("host")).toString().toStdString();
        s.port = settings_->value(QStringLiteral("port"), 8006).toInt();
        s.user = settings_->value(QStringLiteral("user")).toString().toStdString();
        s.fingerprint = settings_->value(QStringLiteral("fingerprint")).toString().toStdString();
        if (!s.id.empty() && !s.host.empty()) addClient(s);
    }
    settings_->endArray();
}

void ProxmoxView::save() const
{
    settings_->remove(QStringLiteral("proxmox"));
    settings_->beginWriteArray(QStringLiteral("proxmox"), int(clients_.size()));
    for (size_t i = 0; i < clients_.size(); ++i) {
        settings_->setArrayIndex(int(i));
        const Server& s = clients_[i]->server();
        settings_->setValue(QStringLiteral("id"), QString::fromStdString(s.id));
        settings_->setValue(QStringLiteral("name"), QString::fromStdString(s.name));
        settings_->setValue(QStringLiteral("host"), QString::fromStdString(s.host));
        settings_->setValue(QStringLiteral("port"), s.port);
        settings_->setValue(QStringLiteral("user"), QString::fromStdString(s.user));
        settings_->setValue(QStringLiteral("fingerprint"), QString::fromStdString(s.fingerprint));
    }
    settings_->endArray();
}

void ProxmoxView::addClient(const Server& server)
{
    auto* client = new ProxmoxClient(server, this);
    clients_.push_back(client);
    connect(client, &ProxmoxClient::resourcesChanged, this, &ProxmoxView::rebuild);
    connect(client, &ProxmoxClient::failed, this, &ProxmoxView::rebuild);
    connect(client, &ProxmoxClient::certificateUnknown, this, [this, client](const QString& fingerprint, const QString& subject) {
        QString pairs;
        for (int i = 0; i < fingerprint.size(); i += 2) pairs += (i ? QStringLiteral(":") : QString()) + fingerprint.mid(i, 2).toUpper();
        const Server& s = client->server();
        const bool known = !s.fingerprint.empty();
        if (!prompts::trust(this,
                            known ? tr("WARNING: the certificate of %1 has CHANGED. Trust the new one?").arg(QString::fromStdString(s.host))
                                  : tr("The certificate of %1 is not known. Trust it?").arg(QString::fromStdString(s.host)),
                            tr("Server: %1:%2\nSubject: %3\nFingerprint (SHA-256):\n%4\n\nCompare it with the one Proxmox shows "
                               "under the node → System → Certificates.")
                                .arg(QString::fromStdString(s.host)).arg(s.port).arg(subject, pairs))) {
            rebuild();
            return;
        }
        Server trusted = s;
        trusted.fingerprint = fingerprint.toStdString();
        client->setServer(trusted);
        save();
        client->refresh();
    });
    secrets::read(keychainKey(server.id), client, [this, client](QString secret) {
        if (client->hasSecret()) return;
        client->setSecret(secret);
        client->refresh();
        rebuild();
    });
}

void ProxmoxView::editServer(ProxmoxClient* client)
{
    const Server old = client ? client->server() : Server{};
    QDialog dialog(this);
    dialog.setWindowTitle(client ? tr("Edit Proxmox Server") : tr("Add Proxmox Server"));
    auto* name = new QLineEdit(QString::fromStdString(old.name));
    auto* address = new QLineEdit(client ? QStringLiteral("%1:%2").arg(QString::fromStdString(old.host)).arg(old.port) : QString());
    address->setPlaceholderText(tr("pve.example.com or pve.example.com:8006"));
    auto* user = new QLineEdit(QString::fromStdString(client ? old.user : "root@pam"));
    auto* secret = new QLineEdit;
    secret->setEchoMode(QLineEdit::Password);
    secret->setPlaceholderText(client ? tr("Unchanged") : QString());
    auto* hint = new QLabel(tr("An API token instead of a password: user@realm!tokenid as the user, its secret as the password. "
                               "Accounts with a second factor need a token."));
    hint->setWordWrap(true);
    auto* form = new QFormLayout;
    form->addRow(tr("&Name:"), name);
    form->addRow(tr("&Address:"), address);
    form->addRow(tr("&User:"), user);
    form->addRow(tr("&Password:"), secret);
    form->addRow(hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto validate = [&] {
        std::string h;
        int p = 0;
        buttons->button(QDialogButtonBox::Ok)->setEnabled(proxmox::parseAddress(address->text(), h, p) && !user->text().trimmed().isEmpty() &&
                                                          (client || !secret->text().isEmpty()));
    };
    for (QLineEdit* e : {address, user, secret}) connect(e, &QLineEdit::textChanged, &dialog, validate);
    validate();
    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(buttons);
    dialog.resize(520, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted) return;

    Server s = old;
    if (s.id.empty()) s.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    proxmox::parseAddress(address->text(), s.host, s.port);
    s.user = user->text().trimmed().toStdString();
    s.name = name->text().trimmed().isEmpty() ? s.host : name->text().trimmed().toStdString();
    if (s.host != old.host || s.port != old.port) s.fingerprint.clear();
    if (!client) {
        addClient(s);
        client = clients_.back();
    } else {
        client->setServer(s);
    }
    if (!secret->text().isEmpty()) storeSecret(client, secret->text());
    save();
    client->refresh();
    rebuild();
}

void ProxmoxView::storeSecret(ProxmoxClient* client, const QString& secret)
{
    client->setSecret(secret);
    secrets::write(keychainKey(client->server().id), secret, this, [this](QString error) {
        if (!error.isEmpty())
            QMessageBox::warning(this, tr("Passwords"), tr("The password was not saved: %1").arg(error));
    });
}

void ProxmoxView::fix(ProxmoxClient* client)
{
    const Server& s = client->server();
    if (client->problem() != Problem::NoSecret && client->problem() != Problem::Refused) return client->retry();
    bool ok = false;
    const QString user = QString::fromStdString(s.user);
    const QString secret = QInputDialog::getText(this, tr("Log In to %1").arg(QString::fromStdString(s.name)),
                                                 proxmox::isApiToken(s.user) ? tr("Secret of the API token %1:").arg(user)
                                                                             : tr("Password of %1:").arg(user),
                                                 QLineEdit::Password, {}, &ok);
    if (!ok || secret.isEmpty()) return;
    storeSecret(client, secret);
    client->refresh();
    rebuild();
}

void ProxmoxView::removeServer(ProxmoxClient* client)
{
    if (!prompts::confirm(this, tr("Remove Server"),
                          tr("Remove \"%1\" from the list? Nothing changes on the server.").arg(QString::fromStdString(client->server().name)),
                          tr("&Remove")))
        return;
    secrets::remove(keychainKey(client->server().id));
    QFile::remove(configDir_ + QStringLiteral("/proxmox/%1-ca.pem").arg(QString::fromStdString(client->server().id)));
    std::erase(clients_, client);
    client->deleteLater();
    save();
    rebuild();
}

void ProxmoxView::refreshAll()
{
    for (ProxmoxClient* c : clients_)
        if (c->hasSecret()) c->refresh();
}

void ProxmoxView::rebuild()
{
    const Rebuild quiet(tree_);
    const QTreeWidgetItem* current = tree_->currentItem();
    const QString keep = current ? current->data(0, KeyRole).toString() : QString();
    tree_->clear();
    const QString graphTip = tr("The graph: its CPU use over the last 200 seconds");
    for (ProxmoxClient* client : clients_) {
        const Server& s = client->server();
        const QString id = QString::fromStdString(s.id);
        const ServerState state = serverState(client, palette());
        auto* server = new QTreeWidgetItem(tree_, {QString::fromStdString(s.name)});
        server->setData(0, KeyRole, QStringLiteral("server:") + id);
        server->setIcon(0, icons::brand("proxmox-line"));
        server->setData(0, PanelDelegate::HeaderRole, state.word);
        server->setData(0, PanelDelegate::HeaderColorRole, state.color);
        QString tip = QStringLiteral("%1:%2 · %3").arg(QString::fromStdString(s.host)).arg(s.port).arg(QString::fromStdString(s.user));
        if (!client->lastError().isEmpty()) tip += QLatin1Char('\n') + client->lastError();
        server->setToolTip(0, tip);
        if (!state.hint.isEmpty()) {
            auto* hint = new QTreeWidgetItem(server, {state.hint});
            hint->setData(0, KeyRole, QStringLiteral("hint:") + id);
            hint->setToolTip(0, tip);
        }
        auto history = [client](const std::string& key) {
            const std::deque<float>* cpu = client->cpu(key);
            return cpu ? std::vector<float>(cpu->begin(), cpu->end()) : std::vector<float>();
        };
        for (const proxmox::Node& n : client->problem() == Problem::None ? client->resources().nodes : std::vector<proxmox::Node>()) {
            auto* node = new QTreeWidgetItem(server, {QString::fromStdString(n.name)}); // drawn by ProxmoxDelegate; the text for type-ahead
            node->setData(0, KeyRole, QStringLiteral("node:%1/%2").arg(id, QString::fromStdString(n.name)));
            GuestRow row{"server", n.online, nullptr, {}, QString::fromStdString(n.name),
                         !n.online ? tr("Offline") : n.maxmem ? tr("RAM %1 / %2 GiB").arg(gib(n.mem), gib(n.maxmem)) : tr("Online"),
                         history("node/" + n.name)};
            node->setData(0, RowRole, QVariant::fromValue(row));
            QString nodeTip = tr("Node %1 · %2").arg(QString::fromStdString(n.name), n.online ? tr("online") : tr("offline"));
            if (n.online)
                nodeTip += tr("\nCPU %1% of %2 · RAM %3 of %4 GiB · up %5").arg(qRound(n.cpu * 100)).arg(n.maxcpu).arg(gib(n.mem), gib(n.maxmem),
                                                                                                          uptimeText(n.uptime)) +
                           QLatin1Char('\n') + graphTip;
            node->setToolTip(0, nodeTip);
            for (const Guest& g : client->resources().guests) {
                if (g.node != n.name) continue;
                auto* guest = new QTreeWidgetItem(node, {QStringLiteral("%1 %2").arg(g.vmid).arg(QString::fromStdString(g.name))});
                guest->setData(0, KeyRole, id + QLatin1Char('/') + QString::fromStdString(g.key()));
                const bool running = g.status == "running", paused = g.status == "paused" || g.status == "suspended";
                GuestRow row{g.type == "lxc" ? "box" : "monitor", running || paused,
                             running ? "play" : paused ? "pause" : g.status == "prelaunch" ? "power" : nullptr,
                             running ? green : orange, QString::fromStdString(g.name), QStringLiteral("%1 · %2").arg(g.vmid).arg(guestState(g)),
                             history(g.key())};
                guest->setData(0, RowRole, QVariant::fromValue(row));
                QString guestTip = QStringLiteral("%1 %2 · %3").arg(g.type == "lxc" ? tr("Container") : tr("VM")).arg(g.vmid).arg(guestState(g));
                if (running) guestTip += tr("\nCPU %1% · RAM %2 of %3 GiB").arg(qRound(g.cpu * 100)).arg(gib(g.mem), gib(g.maxmem)) +
                                         QLatin1Char('\n') + graphTip;
                guest->setToolTip(0, guestTip);
            }
        }
    }
    for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
        folding_->apply(*it);
        if ((*it)->data(0, KeyRole).toString() == keep) tree_->setCurrentItem(*it);
    }
    note_->setText(clients_.empty() ? tr("Add a Proxmox VE server with + to see its VMs and containers here.") : QString());
    note_->setVisible(clients_.empty());
    updateActions();
}

ProxmoxClient* ProxmoxView::selectedClient() const
{
    const QTreeWidgetItem* item = tree_->currentItem();
    while (item && item->parent()) item = item->parent();
    if (!item) return nullptr;
    const QString id = item->data(0, KeyRole).toString().mid(int(qstrlen("server:")));
    for (ProxmoxClient* c : clients_)
        if (QString::fromStdString(c->server().id) == id) return c;
    return nullptr;
}

std::optional<Guest> ProxmoxView::selectedGuest() const
{
    const QTreeWidgetItem* item = tree_->currentItem();
    ProxmoxClient* client = selectedClient();
    if (!item || !client || !item->parent() || !item->parent()->parent()) return std::nullopt;
    const QString key = item->data(0, KeyRole).toString().section(QLatin1Char('/'), 1);
    for (const Guest& g : client->resources().guests)
        if (QString::fromStdString(g.key()) == key) return g;
    return std::nullopt;
}

void ProxmoxView::updateActions()
{
    updateActions(actions_, selectedGuest());
    for (QAction* a : {edit_, remove_}) a->setEnabled(selectedClient());
    web_->setEnabled(selectedClient() || clients_.size() == 1);
    refresh_->setEnabled(!clients_.empty());
}

void ProxmoxView::updateActions(const Actions& a, const std::optional<Guest>& g) const
{
    const bool running = g && g->status == "running", stopped = g && g->status == "stopped" && !g->isTemplate;
    const bool vm = g && g->type == "qemu";
    a.console->setEnabled(g && !g->isTemplate);
    a.start->setEnabled(stopped);
    a.shutdown->setEnabled(running);
    a.reboot->setEnabled(running);
    a.stop->setEnabled(g && !stopped && !g->isTemplate);
    a.reset->setEnabled(vm && running);
    a.suspend->setEnabled(running);
    a.resume->setEnabled(g && !stopped && !g->isTemplate); // paused shows as running
}

void ProxmoxView::act(ProxmoxClient* client, const Guest& guest, const QString& action)
{
    if (action == QLatin1String("stop") || action == QLatin1String("reset")) {
        const bool stop = action == QLatin1String("stop");
        if (!prompts::confirm(this, stop ? tr("Stop") : tr("Reset"),
                              stop ? tr("Stop \"%1\"? It is like pulling the power plug: unsaved work in it is lost.").arg(QString::fromStdString(guest.name))
                                   : tr("Reset \"%1\"? It is like pressing its reset button: unsaved work in it is lost.").arg(QString::fromStdString(guest.name)),
                              stop ? tr("&Stop") : tr("R&eset"), QStyle::SP_MessageBoxWarning))
            return;
    }
    client->control(guest, action, [this, name = guest.name](const QString& error) {
        emit message(tr("%1: %2").arg(QString::fromStdString(name), error));
    });
}

void ProxmoxView::requestConsole(const QString& key, QWidget* replace)
{
    const auto [client, g] = find(key);
    if (!client || !g || g->isTemplate) return;
    Console console{client, *g, key, replace, replace != nullptr};
    if (g->type == "lxc") return openTerminal(console);
    client->checkSpice(*g, [this, console](bool spice) { spice ? openSpice(console) : openVnc(console); },
                          [this, console](const QString& message) { consoleFailed(console, message); });
}

void ProxmoxView::consoleFailed(const Console& console, const QString& message)
{
    QMessageBox::warning(this, tr("Console"), tr("No console for %1: %2").arg(QString::fromStdString(console.guest.name), message));
}

QString ProxmoxView::consoleAddress(const Console& console) const
{
    return QStringLiteral("%1 · %2/%3").arg(QString::fromStdString(console.client->server().name), QString::fromStdString(console.guest.node))
        .arg(console.guest.vmid);
}

void ProxmoxView::openSpice(const Console& console)
{
    if (!console.client) return;
    console.client->requestSpice(console.guest, [this, console](const QByteArray& json) {
        if (!console.client || (console.reconnect && !console.replace)) return; // tab closed meanwhile
        const std::string id = console.client->server().id;
        QDir().mkpath(configDir_ + QStringLiteral("/proxmox"));
        const QString caPath = configDir_ + QStringLiteral("/proxmox/%1-ca.pem").arg(QString::fromStdString(id));
        const QByteArray ca = proxmox::spiceCa(json);
        if (!ca.isEmpty()) {
            QFile file(caPath);
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(ca);
        }
        QString error;
        const auto c = proxmox::spiceConnection(json, console.guest.name, ca.isEmpty() ? std::string() : caPath.toStdString(), &error);
        if (!c) return consoleFailed(console, error);
        emit consoleReady(*c, console.key, console.replace, consoleAddress(console), nullptr);
    }, [this, console](const QString& message) { consoleFailed(console, message); });
}

void ProxmoxView::openVnc(const Console& console)
{
    if (!console.client) return;
    console.client->requestVnc(console.guest, [this, console](const QByteArray& json) {
        if (!console.client || (console.reconnect && !console.replace)) return;
        QString error;
        const auto ticket = proxmox::parseVncProxy(json, &error);
        if (!ticket) return consoleFailed(console, error);
        QPointer<ProxmoxClient> client = console.client;
        auto* tunnel = new WebSocketTunnel(client->vncWebSocket(console.guest, ticket->port, QString::fromStdString(ticket->ticket)),
                                           {QStringLiteral("binary")}, // required by pveproxy
                                           [client](const QSslCertificate& cert) { return client && client->trusts(cert); }, this);
        if (!tunnel->isListening()) {
            delete tunnel;
            return consoleFailed(console, tr("No local port for the console."));
        }
        connect(tunnel, &WebSocketTunnel::failed, this, [this, name = console.guest.name](const QString& message) {
            emit this->message(tr("%1: %2").arg(QString::fromStdString(name), message));
        });
        rdc::Connection c;
        c.protocol = rdc::Protocol::VNC;
        c.name = console.guest.name;
        c.host = "127.0.0.1";
        c.port = tunnel->serverPort();
        c.password = ticket->password; // as noVNC
        emit consoleReady(c, console.key, console.replace, consoleAddress(console), tunnel);
    }, [this, console](const QString& message) { consoleFailed(console, message); });
}

void ProxmoxView::openTerminal(const Console& console)
{
    if (!console.client) return;
    console.client->requestTerminal(console.guest, [this, console](const QByteArray& json) {
        if (!console.client || (console.reconnect && !console.replace)) return;
        QString error;
        const auto ticket = proxmox::parseVncProxy(json, &error);
        if (!ticket) return consoleFailed(console, error);
        QPointer<ProxmoxClient> client = console.client;
        const QNetworkRequest request = client->vncWebSocket(console.guest, ticket->port, QString::fromStdString(ticket->ticket));
        const QByteArray login = QByteArray::fromStdString((ticket->user.empty() ? client->server().user : ticket->user) + ':' + ticket->ticket + '\n');
        emit terminalReady(console.key, console.replace, QString::fromStdString(console.guest.name), consoleAddress(console),
                           [request, login, client](rdc::TerminalListener& listener) -> std::unique_ptr<rdc::TerminalSession> {
                               return std::make_unique<ProxmoxTerminal>(listener, request, login, [client](const QSslCertificate& cert) {
                                   return client && client->trusts(cert);
                               });
                           });
    }, [this, console](const QString& message) { consoleFailed(console, message); });
}
