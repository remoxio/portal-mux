#include "app/VmView.h"
#include "app/Prompts.h"

#include "app/ConnectionStore.h"
#include "app/PanelTree.h"
#include "app/Icons.h"
#include "app/Look.h"
#include "app/SshServers.h"
#include "app/VmSettingsDialog.h"
#include "ssh/SshTunnel.h"

#include <QAction>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QUrl>
#include <QUrlQuery>
#include <QSettings>
#include <QLineEdit>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QDialog>
#include <QApplication>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <tuple>

using Action = LibvirtVms::Action;
using State = LibvirtVms::State;
using Vm = LibvirtVms::Vm;

QString VmView::stateText(State s)
{
    switch (s) {
    case State::Running: return tr("Running");
    case State::Paused: return tr("Paused");
    case State::ShuttingDown: return tr("Shutting down");
    case State::ShutOff: return tr("Shut off");
    case State::Saved: return tr("Saved");
    case State::Crashed: return tr("Crashed");
    case State::Suspended: return tr("Suspended");
    case State::Other: break;
    }
    return tr("Unknown");
}

namespace {

// Breeze: positive, neutral, focus, negative; dimmed text
QColor stateColor(State s, const QPalette& palette)
{
    switch (s) {
    case State::Running: return QColor(0x27, 0xae, 0x60);
    case State::Paused:
    case State::Suspended:
    case State::ShuttingDown: return QColor(0xf6, 0x74, 0x00);
    case State::Saved: return QColor(0x3d, 0xae, 0xe9);
    case State::Crashed: return QColor(0xda, 0x44, 0x53);
    default: return palette.color(QPalette::Disabled, QPalette::Text);
    }
}

const char* stateGlyph(State s)
{
    switch (s) {
    case State::Running: return "play";
    case State::Paused:
    case State::Suspended: return "pause";
    case State::ShuttingDown: return "power";
    case State::Saved: return "save";
    case State::Crashed: return "triangle-alert";
    default: return nullptr;
    }
}

constexpr int HostRole = Qt::UserRole + 1; // its LibvirtVms; Qt::UserRole: the VM's uuid, empty on a host row
constexpr int FoldRole = Qt::UserRole + 2; // a host row's uri

LibvirtVms* hostOf(const QModelIndex& index) { return reinterpret_cast<LibvirtVms*>(index.data(HostRole).value<quintptr>()); }
LibvirtVms* hostOf(const QTreeWidgetItem* item) { return reinterpret_cast<LibvirtVms*>(item->data(0, HostRole).value<quintptr>()); }

// a VM row as a GuestRow; host rows are panel headers
class VmDelegate : public PanelDelegate {
public:
    using PanelDelegate::PanelDelegate;

protected:
    std::optional<GuestRow> row(const QModelIndex& index) const override
    {
        const std::string uuid = index.data(Qt::UserRole).toString().toStdString();
        const LibvirtVms* vms = hostOf(index);
        if (uuid.empty() || !vms) return std::nullopt;
        const auto vm = std::find_if(vms->vms().begin(), vms->vms().end(), [&](const Vm& v) { return v.uuid == uuid; });
        if (vm == vms->vms().end()) return std::nullopt;
        GuestRow row;
        row.active = vm->state == State::Running || vm->state == State::Paused || vm->state == State::ShuttingDown ||
                     vm->state == State::Suspended;
        row.glyph = stateGlyph(vm->state);
        row.glyphColor = stateColor(vm->state, QApplication::palette());
        row.name = QString::fromStdString(vm->name);
        row.detail = VmView::stateText(vm->state);
        if (const std::deque<float>* cpu = vms->cpu(vm->uuid)) row.cpu.assign(cpu->begin(), cpu->end());
        return row;
    }
};

struct SshLogin {
    QString user, host, keyFile;
    int port = 22;
};

SshLogin loginOf(const SshServer& s)
{
    const rdc::SshConnection::Params p = rdc::SshTunnel::parse(s.address().toStdString());
    return {QString::fromStdString(p.user), QString::fromStdString(p.host),
            s.auth == QLatin1String("publicKey") ? s.keyFile : QString(), p.port};
}

// qemu+ssh://[user@]host[:port]/system?keyfile=…; other query items as in `base`
QString libvirtUri(const SshLogin& login, const QString& machines, const QUrl& base)
{
    QUrlQuery query(base);
    query.removeAllQueryItems(QStringLiteral("keyfile"));
    QString key = login.keyFile.trimmed();
    if (key.startsWith(QLatin1String("~/"))) key = QDir::homePath() + key.mid(1); // libvirt doesn't expand it
    if (!key.isEmpty()) query.addQueryItem(QStringLiteral("keyfile"), key);
    QUrl url;
    url.setScheme(QStringLiteral("qemu+ssh"));
    if (!login.user.trimmed().isEmpty()) url.setUserName(login.user.trimmed()); // else qemu+ssh://@host
    url.setHost(login.host.trimmed());
    if (login.port != 22) url.setPort(login.port);
    url.setPath(QLatin1Char('/') + machines);
    url.setQuery(query);
    return url.toString(QUrl::FullyEncoded); // libvirt's parser takes no spaces
}

QString machinesOf(const QUrl& url) { return url.path() == QLatin1String("/session") ? QStringLiteral("session") : QStringLiteral("system"); }

} // namespace

