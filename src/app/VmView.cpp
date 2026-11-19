#include "app/VmView.h"

#include "app/ConnectionStore.h"
#include "app/PanelTree.h"
#include "app/Icons.h"

#include <QAction>
#include <QUrl>
#include <QSettings>
#include <QLineEdit>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QDialog>
#include <QApplication>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>
#include <set>
#include <cstdlib>
#include <functional>

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
    case State::Running: return "player-play";
    case State::Paused:
    case State::Suspended: return "player-pause";
    case State::ShuttingDown: return "power";
    case State::Saved: return "device-floppy";
    case State::Crashed: return "alert-triangle";
    default: return nullptr;
    }
}

constexpr int HostRole = Qt::UserRole + 1; // its LibvirtVms; Qt::UserRole: the VM's uuid, empty on a host row

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

} // namespace

VmView::VmView(QSettings* settings, QWidget* parent) : QWidget(parent), settings_(settings)
{
    tree_ = new QTreeWidget;
    tree_->setHeaderHidden(true);
    tree_->setFrameShape(QFrame::NoFrame);
    setUpPanels(tree_, new VmDelegate(tree_));
    tree_->setContextMenuPolicy(Qt::ActionsContextMenu);
    note_ = new QLabel(tr("Add a KVM host with +: libvirt over SSH (qemu+ssh://)."));
    note_->setWordWrap(true);
    note_->setContentsMargins(10, 6, 10, 6);
    note_->hide();
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(note_);
    layout->addWidget(tree_, 1);

    auto make = [this](const char* icon, const QString& text, std::function<void()> run) {
        auto* a = new QAction(icons::tabler(icon), text, this);
        connect(a, &QAction::triggered, this, run);
        return a;
    };
    open_ = make("device-desktop", tr("&Open"), [this] {
        if (const Vm* vm = selected()) emit openRequested(selectedHost(), vm->uuid);
    });
    open_->setToolTip(tr("Its console: the display, or a power button while it is off"));
    start_ = make("player-play", tr("&Start"), [this] { // Resume while paused, as virt-manager's Run
        const Vm* vm = selected();
        act(vm && LibvirtVms::allowed(Action::Resume, vm->state) ? Action::Resume : Action::Start);
    });
    pause_ = make("player-pause", tr("&Pause"), [this] { act(Action::Pause); });
    shutdown_ = make("power", tr("Shut &Down"), [this] { act(Action::Shutdown); });
    shutdown_->setToolTip(tr("Asks the guest system to shut down"));
    reboot_ = make("refresh", tr("&Reboot"), [this] { act(Action::Reboot); });
    save_ = make("device-floppy", tr("Sa&ve"), [this] { act(Action::Save); });
    save_->setToolTip(tr("Saves its memory to disk and stops it; Start continues where it was"));
    forceOff_ = make("plug-off", tr("Force &Off"), [this] { act(Action::ForceOff); });
    reset_ = make("rotate-2", tr("R&eset"), [this] { act(Action::Reset); });
    serial_ = make("terminal", tr("Serial &Console"), [this] {
        if (const Vm* vm = selected()) emit serialRequested(selectedHost(), vm->uuid);
    });
    serial_->setToolTip(tr("Its serial console in a terminal (a console device in the VM, a getty in the guest)"));
    addHost_ = make("plus", tr("&Add Host…"), [this] { editHost(nullptr); });
    addHost_->setToolTip(tr("A libvirt host over SSH"));
    editHost_ = make("pencil", tr("&Edit Host…"), [this] { editHost(selectedHost()); });
    removeHost_ = make("trash", tr("Re&move Host"), [this] { removeHost(selectedHost()); });
    auto separator = [this] {
        auto* s = new QAction(this);
        s->setSeparator(true);
        return s;
    };
    tree_->addActions({open_, serial_, separator(), start_, pause_, separator(), shutdown_, reboot_, save_, separator(), forceOff_, reset_,
                       separator(), addHost_, editHost_, removeHost_});

    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        if (!item->data(0, Qt::UserRole).toString().isEmpty()) open_->trigger();
        else foldLater(tree_, tree_->indexFromItem(item));
    });
    connect(tree_, &QTreeWidget::currentItemChanged, this, &VmView::updateActions);

    const char* testUri = std::getenv("PORTAL_MUX_LIBVIRT_URI"); // test:///<file>: fake VMs for UI tests
