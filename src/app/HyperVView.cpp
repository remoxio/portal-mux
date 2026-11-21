#include "app/HyperVView.h"

#include "app/Icons.h"
#include "app/PanelTree.h"
#include "app/Secrets.h"
#include "app/SshServers.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>

using hyperv::Action;
using hyperv::State;
using hyperv::Vm;
using Status = HyperVHost::Status;

QString HyperVView::stateText(State s)
{
    switch (s) {
    case State::Running: return tr("Running");
    case State::Off: return tr("Off");
    case State::Saved: return tr("Saved");
    case State::Paused: return tr("Paused");
    case State::Starting: return tr("Starting");
    case State::Stopping: return tr("Shutting down");
    case State::Saving: return tr("Saving");
    case State::Pausing: return tr("Pausing");
    case State::Resuming: return tr("Resuming");
    case State::Resetting: return tr("Resetting");
    case State::Other: break;
    }
    return tr("Unknown");
}

namespace {

// Breeze: positive, neutral, focus; dimmed text
QColor stateColor(State s, const QPalette& palette)
{
    switch (s) {
    case State::Running: return QColor(0x27, 0xae, 0x60);
    case State::Paused:
    case State::Pausing:
    case State::Resuming:
    case State::Starting:
    case State::Stopping:
    case State::Resetting: return QColor(0xf6, 0x74, 0x00);
    case State::Saved:
    case State::Saving: return QColor(0x3d, 0xae, 0xe9);
    default: return palette.color(QPalette::Disabled, QPalette::Text);
    }
}

const char* stateGlyph(State s)
{
    switch (s) {
    case State::Running:
    case State::Resuming: return "play";
    case State::Paused:
    case State::Pausing: return "pause";
    case State::Starting:
    case State::Stopping:
    case State::Resetting: return "power";
    case State::Saved:
    case State::Saving: return "save";
    default: return nullptr;
    }
}

bool active(State s) { return s != State::Off && s != State::Saved && s != State::Other; }

constexpr int HostRole = Qt::UserRole + 1; // the SSH server's id; Qt::UserRole: the VM's id, empty on other rows

QString uptimeText(long long s)
{
    const QString time = QStringLiteral("%1:%2:%3").arg((s % 86400) / 3600).arg((s % 3600) / 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
    return s >= 86400 ? HyperVView::tr("%n day(s)", "", int(s / 86400)) + QLatin1Char(' ') + time : time;
}

// a VM row as a GuestRow; host rows are panel headers
class HyperVDelegate : public PanelDelegate {
public:
    HyperVDelegate(std::function<const HyperVHost*(const QString&)> host, QObject* parent) : PanelDelegate(parent), host_(std::move(host)) {}

protected:
    std::optional<GuestRow> row(const QModelIndex& index) const override
    {
        const std::string id = index.data(Qt::UserRole).toString().toStdString();
        const HyperVHost* host = host_(index.data(HostRole).toString());
        if (id.empty() || !host) return std::nullopt;
        const auto vm = std::find_if(host->vms().begin(), host->vms().end(), [&](const Vm& v) { return v.id == id; });
        if (vm == host->vms().end()) return std::nullopt;
        GuestRow row;
        row.active = active(vm->state);
        row.glyph = stateGlyph(vm->state);
        row.glyphColor = stateColor(vm->state, QApplication::palette());
        row.name = QString::fromStdString(vm->name);
        row.detail = HyperVView::stateText(vm->state) + (vm->ip.empty() ? QString() : QStringLiteral(" · ") + QString::fromStdString(vm->ip));
        if (const std::deque<float>* cpu = host->cpu(vm->id)) row.cpu.assign(cpu->begin(), cpu->end());
        return row;
    }

private:
    std::function<const HyperVHost*(const QString&)> host_;
};

} // namespace

HyperVView::HyperVView(QSettings* settings, QString configDir, QWidget* parent)
    : QWidget(parent), settings_(settings), configDir_(std::move(configDir))
{
    tree_ = new QTreeWidget;
    tree_->setHeaderHidden(true);
    tree_->setFrameShape(QFrame::NoFrame);
    setUpPanels(tree_, new HyperVDelegate([this](const QString& id) -> const HyperVHost* {
        const auto it = hosts_.find(id);
        return it == hosts_.end() ? nullptr : it->second;
    }, tree_));
    folding_ = new Folding(tree_, settings_, QStringLiteral("folded/hyperv"), HostRole);
    tree_->setContextMenuPolicy(Qt::ActionsContextMenu);
    note_ = new QLabel(tr("Add a Hyper-V host with +: a server of Settings → SSH Servers, with an administrator's account."));
    note_->setWordWrap(true);
    note_->setContentsMargins(10, 6, 10, 6);
    note_->hide();
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
    open_ = make("monitor", tr("&Open"), [this] { open(); });
    open_->setToolTip(tr("Its console; starts it first if it is off"));
    start_ = make("play", tr("&Start"), [this] {
        const Vm* vm = selected();
        act(vm && hyperv::allowed(Action::Resume, vm->state) ? Action::Resume : Action::Start);
    });
    pause_ = make("pause", tr("&Pause"), [this] { act(Action::Pause); });
    shutdown_ = make("power", tr("Shut &Down"), [this] { act(Action::Shutdown); });
    shutdown_->setToolTip(tr("Asks the guest system to shut down (needs Hyper-V's integration services in it)"));
    reboot_ = make("rotate-cw", tr("&Restart"), [this] { act(Action::Reboot); });
    save_ = make("save", tr("Sa&ve"), [this] { act(Action::Save); });
    save_->setToolTip(tr("Saves its memory to disk and stops it; Start continues where it was"));
    turnOff_ = make("power-off", tr("&Turn Off"), [this] { act(Action::TurnOff); });
    reset_ = make("rotate-ccw", tr("R&eset"), [this] { act(Action::Reset); });
    addHost_ = make("plus", tr("&Add Host…"), [this] { addHost(); });
    addHost_->setToolTip(tr("A Hyper-V host from Settings → SSH Servers"));
    removeHost_ = make("trash", tr("Re&move Host"), [this] { removeHost(selectedHost()); });
    auto separator = [this] {
        auto* s = new QAction(this);
        s->setSeparator(true);
        return s;
    };
    tree_->addActions({open_, separator(), start_, pause_, separator(), shutdown_, reboot_, save_, separator(), turnOff_, reset_,
                       separator(), addHost_, removeHost_});

    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        if (!item->data(0, Qt::UserRole).toString().isEmpty()) return open();
        if (!item->parent()) return foldLater(tree_, tree_->indexFromItem(item));
        HyperVHost* host = selectedHost(); // a hint row
        if (!host) return;
        if (host->status() == Status::LoginFailed) {
            connectHost(host->server().id);
        } else if (host->status() == Status::Unreachable) {
            host->reconnect();
        } else if (host->status() == Status::NoModule) {
            QMessageBox box(QMessageBox::Question, tr("Hyper-V PowerShell Module"),
                            tr("Install Hyper-V's PowerShell module on \"%1\"? It is a Windows feature (Hyper-V-PowerShell); "
                               "it takes a minute or two and needs no restart.").arg(host->server().name),
                            QMessageBox::Yes | QMessageBox::No, this);
            box.setOption(QMessageBox::Option::DontUseNativeDialog); // KDE's box: Enter presses the first button
            if (box.exec() != QMessageBox::Yes) return;
            host->installModule();
            emit message(tr("Installing Hyper-V's PowerShell module on %1…").arg(host->server().name));
        }
    });
    connect(tree_, &QTreeWidget::currentItemChanged, this, &HyperVView::updateActions);

    const int n = settings_->beginReadArray(QStringLiteral("hypervHosts"));
    for (int i = 0; i < n; ++i) {
        settings_->setArrayIndex(i);
        if (const QString id = settings_->value(QStringLiteral("server")).toString(); !id.isEmpty() && !ids_.contains(id)) ids_ << id;
    }
    settings_->endArray();
    reloadServers();
    for (const QString& id : std::as_const(ids_)) connectHost(id);
}