VmView::VmView(QSettings* settings, QWidget* parent) : QWidget(parent), settings_(settings)
{
    tree_ = new QTreeWidget;
    tree_->setHeaderHidden(true);
    tree_->setFrameShape(QFrame::NoFrame);
    setUpPanels(tree_, new VmDelegate(tree_));
    folding_ = new Folding(tree_, settings_, QStringLiteral("folded/kvm"), FoldRole);
    note_ = new QLabel(tr("Add a KVM host with +: libvirt over SSH (qemu+ssh://)."));
    note_->setWordWrap(true);
    note_->setContentsMargins(10, 6, 10, 6);
    note_->hide();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(note_);
    layout->addWidget(tree_, 1);

    actions_ = makeActions(this, [this] { return std::pair<LibvirtVms*, const Vm*>(selectedHost(), selected()); });
    auto make = [this](const char* icon, const QString& text, std::function<void()> run) {
        auto* a = new QAction(icons::lucide(icon), text, this);
        connect(a, &QAction::triggered, this, run);
        return a;
    };
    addHost_ = make("plus", tr("&Add Host…"), [this] { editHost(nullptr); });
    addHost_->setToolTip(tr("A libvirt host over SSH"));
    editHost_ = make("pencil", tr("&Edit Host…"), [this] { editHost(selectedHost()); });
    removeHost_ = make("trash", tr("Re&move Host"), [this] { removeHost(selectedHost()); });
    auto separator = [this] {
        auto* s = new QAction(this);
        s->setSeparator(true);
        return s;
    };
    const Actions& a = actions_;
    rowMenus(tree_, [this] { return selected() != nullptr; },
             {a.open, a.serial, a.settings, a.snapshots, separator(), a.start, a.pause, separator(), a.shutdown, a.reboot, a.save, separator(),
              a.forceOff, a.reset},
             {editHost_, removeHost_, separator(), addHost_}, {addHost_});

    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        if (!item->data(0, Qt::UserRole).toString().isEmpty()) actions_.open->trigger();
        else foldLater(tree_, tree_->indexFromItem(item));
    });
    connect(tree_, &QTreeWidget::currentItemChanged, this, qOverload<>(&VmView::updateActions));

    const char* testUri = std::getenv("PORTAL_MUX_LIBVIRT_URI"); // test:///<file>: fake VMs for UI tests
#if defined(_WIN32) || defined(Q_OS_MACOS)
    local_ = testUri && *testUri;