#if defined(_WIN32) || defined(Q_OS_MACOS)
    local_ = testUri && *testUri;
#endif
    if (local_) addHost(tr("This computer"), testUri && *testUri ? testUri : "qemu:///system");
    const int n = settings_->beginReadArray(QStringLiteral("libvirtHosts"));
    std::vector<std::pair<QString, std::string>> saved;
    for (int i = 0; i < n; ++i) {
        settings_->setArrayIndex(i);
        saved.emplace_back(settings_->value(QStringLiteral("name")).toString(), settings_->value(QStringLiteral("uri")).toString().toStdString());
    }
    settings_->endArray();
    for (const auto& [name, uri] : saved)
        if (!uri.empty()) addHost(name, uri);
    rebuild();
}

QList<QAction*> VmView::titleActions() const { return {addHost_, open_, start_, pause_, shutdown_, forceOff_}; }

void VmView::addHost(const QString& name, const std::string& uri)
{
    auto* host = new LibvirtVms(name, uri, this);
    hosts_.push_back(host);
    connect(host, &LibvirtVms::changed, this, &VmView::rebuild);
    connect(host, &LibvirtVms::changed, this, &VmView::vmsChanged);
    connect(host, &LibvirtVms::statsChanged, tree_->viewport(), qOverload<>(&QWidget::update));
    connect(host, &LibvirtVms::failed, this, [this](const QString&, const QString& name, const QString& text) {
        QMessageBox::warning(this, tr("Virtual machine"), name.isEmpty() ? text : tr("%1: %2").arg(name, text));
    });
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
    }
    settings_->endArray();
}

void VmView::editHost(LibvirtVms* host)
{
    if (host && !host->remote()) return;
    QDialog dialog(this);
    dialog.setWindowTitle(host ? tr("Edit libvirt Host") : tr("Add libvirt Host"));
    auto* name = new QLineEdit(host ? host->name() : QString());
    auto* address = new QLineEdit(host ? host->uri() : QString());
    address->setPlaceholderText(tr("user@host, a ~/.ssh/config name, or qemu+ssh://user@host/system"));
    auto* hint = new QLabel(tr("Connects with ssh, as virsh does: your keys, agent and ~/.ssh/config, no password prompt. "
                               "The host's key must be known (ssh to it once), and its user must be allowed to use libvirt."));
    hint->setWordWrap(true);
    auto* form = new QFormLayout;
    form->addRow(tr("&Name:"), name);
    form->addRow(tr("&Address:"), address);
    form->addRow(hint);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto validate = [&] { buttons->button(QDialogButtonBox::Ok)->setEnabled(!address->text().trimmed().isEmpty()); };
    connect(address, &QLineEdit::textChanged, &dialog, validate);
    validate();
    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(buttons);
    dialog.resize(520, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted) return;
    QString uri = address->text().trimmed();
    if (!uri.contains(QLatin1String("://"))) uri = QStringLiteral("qemu+ssh://%1/system").arg(uri);
    const QString title = name->text().trimmed().isEmpty() ? QUrl(uri).host() : name->text().trimmed();
    if (host) removeHost(host, false); // its consoles close with it
    addHost(title, uri.toStdString());
    save();
    rebuild();
}

void VmView::removeHost(LibvirtVms* host, bool ask)
{
    if (!host || !host->remote()) return;
    if (ask) {
        QMessageBox box(QMessageBox::Question, tr("Remove Host"),
                        tr("Remove \"%1\" from the list? Nothing changes on the host; its open consoles close.").arg(host->name()),
                        QMessageBox::Yes | QMessageBox::No, this);
        box.setDefaultButton(QMessageBox::No);
        box.setOption(QMessageBox::Option::DontUseNativeDialog); // KDE's box: Enter presses the first button
        if (box.exec() != QMessageBox::Yes) return;
    }
    std::erase(hosts_, host);
    delete host;
    save();
    rebuild();
}