QList<QAction*> HyperVView::titleActions() const { return {addHost_, open_, start_, pause_, shutdown_, turnOff_}; }

void HyperVView::save() const
{
    settings_->remove(QStringLiteral("hypervHosts"));
    settings_->beginWriteArray(QStringLiteral("hypervHosts"), int(ids_.size()));
    for (int i = 0; i < ids_.size(); ++i) {
        settings_->setArrayIndex(i);
        settings_->setValue(QStringLiteral("server"), ids_[i]);
    }
    settings_->endArray();
}

void HyperVView::connectHost(const QString& id)
{
    const auto server = findSshServer(settings_, id);
    if (!server) return;
    auto create = [this, server = *server](const QString& password) {
        if (!ids_.contains(server.id)) return; // removed meanwhile
        if (const auto old = hosts_.find(server.id); old != hosts_.end()) {
            if (pendingOpen_.first == old->second) pendingOpen_ = {};
            delete old->second;
            hosts_.erase(old);
        }
        auto* host = new HyperVHost(server, password, (configDir_ + QStringLiteral("/known_hosts")).toStdString(), this, this);
        hosts_[server.id] = host;
        connect(host, &HyperVHost::changed, this, [this, host] { hostChanged(host); });
        connect(host, &HyperVHost::statsChanged, tree_->viewport(), qOverload<>(&QWidget::update));
        connect(host, &HyperVHost::failed, this, [this, host](const QString& text) {
            if (pendingOpen_.first == host) pendingOpen_ = {};
            QMessageBox::warning(this, tr("Hyper-V"), tr("%1: %2").arg(host->server().name, text));
        });
        rebuild();
    };
    if (server->auth == QLatin1String("password")) secrets::read(sshServerSecret(id), this, create);
    else create({});
}