#endif
    if (local_) addHost(tr("This computer"), testUri && *testUri ? testUri : "qemu:///system");
    const int n = settings_->beginReadArray(QStringLiteral("libvirtHosts"));
    std::vector<std::tuple<QString, QString, QString>> saved;
    for (int i = 0; i < n; ++i) {
        settings_->setArrayIndex(i);
        saved.emplace_back(settings_->value(QStringLiteral("name")).toString(), settings_->value(QStringLiteral("uri")).toString(),
                           settings_->value(QStringLiteral("server")).toString());
    }
    settings_->endArray();
    const std::vector<SshServer> servers = loadSshServers(settings_);
    for (const auto& [name, uri, server] : saved) {
        const auto s = std::find_if(servers.begin(), servers.end(), [&](const SshServer& s) { return s.id == server; });
        if (s == servers.end()) { // none, or removed in Settings: its last address
            if (!uri.isEmpty()) addHost(name, uri.toStdString());
            continue;
        }
        servers_[addHost(name, libvirtUri(loginOf(*s), machinesOf(QUrl(uri)), QUrl(uri)).toStdString())] = server;
    }
    rebuild();
}

QList<QAction*> VmView::titleActions() const { return {addHost_}; }

VmView::Actions VmView::makeActions(QObject* owner, const Target& target)
{
    auto make = [this, owner, target](const char* icon, const QString& text, std::function<void(LibvirtVms*, const Vm&)> run) {
        auto* a = new QAction(icons::lucide(icon), text, owner);
        connect(a, &QAction::triggered, this, [target, run] {
            const auto [host, vm] = target();
            if (host && vm) run(host, *vm);
        });
        return a;
    };
    Actions a;
    a.open = make("monitor", tr("&Open"), [this](LibvirtVms* host, const Vm& vm) { emit openRequested(host, vm.uuid); });
    a.open->setToolTip(tr("Its tab: the display, or a power button while it is off"));
    a.start = make("play", tr("&Start"), [this](LibvirtVms* host, const Vm& vm) { // Resume while paused, as virt-manager's Run
        act(host, vm, LibvirtVms::allowed(Action::Resume, vm.state) ? Action::Resume : Action::Start);
    });
    auto control = [this](Action action) { return [this, action](LibvirtVms* host, const Vm& vm) { act(host, vm, action); }; };
    a.pause = make("pause", tr("&Pause"), control(Action::Pause));
    a.shutdown = make("power", tr("Shut &Down"), control(Action::Shutdown));
    a.shutdown->setToolTip(tr("Asks the guest system to shut down"));
    a.reboot = make("rotate-cw", tr("&Reboot"), control(Action::Reboot));
    a.save = make("save", tr("Sa&ve"), control(Action::Save));
    a.save->setToolTip(tr("Saves its memory to disk and stops it; Start continues where it was"));
    a.forceOff = make("power-off", tr("Force &Off"), control(Action::ForceOff));
    a.reset = make("rotate-ccw", tr("R&eset"), control(Action::Reset));
    a.serial = make("terminal", tr("Serial &Console"), [this](LibvirtVms* host, const Vm& vm) { emit serialRequested(host, vm.uuid); });
    a.serial->setToolTip(tr("Its serial console in a terminal (a console device in the VM, a getty in the guest)"));
    a.settings = make("settings", tr("Se&ttings…"), [this](LibvirtVms* host, const Vm& vm) { editSettings(host, vm.uuid); });
    a.settings->setToolTip(tr("Processors, memory and its CD/DVD"));
    a.snapshots = make("rotate-ccw-clock", tr("S&napshots…"), [this](LibvirtVms* host, const Vm& vm) { editSnapshots(host, vm.uuid); });
    a.snapshots->setToolTip(tr("Take one, go back to one, delete them"));
    return a;
}