LibvirtVms* VmView::selectedHost() const
{
    const QTreeWidgetItem* item = tree_->currentItem();
    return item ? hostOf(item) : nullptr;
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
    const Vm* current = selected();
    const std::string keep = current ? current->uuid : std::string();
    LibvirtVms* keepHost = selectedHost();
    std::set<LibvirtVms*> collapsed;
    for (int i = 0; i < tree_->topLevelItemCount(); ++i)
        if (QTreeWidgetItem* top = tree_->topLevelItem(i); top->childCount() && !top->isExpanded())
            collapsed.insert(hostOf(top));
    tree_->clear();
    for (LibvirtVms* host : hosts_) {
        auto* parent = new QTreeWidgetItem(tree_, {host->name()});
        parent->setIcon(0, icons::tabler("server"));
        parent->setData(0, PanelDelegate::HeaderRole, hostState(host));
        parent->setData(0, PanelDelegate::HeaderColorRole, host->connected() || host->connecting() ? palette().color(QPalette::Disabled, QPalette::Text)
                                                                                                : QColor(0xda, 0x44, 0x53));
        parent->setData(0, HostRole, QVariant::fromValue(quintptr(host)));
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
        parent->setExpanded(!collapsed.count(host));
    }
    if (!tree_->currentItem() && tree_->topLevelItemCount()) tree_->setCurrentItem(tree_->topLevelItem(0));
    note_->setVisible(hosts_.empty());
    updateActions();
}

void VmView::updateActions()
{
    const Vm* vm = selected();
    LibvirtVms* host = selectedHost();
    const bool control = vm && host && host->connected() && !host->readOnly();
    const State s = vm ? vm->state : State::Other;
    auto allowed = [&](Action a) { return control && LibvirtVms::allowed(a, s); };
    const bool resume = LibvirtVms::allowed(Action::Resume, s);
    start_->setText(resume ? tr("&Resume") : tr("&Start"));
    start_->setEnabled(allowed(Action::Start) || allowed(Action::Resume));
    pause_->setEnabled(allowed(Action::Pause));
    shutdown_->setEnabled(allowed(Action::Shutdown));
    reboot_->setEnabled(allowed(Action::Reboot));
    save_->setEnabled(allowed(Action::Save));
    forceOff_->setEnabled(allowed(Action::ForceOff));
    reset_->setEnabled(allowed(Action::Reset));
    open_->setEnabled(vm);
    serial_->setEnabled(control && (s == State::Running || s == State::Paused));
    const bool remoteRow = host && host->remote() && !vm;
    editHost_->setEnabled(remoteRow);
    removeHost_->setEnabled(remoteRow);
}

void VmView::act(Action action)
{
    const Vm* vm = selected();
    LibvirtVms* host = selectedHost();
    if (!vm || !host) return;
    const QString name = QString::fromStdString(vm->name);
    const std::string uuid = vm->uuid; // vm may be gone after the question below
    if (action == Action::ForceOff || action == Action::Reset) {
        const bool off = action == Action::ForceOff;
        QMessageBox box(QMessageBox::Warning, off ? tr("Force Off") : tr("Reset"),
                        off ? tr("Force off \"%1\"? It is like pulling the power plug: unsaved work in it is lost.").arg(name)
                            : tr("Reset \"%1\"? It is like pressing its reset button: unsaved work in it is lost.").arg(name),
                        QMessageBox::Cancel, this);
        const QAbstractButton* yes = box.addButton(off ? tr("&Force Off") : tr("R&eset"), QMessageBox::DestructiveRole);
        box.setDefaultButton(QMessageBox::Cancel);
        box.setOption(QMessageBox::Option::DontUseNativeDialog); // KDE's box: Enter presses the first button
        box.exec();
        if (box.clickedButton() != yes) return;
    }
    host->control(uuid, action);
}