void HyperVView::reloadServers()
{
    for (const QString& id : QStringList(ids_)) {
        const auto server = findSshServer(settings_, id);
        const auto host = hosts_.find(id);
        if (!server) {
            ids_.removeAll(id);
            if (host != hosts_.end()) {
                if (pendingOpen_.first == host->second) pendingOpen_ = {};
                delete host->second;
                hosts_.erase(host);
            }
            continue;
        }
        if (host == hosts_.end()) continue; // its password is still being read
        const SshServer& s = host->second->server();
        if (s.name != server->name || s.host != server->host || s.user != server->user || s.auth != server->auth || s.keyFile != server->keyFile)
            connectHost(id);
    }
    save();
    rebuild();
}

void HyperVView::addHost()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Add Hyper-V Host"));
    auto* server = new QComboBox;
    server->setPlaceholderText(tr("Choose an SSH server"));
    server->setSizeAdjustPolicy(QComboBox::AdjustToContents); // starts empty
    SshServersRefill fill = [&](const QString& select, const std::vector<SshServer>& servers) {
        server->clear();
        for (const SshServer& s : servers)
            if (!ids_.contains(s.id)) server->addItem(icons::lucide("server"), s.name + QStringLiteral("  (") + s.address() + QLatin1Char(')'), s.id);
        offerNewSshServer(server, fill);
        server->setCurrentIndex(server->findData(select));
    };
    fill({}, loadSshServers(settings_));
    auto* hint = new QLabel(tr("Portal Mux runs PowerShell on it over SSH: it needs OpenSSH Server (an optional feature of Windows), "
                               "and the account must be an administrator or in Hyper-V Administrators."));
    hint->setWordWrap(true);
    auto* form = new QFormLayout;
    form->addRow(tr("&SSH server:"), server);
    form->addRow(hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
    auto validate = [server, ok] { ok->setEnabled(!server->currentData().toString().isEmpty()); };
    connect(server, &QComboBox::currentIndexChanged, &dialog, validate);
    validate();
    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(buttons);
    dialog.resize(520, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted) return;
    const QString id = server->currentData().toString();
    ids_ << id;
    save();
    connectHost(id);
}

void HyperVView::removeHost(HyperVHost* host)
{
    if (!host) return;
    QMessageBox box(QMessageBox::Question, tr("Remove Host"),
                    tr("Remove \"%1\" from the list? Nothing changes on the host; it stays in Settings → SSH Servers.").arg(host->server().name),
                    QMessageBox::Yes | QMessageBox::No, this);
    box.setDefaultButton(QMessageBox::No);
    box.setOption(QMessageBox::Option::DontUseNativeDialog); // KDE's box: Enter presses the first button
    if (box.exec() != QMessageBox::Yes) return;
    if (pendingOpen_.first == host) pendingOpen_ = {};
    ids_.removeAll(host->server().id);
    hosts_.erase(host->server().id);
    delete host;
    save();
    rebuild();
}