VmConsole* VmView::console(LibvirtVms* host, const std::string& uuid, VmConsole::Open openDisplay)
{
    auto find = [host, uuid]() -> const Vm* {
        const auto& vms = host->vms();
        const auto it = std::find_if(vms.begin(), vms.end(), [&](const Vm& v) { return v.uuid == uuid; });
        return it == vms.end() ? nullptr : &*it;
    };
    auto status = [host, find]() -> std::optional<VmConsole::Status> {
        const Vm* vm = find();
        if (!vm) return host->connected() ? std::nullopt : std::optional(VmConsole::Status{.known = false});
        VmConsole::Status s;
        s.name = QString::fromStdString(vm->name);
        s.state = stateText(vm->state);
        const bool control = host->connected() && !host->readOnly();
        switch (vm->state) {
        case State::ShutOff: s.text = tr("Shut off. The button starts it."); break;
        case State::Saved: s.text = tr("Saved. The button starts it where it was."); break;
        case State::Crashed: s.text = tr("Crashed. The button starts it again."); break;
        case State::ShuttingDown: s.text = tr("Shutting down…"); break;
        default: s.text = tr("%1, without a SPICE or VNC display on the network.").arg(stateText(vm->state)); break;
        }
        if (!control) s.text = tr("Read-only access to libvirt: it cannot be started from here.");
        s.startable = control && LibvirtVms::allowed(Action::Start, vm->state);
        if (const auto& d = vm->display)
            s.display = QStringLiteral("%1:%2:%3:%4").arg(QString::fromStdString(d->host)).arg(d->port)
                            .arg(QString::fromStdString(d->option("tlsPort")), QString::fromStdString(d->password));
        return s;
    };
    auto* console = new VmConsole(status, [host, uuid] { host->control(uuid, Action::Start); }, std::move(openDisplay));
    if (const Vm* vm = find()) console->setWindowTitle(QString::fromStdString(vm->name));
    connect(host, &LibvirtVms::changed, console, &VmConsole::follow);
    connect(host, &QObject::destroyed, console, &VmConsole::closeRequested); // its host was removed
    connect(host, &LibvirtVms::failed, console, [console, uuid](const QString& failed) { // the error itself: this view's dialog
        if (failed.toStdString() == uuid) console->startFailed();
    });
    const Actions a = makeActions(console, [host, find] { return std::pair<LibvirtVms*, const Vm*>(host, find()); });
    auto* separator = new QAction(console);
    separator->setSeparator(true);
    console->addActions({a.start, a.pause, a.shutdown, a.reboot, a.forceOff, separator, a.settings, a.snapshots});
    auto update = [this, a, host, find] { updateActions(a, host, find()); };
    connect(host, &LibvirtVms::changed, console, update);
    update();
    return console;
}

LibvirtVms* VmView::addHost(const QString& name, const std::string& uri)
{
    auto* host = new LibvirtVms(name, uri, this);
    hosts_.push_back(host);
    connect(host, &LibvirtVms::changed, this, &VmView::rebuild);
    connect(host, &LibvirtVms::changed, this, &VmView::vmsChanged);
    connect(host, &LibvirtVms::statsChanged, tree_->viewport(), qOverload<>(&QWidget::update));
    connect(host, &LibvirtVms::failed, this, [this](const QString&, const QString& name, const QString& text) {
        QMessageBox::warning(this, tr("Virtual machine"), name.isEmpty() ? text : tr("%1: %2").arg(name, text));
    });
    return host;
}

void VmView::putHost(LibvirtVms* old, const QString& name, const std::string& uri, const QString& server)
{
    const auto at = std::find(hosts_.begin(), hosts_.end(), old) - hosts_.begin();
    LibvirtVms* host = addHost(name, uri);
    if (!server.isEmpty()) servers_[host] = server;
    if (old) { // in its place; its consoles close with it
        std::rotate(hosts_.begin() + at, hosts_.end() - 1, hosts_.end());
        std::erase(hosts_, old);
        servers_.erase(old);
        delete old;
    }
    save();
    rebuild();
}

void VmView::reloadServers()
{
    const std::vector<SshServer> servers = loadSshServers(settings_);
    for (LibvirtVms* host : std::vector(hosts_)) {
        const auto linked = servers_.find(host);
        if (linked == servers_.end()) continue;
        const auto s = std::find_if(servers.begin(), servers.end(), [&](const SshServer& s) { return s.id == linked->second; });
        if (s == servers.end()) { // removed: its last address stays
            servers_.erase(linked);
            continue;
        }
        const QUrl uri(host->uri());
        if (const QString now = libvirtUri(loginOf(*s), machinesOf(uri), uri); now != host->uri())
            putHost(host, host->name(), now.toStdString(), s->id);
    }
    save();
}

void VmView::save() const
{
    settings_->remove(QStringLiteral("libvirtHosts"));
    const size_t first = local_ ? 1 : 0;
    settings_->beginWriteArray(QStringLiteral("libvirtHosts"), int(hosts_.size() - first));
    for (size_t i = first; i < hosts_.size(); ++i) {
        settings_->setArrayIndex(int(i - first));
        settings_->setValue(QStringLiteral("name"), hosts_[i]->name());
        settings_->setValue(QStringLiteral("uri"), hosts_[i]->uri());
        if (const auto server = servers_.find(hosts_[i]); server != servers_.end()) settings_->setValue(QStringLiteral("server"), server->second);
    }
    settings_->endArray();
}