HyperVHost* HyperVView::selectedHost() const
{
    const QTreeWidgetItem* item = tree_->currentItem();
    if (!item) return nullptr;
    const auto it = hosts_.find(item->data(0, HostRole).toString());
    return it == hosts_.end() ? nullptr : it->second;
}

const Vm* HyperVView::selected() const
{
    const QTreeWidgetItem* item = tree_->currentItem();
    const HyperVHost* host = selectedHost();
    if (!item || !host) return nullptr;
    const std::string id = item->data(0, Qt::UserRole).toString().toStdString();
    for (const Vm& vm : host->vms())
        if (vm.id == id) return &vm;
    return nullptr;
}

void HyperVView::hostChanged(HyperVHost* host)
{
    rebuild();
    if (pendingOpen_.first != host) return;
    for (const Vm& vm : host->vms())
        if (vm.id == pendingOpen_.second && vm.state == State::Running) {
            pendingOpen_ = {};
            emit openRequested(console(host, vm));
            return;
        }
}

void HyperVView::rebuild()
{
    const Rebuild quiet(tree_);
    const Vm* current = selected();
    const std::string keep = current ? current->id : std::string();
    const HyperVHost* keepHost = selectedHost();
    tree_->clear();
    for (const QString& id : std::as_const(ids_)) {
        const auto found = hosts_.find(id);
        if (found == hosts_.end()) continue;
        const HyperVHost* host = found->second;
        QString state, hint;
        switch (host->status()) {
        case Status::Connecting: state = tr("connecting…"); break;
        case Status::Connected:
            if (host->vms().empty()) hint = tr("No virtual machines");
            break;
        case Status::Unreachable:
            state = tr("not reachable");
            hint = tr("Trying again every 5 seconds");
            break;
        case Status::LoginFailed:
            state = tr("login failed");
            hint = tr("Double-click to connect again");
            break;
        case Status::NoModule:
            state = tr("no PowerShell module");
            hint = tr("Double-click to install it");
            break;
        case Status::Refused:
            state = tr("error");
            hint = host->lastError().section(QLatin1Char('\n'), 0, 0);
            break;
        }
        const bool bad = host->status() != Status::Connecting && host->status() != Status::Connected;
        auto* parent = new QTreeWidgetItem(tree_, {host->server().name});
        parent->setIcon(0, icons::lucide("server"));
        parent->setData(0, PanelDelegate::HeaderRole, state);
        parent->setData(0, PanelDelegate::HeaderColorRole, bad ? QColor(0xda, 0x44, 0x53) : palette().color(QPalette::Disabled, QPalette::Text));
        parent->setData(0, HostRole, id);
        const QString tip = host->server().address() + (host->lastError().isEmpty() ? QString() : QLatin1Char('\n') + host->lastError());
        parent->setToolTip(0, tip);
        if (host == keepHost && keep.empty()) tree_->setCurrentItem(parent);
        if (!hint.isEmpty()) {
            auto* item = new QTreeWidgetItem(parent, {hint});
            item->setData(0, HostRole, id);
            item->setToolTip(0, tip);
        }
        for (const Vm& vm : host->vms()) {
            auto* item = new QTreeWidgetItem(parent, {QString::fromStdString(vm.name)}); // drawn by the delegate; the text for type-ahead
            item->setData(0, Qt::UserRole, QString::fromStdString(vm.id));
            item->setData(0, HostRole, id);
            QStringList tip{QString::fromStdString(vm.name), stateText(vm.state)};
            if (active(vm.state)) {
                tip << tr("Memory: %1").arg(QLocale().formattedDataSize(vm.memory)) << tr("Up for %1").arg(uptimeText(vm.uptime));
                if (!vm.ip.empty()) tip << tr("Address: %1").arg(QString::fromStdString(vm.ip));
            }
            tip << tr("Generation %1").arg(vm.generation) << QString::fromStdString(vm.status) << tr("The graph: its CPU use over the last 80 seconds");
            item->setToolTip(0, tip.join(QLatin1Char('\n')));
            if (host == keepHost && vm.id == keep) tree_->setCurrentItem(item);
        }
        folding_->apply(parent);
    }
    if (!tree_->currentItem() && tree_->topLevelItemCount()) tree_->setCurrentItem(tree_->topLevelItem(0));
    note_->setVisible(ids_.isEmpty());
    updateActions();
}