void VmView::editHost(LibvirtVms* host)
{
    if (host && !host->remote()) return;
    const QUrl old(host ? host->uri() : QString());
    QDialog dialog(this);
    dialog.setWindowTitle(host ? tr("Edit libvirt Host") : tr("Add libvirt Host"));
    auto* name = new QLineEdit(host ? host->name() : QString());
    auto* server = new QComboBox;
    auto* hostName = new QLineEdit(old.host());
    auto* port = new QSpinBox;
    port->setRange(1, 65535);
    port->setValue(old.port(22));
    auto* user = new QLineEdit(old.userName());
    auto* keyFile = new QLineEdit(QUrlQuery(old).queryItemValue(QStringLiteral("keyfile"), QUrl::FullyDecoded));
    auto* browse = new QPushButton(tr("Browse…"));
    auto* keyRow = new QWidget;
    auto* keyLayout = new QHBoxLayout(keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->addWidget(keyFile, 1);
    keyLayout->addWidget(browse);
    auto* machines = new QComboBox;
    machines->addItem(tr("System"), QStringLiteral("system"));
    machines->setItemData(0, tr("The machines libvirt runs for the whole host (/system)"), Qt::ToolTipRole);
    machines->addItem(tr("User session"), QStringLiteral("session"));
    machines->setItemData(1, tr("The user's own machines, run as that user (/session)"), Qt::ToolTipRole);
    machines->setCurrentIndex(machines->findData(machinesOf(old)));
    auto* address = new QLineEdit;
    address->setReadOnly(true);
    auto* form = new QFormLayout;
    auto* layout = new QVBoxLayout(&dialog); // before hints are shown: a widget without a parent would be a window
    layout->addLayout(form);
    QLabel* nameHint = look::hintedRow(form, tr("&Name:"), name).hint;
    QLabel* serverHint = look::hintedRow(form, tr("&SSH server:"), server).hint;
    look::hintedRow(form, tr("&Host:"), hostName,
                    tr("A host name, an address or a Host of ~/.ssh/config. Its key must be known here: ssh to it once."));
    form->addRow(tr("&Port:"), port);
    look::hintedRow(form, tr("&User name:"), user,
                    tr("Leave empty for your user name on this computer. It must be allowed to use libvirt there: in the libvirt group."));
    QLabel* keyLabel = look::hintedRow(form, tr("Private &key:"), keyRow,
                                       tr("Leave empty for your SSH agent and ~/.ssh default keys. libvirt can't type a passphrase: "
                                          "use a key without one, or one in your SSH agent."))
                           .label;
    keyLabel->setBuddy(keyFile);
    form->addRow(tr("&Machines:"), machines);
    look::hintedRow(form, tr("&Address:"), address, tr("Made of the fields above."));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);

    std::vector<SshServer> servers;
    auto chosen = [&]() -> const SshServer* {
        const QString id = server->currentData().toString();
        const auto s = std::find_if(servers.begin(), servers.end(), [&](const SshServer& s) { return s.id == id; });
        return id.isEmpty() || s == servers.end() ? nullptr : &*s;
    };
    auto useServer = [&] { // its login in the fields, not editable here
        const SshServer* s = chosen();
        if (s) {
            const SshLogin login = loginOf(*s);
            hostName->setText(login.host);
            port->setValue(login.port);
            user->setText(login.user);
            keyFile->setText(login.keyFile);
        }
        for (QWidget* w : {static_cast<QWidget*>(hostName), static_cast<QWidget*>(port), static_cast<QWidget*>(user), keyRow}) w->setEnabled(!s);
        look::setHint(nameHint, s ? tr("Leave empty to show the server's name.") : tr("Leave empty to show the host."));
        look::setHint(serverHint, s && s->auth != QLatin1String("publicKey")
                                      ? tr("This server logs in with a password, which libvirt can't type: it tries your SSH agent "
                                           "and ~/.ssh default keys instead.")
                                      : QString());
        if (dialog.isVisible()) dialog.resize(dialog.width(), dialog.layout()->totalHeightForWidth(dialog.width())); // the hint's lines
    };
    SshServersRefill fill = [&](const QString& select, const std::vector<SshServer>& list) {
        servers = list;
        {
            const QSignalBlocker block(server);
            server->clear();
            server->addItem(tr("None: the fields below"), QString());
            for (const SshServer& s : servers) server->addItem(icons::lucide("server"), s.name + QStringLiteral("  (") + s.address() + QLatin1Char(')'), s.id);
            offerNewSshServer(server, fill);
            server->setCurrentIndex(std::max(0, server->findData(select)));
        }
        useServer();
    };
    const auto linked = servers_.find(host);
    fill(linked != servers_.end() ? linked->second : QString(), loadSshServers(settings_));
    connect(server, &QComboBox::currentIndexChanged, &dialog, useServer);
    auto update = [&] {
        const bool any = !hostName->text().trimmed().isEmpty();
        address->setText(any ? libvirtUri({user->text(), hostName->text(), keyFile->text(), port->value()}, machines->currentData().toString(), old) : QString());
        address->setCursorPosition(0);
        ok->setEnabled(any);
    };
    for (QLineEdit* e : {hostName, user, keyFile}) connect(e, &QLineEdit::textChanged, &dialog, update);
    connect(port, &QSpinBox::valueChanged, &dialog, update);
    connect(machines, &QComboBox::currentIndexChanged, &dialog, update);
    connect(browse, &QPushButton::clicked, &dialog, [&] {
        const QString path = QFileDialog::getOpenFileName(&dialog, tr("Private Key"), keyFile->text().isEmpty() ? QDir::homePath() + QStringLiteral("/.ssh") : keyFile->text());
        if (!path.isEmpty()) keyFile->setText(path);
    });
    update();
    layout->addWidget(buttons);
    dialog.resize(560, std::max(dialog.sizeHint().height(), dialog.heightForWidth(560))); // the hints wrapped
    if (dialog.exec() != QDialog::Accepted) return;
    const SshServer* s = chosen();
    const QString title = !name->text().trimmed().isEmpty() ? name->text().trimmed() : s ? s->name : hostName->text().trimmed();
    putHost(host, title, address->text().toStdString(), s ? s->id : QString());
}

void VmView::removeHost(LibvirtVms* host, bool ask)
{
    if (!host || !host->remote()) return;
    if (ask) {
        if (!prompts::confirm(this, tr("Remove Host"),
                              tr("Remove \"%1\" from the list? Nothing changes on the host; its open consoles close.").arg(host->name()), tr("&Remove")))
            return;
    }
    std::erase(hosts_, host);
    servers_.erase(host);
    delete host;
    save();
    rebuild();
}