void HyperVView::updateActions()
{
    const Vm* vm = selected();
    const HyperVHost* host = selectedHost();
    const bool control = vm && host && host->status() == Status::Connected;
    const State s = vm ? vm->state : State::Other;
    auto allowed = [&](Action a) { return control && hyperv::allowed(a, s); };
    start_->setText(hyperv::allowed(Action::Resume, s) ? tr("&Resume") : tr("&Start"));
    start_->setEnabled(allowed(Action::Start) || allowed(Action::Resume));
    pause_->setEnabled(allowed(Action::Pause));
    shutdown_->setEnabled(allowed(Action::Shutdown));
    reboot_->setEnabled(allowed(Action::Reboot));
    save_->setEnabled(allowed(Action::Save));
    turnOff_->setEnabled(allowed(Action::TurnOff));
    reset_->setEnabled(allowed(Action::Reset));
    open_->setEnabled(control && (active(s) || hyperv::allowed(Action::Start, s)));
    removeHost_->setEnabled(host && !vm);
}

void HyperVView::act(Action action)
{
    const Vm* vm = selected();
    HyperVHost* host = selectedHost();
    if (!vm || !host) return;
    const QString name = QString::fromStdString(vm->name);
    const std::string id = vm->id; // vm may be gone after the question below
    if (action == Action::TurnOff || action == Action::Reset) {
        const bool off = action == Action::TurnOff;
        QMessageBox box(QMessageBox::Warning, off ? tr("Turn Off") : tr("Reset"),
                        off ? tr("Turn off \"%1\"? It is like pulling the power plug: unsaved work in it is lost.").arg(name)
                            : tr("Reset \"%1\"? It is like pressing its reset button: unsaved work in it is lost.").arg(name),
                        QMessageBox::Cancel, this);
        const QAbstractButton* yes = box.addButton(off ? tr("&Turn Off") : tr("R&eset"), QMessageBox::DestructiveRole);
        box.setDefaultButton(QMessageBox::Cancel);
        box.setOption(QMessageBox::Option::DontUseNativeDialog); // KDE's box: Enter presses the first button
        box.exec();
        if (box.clickedButton() != yes) return;
    }
    host->control(id, action);
}

void HyperVView::open()
{
    const Vm* vm = selected();
    HyperVHost* host = selectedHost();
    if (!vm || !host || host->status() != Status::Connected) return;
    if (vm->state == State::Running || vm->state == State::Paused) return emit openRequested(console(host, *vm));
    if (!active(vm->state) && !hyperv::allowed(Action::Start, vm->state)) return;
    pendingOpen_ = {host, vm->id};
    if (hyperv::allowed(Action::Start, vm->state)) {
        host->control(vm->id, Action::Start);
        emit message(tr("Starting %1…").arg(QString::fromStdString(vm->name)));
    }
}

rdc::Connection HyperVView::console(const HyperVHost* host, const Vm& vm) const
{
    const SshServer& server = host->server();
    rdc::Connection c;
    c.id = "hyperv/" + server.id.toStdString() + "/" + vm.id; // also keys the tunnel's local port: one certificate prompt
    c.name = vm.name;
    c.protocol = rdc::Protocol::RDP;
    c.host = "localhost"; // from the server, through its SSH
    c.port = 2179;        // VMConnect
    c.username = server.user.toStdString();
    c.options["tunnel"] = server.id.toStdString();
    c.options["vmConnect"] = "true";
    c.options["preconnectionBlob"] = vm.id;
    c.options["security"] = "nla";
    c.options["address"] = tr("%1 · Hyper-V").arg(server.name).toStdString();
    return c;
}