LibvirtVms* VmView::selectedHost() const
{
    const QTreeWidgetItem* item = tree_->currentItem();
    LibvirtVms* host = item ? hostOf(item) : nullptr;
    return std::find(hosts_.begin(), hosts_.end(), host) != hosts_.end() ? host : nullptr; // its rows outlive it until rebuild()
}

const Vm* VmView::selected() const
{
    const QTreeWidgetItem* item = tree_->currentItem();
    LibvirtVms* host = selectedHost();
    if (!item || !host) return nullptr;
    const std::string uuid = item->data(0, Qt::UserRole).toString().toStdString();
    for (const Vm& vm : host->vms())
        if (vm.uuid == uuid) return &vm;
    return nullptr;
}

static QString hostState(const LibvirtVms* host)
{
    if (host->connecting() && !host->connected()) return VmView::tr("connecting…");
    if (!host->connected()) return VmView::tr("not reachable");
    if (host->readOnly()) return VmView::tr("read-only");
    return {};
}

void VmView::rebuild()
{
    const Rebuild quiet(tree_);
    const Vm* current = selected();
    const std::string keep = current ? current->uuid : std::string();
    LibvirtVms* keepHost = selectedHost();
    tree_->clear();
    for (LibvirtVms* host : hosts_) {
        auto* parent = new QTreeWidgetItem(tree_, {host->name()});
        parent->setIcon(0, icons::lucide("server"));
        parent->setData(0, PanelDelegate::HeaderRole, hostState(host));
        parent->setData(0, PanelDelegate::HeaderColorRole, host->connected() || host->connecting() ? palette().color(QPalette::Disabled, QPalette::Text)
                                                                                                : QColor(0xda, 0x44, 0x53));
        parent->setData(0, HostRole, QVariant::fromValue(quintptr(host)));
        parent->setData(0, FoldRole, host->uri());
        const QString tip = host->uri() + (host->lastError().isEmpty() ? QString() : QLatin1Char('\n') + host->lastError());
        parent->setToolTip(0, tip);
        if (host == keepHost && keep.empty()) tree_->setCurrentItem(parent);
        const QString hint = !host->connected() && !host->connecting() ? tr("Trying again every 5 seconds")
                             : host->connected() && host->vms().empty() ? tr("No virtual machines")
                                                                        : QString();
        if (!hint.isEmpty()) {
            auto* item = new QTreeWidgetItem(parent, {hint});
            item->setData(0, HostRole, QVariant::fromValue(quintptr(host)));
            item->setToolTip(0, tip);
        }
        for (const Vm& vm : host->vms()) {
            auto* item = new QTreeWidgetItem(parent, {QString::fromStdString(vm.name)}); // drawn by VmDelegate; the text for type-ahead
            item->setData(0, Qt::UserRole, QString::fromStdString(vm.uuid));
            item->setData(0, HostRole, QVariant::fromValue(quintptr(host)));
            QString tip = QString::fromStdString(vm.name) + QLatin1Char('\n') + stateText(vm.state);
            if (const auto& d = vm.display)
                tip += QStringLiteral("\n%1://%2:%3").arg(QLatin1String(protocolName(d->protocol)), QString::fromStdString(d->host))
                           .arg(d->port > 0 ? d->port : d->number("tlsPort"));
            item->setToolTip(0, tip + QLatin1Char('\n') + tr("The graph: its CPU use over the last 80 seconds"));
            if (host == keepHost && vm.uuid == keep) tree_->setCurrentItem(item);
        }
        folding_->apply(parent);
    }
    if (!tree_->currentItem() && tree_->topLevelItemCount()) tree_->setCurrentItem(tree_->topLevelItem(0));
    note_->setVisible(hosts_.empty());
    updateActions();
}

void VmView::updateActions()
{
    LibvirtVms* host = selectedHost();
    const Vm* vm = selected();
    updateActions(actions_, host, vm);
    const bool remoteRow = host && host->remote() && !vm;
    editHost_->setEnabled(remoteRow);
    removeHost_->setEnabled(remoteRow);
}

void VmView::updateActions(const Actions& a, LibvirtVms* host, const Vm* vm) const
{
    const bool control = vm && host && host->connected() && !host->readOnly();
    const State s = vm ? vm->state : State::Other;
    auto allowed = [&](Action a) { return control && LibvirtVms::allowed(a, s); };
    const bool resume = LibvirtVms::allowed(Action::Resume, s);
    a.start->setText(resume ? tr("&Resume") : tr("&Start"));
    a.start->setEnabled(allowed(Action::Start) || allowed(Action::Resume));
    a.pause->setEnabled(allowed(Action::Pause));
    a.shutdown->setEnabled(allowed(Action::Shutdown));
    a.reboot->setEnabled(allowed(Action::Reboot));
    a.save->setEnabled(allowed(Action::Save));
    a.forceOff->setEnabled(allowed(Action::ForceOff));
    a.reset->setEnabled(allowed(Action::Reset));
    a.open->setEnabled(vm);
    a.serial->setEnabled(control && (s == State::Running || s == State::Paused));
    a.settings->setEnabled(control);
    a.snapshots->setEnabled(control);
}

void VmView::act(LibvirtVms* host, const Vm& vm, Action action)
{
    const QString name = QString::fromStdString(vm.name);
    const std::string uuid = vm.uuid; // vm may be gone after the question below
    if (action == Action::ForceOff || action == Action::Reset) {
        const bool off = action == Action::ForceOff;
        if (!prompts::confirm(this, off ? tr("Force Off") : tr("Reset"),
                              off ? tr("Force off \"%1\"? It is like pulling the power plug: unsaved work in it is lost.").arg(name)
                                  : tr("Reset \"%1\"? It is like pressing its reset button: unsaved work in it is lost.").arg(name),
                              off ? tr("&Force Off") : tr("R&eset"), QStyle::SP_MessageBoxWarning))
            return;
    }
    host->control(uuid, action);
}

void VmView::editSettings(LibvirtVms* host, const std::string& uuid)
{
    const auto vm = std::find_if(host->vms().begin(), host->vms().end(), [&](const Vm& v) { return v.uuid == uuid; });
    if (vm == host->vms().end()) return;
    const QString name = QString::fromStdString(vm->name);
    QString error;
    const std::optional<VmSettings> was = host->settings(uuid, error);
    if (!was) return (void)QMessageBox::warning(this, tr("Virtual machine"), tr("%1: %2").arg(name, error));
    VmSettingsDialog dialog(name, *was, [&](const VmSettings& now) { return host->apply(uuid, *was, now); }, this);
    if (const QString virtManager = QStandardPaths::findExecutable(QStringLiteral("virt-manager")); !virtManager.isEmpty())
        dialog.addMore(tr("More in virt-manager…"), [virtManager, uri = host->uri(), uuid] {
            QProcess::startDetached(virtManager, {QStringLiteral("--connect"), uri, QStringLiteral("--show-domain-editor"), QString::fromStdString(uuid)});
        });
    dialog.exec();
}

void VmView::editSnapshots(LibvirtVms* host, const std::string& uuid)
{
    const auto vm = std::find_if(host->vms().begin(), host->vms().end(), [&](const Vm& v) { return v.uuid == uuid; });
    if (vm == host->vms().end()) return;
    VmSnapshotsDialog dialog(
        QString::fromStdString(vm->name),
        [host, uuid] {
            const auto& vms = host->vms();
            const auto it = std::find_if(vms.begin(), vms.end(), [&](const Vm& v) { return v.uuid == uuid; });
            return it != vms.end() && (it->state == State::Running || it->state == State::Paused);
        },
        [host, uuid](QString& error) { return host->snapshots(uuid, error); },
        [host, uuid](SnapshotOp op, const QString& name, VmSnapshotsDialog::Done done) { host->snapshot(uuid, op, name, std::move(done)); }, this);
    dialog.exec();
}
