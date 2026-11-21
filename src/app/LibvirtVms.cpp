#include "app/LibvirtVms.h"

#include "app/DomainXml.h"
#include "app/LibvirtConsole.h"

#include <QCoreApplication>
#include <QPointer>
#include <QUrl>
#include <QUrlQuery>

#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

using rdc::Connection;

// ssh can't ask for a password here: keys, agent or nothing
static std::string openUri(std::string uri)
{
#ifdef _WIN32
    // libvirt cannot run ssh on Windows
    if (const size_t at = uri.find("+ssh://"); at != std::string::npos) uri.replace(at, 7, "+libssh://");
    return uri;
#endif
    if (uri.find("+ssh://") == std::string::npos || uri.find("no_tty") != std::string::npos) return uri;
    return uri + (uri.find('?') == std::string::npos ? "?no_tty=1" : "&no_tty=1");
}

// [user@]host[:port] of a qemu+ssh:// URI, for tunnels to its VMs' displays
static std::string sshTarget(const std::string& uri)
{
    const QUrl url(QString::fromStdString(uri));
    if (url.host().isEmpty()) return {};
    QString target = url.userName().isEmpty() ? url.host() : url.userName() + QLatin1Char('@') + url.host();
    if (url.port() > 0) target += QStringLiteral(":%1").arg(url.port());
    return target.toStdString();
}

static LibvirtVms::State stateOf(virDomainPtr d)
{
    int state = VIR_DOMAIN_NOSTATE, reason = 0;
    if (virDomainGetState(d, &state, &reason, 0) < 0) return LibvirtVms::State::Other;
    switch (state) {
    case VIR_DOMAIN_RUNNING:
    case VIR_DOMAIN_BLOCKED: return LibvirtVms::State::Running;
    case VIR_DOMAIN_PAUSED: return LibvirtVms::State::Paused;
    case VIR_DOMAIN_SHUTDOWN: return LibvirtVms::State::ShuttingDown;
    case VIR_DOMAIN_SHUTOFF:
        return virDomainHasManagedSaveImage(d, 0) > 0 ? LibvirtVms::State::Saved : LibvirtVms::State::ShutOff;
    case VIR_DOMAIN_CRASHED: return LibvirtVms::State::Crashed;
    case VIR_DOMAIN_PMSUSPENDED: return LibvirtVms::State::Suspended;
    default: return LibvirtVms::State::Other;
    }
}

// callbacks on the UI thread, open() and control() elsewhere
// shortcut: a remote host's listing and CPU samples still block the UI thread
struct LibvirtVms::Worker {
    LibvirtVms* owner;
    std::string uri;
    std::string tunnel;    // remote hosts: displays through SSH
    std::string tunnelKey; // the URI's keyfile, for the tunnel too
    std::string error;
    bool opening = false;
    int timer = -1;
    int statsTimer = -1;
    struct Sample {
        unsigned long long cpuTime; // ns
        std::chrono::steady_clock::time_point at;
    };
    std::map<std::string, Sample> last;
    virConnectPtr conn = nullptr;
    int lifecycle = -1;
    bool lost = false;     // libvirt closed the connection: cleaned up and reopened by the timer
    bool readOnly = false;

    void publish(std::vector<Vm> vms)
    {
        owner->vms_ = std::move(vms);
        emit owner->changed();
    }

    static void onClosed(virConnectPtr, int, void* self)
    {
        auto* w = static_cast<Worker*>(self);
        w->lost = true; // not closed from inside its own close callback
        w->publish({});
    }

    static int onLifecycle(virConnectPtr, virDomainPtr, int, int, void* self)
    {
        static_cast<Worker*>(self)->refresh();
        return 0;
    }

    void connect()
    {
        if (lost) disconnect();
        if (conn || opening) return;
        opening = true;
        // off the UI thread: ssh may take seconds, or its timeout
        std::thread([uri = openUri(uri), self = QPointer<LibvirtVms>(owner)] {
            virConnectPtr c = virConnectOpen(uri.c_str()); // read-write: display passwords and control, as virt-manager
            const bool readOnly = !c;
            if (!c) c = virConnectOpenReadOnly(uri.c_str());
            const std::string error = c ? std::string() : std::string(virGetLastErrorMessage());
            QMetaObject::invokeMethod(qApp, [self, c, readOnly, error] {
                if (self) self->worker_->opened(c, readOnly, error);
                else if (c) virConnectClose(c);
            });
        }).detach();
        publish(owner->vms_); // "connecting"
    }

    void opened(virConnectPtr c, bool ro, const std::string& why)
    {
        opening = false;
        error = why;
        conn = c;
        readOnly = ro;
        if (!conn) return publish({});
        virConnectRegisterCloseCallback(conn, onClosed, this, nullptr);
        lifecycle = virConnectDomainEventRegisterAny(conn, nullptr, VIR_DOMAIN_EVENT_ID_LIFECYCLE,
                                                     VIR_DOMAIN_EVENT_CALLBACK(onLifecycle), this, nullptr);
        refresh();
    }

    void disconnect()
    {
        if (!conn) return;
        if (lifecycle >= 0) virConnectDomainEventDeregisterAny(conn, lifecycle);
        virConnectUnregisterCloseCallback(conn, onClosed);
        virConnectClose(conn);
        conn = nullptr;
        lifecycle = -1;
        lost = false;
    }

    // CPU time delta over wall time times vCPUs
    void sample()
    {
        if (!conn || lost) return;
        virDomainPtr* domains = nullptr;
        const int n = virConnectListAllDomains(conn, &domains, VIR_CONNECT_LIST_DOMAINS_ACTIVE);
        if (n < 0) return;
        const auto now = std::chrono::steady_clock::now();
        std::map<std::string, Sample> seen;
        for (int i = 0; i < n; ++i) {
            char uuid[VIR_UUID_STRING_BUFLEN] = {};
            virDomainInfo info{};
            if (virDomainGetUUIDString(domains[i], uuid) == 0 && virDomainGetInfo(domains[i], &info) == 0 && info.nrVirtCpu > 0) {
                if (const auto it = last.find(uuid); it != last.end() && info.cpuTime >= it->second.cpuTime) {
                    const double wall = std::chrono::duration<double, std::nano>(now - it->second.at).count();
                    const double used = double(info.cpuTime - it->second.cpuTime);
                    std::deque<float>& history = owner->cpu_[uuid];
                    history.push_back(wall > 0 ? float(std::clamp(100.0 * used / (wall * info.nrVirtCpu), 0.0, 100.0)) : 0.f);
                    if (history.size() > 40) history.pop_front();
                }
                seen[uuid] = {info.cpuTime, now};
            }
            virDomainFree(domains[i]);
        }
        std::free(domains);
        last = std::move(seen);
        std::erase_if(owner->cpu_, [this](const auto& entry) { return !last.count(entry.first); });
        emit owner->statsChanged();
    }

    void refresh()
    {
        virDomainPtr* domains = nullptr;
        const int n = virConnectListAllDomains(conn, &domains, 0);
        if (n < 0) return;
        std::vector<Vm> vms;
        for (int i = 0; i < n; ++i) {
            Vm vm;
            char uuid[VIR_UUID_STRING_BUFLEN] = {};
            virDomainGetUUIDString(domains[i], uuid);
            vm.uuid = uuid;
            if (const char* name = virDomainGetName(domains[i])) vm.name = name;
            vm.state = stateOf(domains[i]);
            if (virDomainIsActive(domains[i]) == 1) {
                char* xml = virDomainGetXMLDesc(domains[i], VIR_DOMAIN_XML_SECURE);
                if (!xml) xml = virDomainGetXMLDesc(domains[i], 0); // read-only connection: no passwords
                if (xml) {
                    vm.display = connectionFromDomainXml(QString::fromUtf8(xml));
                    std::free(xml);
                }
            }
            if (vm.display && !tunnel.empty()) vm.display->options["tunnel"] = tunnel;
            if (vm.display && !tunnelKey.empty()) vm.display->options["tunnel.keyFile"] = tunnelKey;
            virDomainFree(domains[i]);
            vms.push_back(std::move(vm));
        }
        std::free(domains);
        std::sort(vms.begin(), vms.end(), [](const Vm& a, const Vm& b) {
            return QString::fromStdString(a.name).compare(QString::fromStdString(b.name), Qt::CaseInsensitive) < 0;
        });
        publish(std::move(vms));
    }
};

LibvirtVms::LibvirtVms(QString name, std::string uri, QObject* parent)
    : QObject(parent), worker_(std::make_unique<Worker>()), name_(std::move(name))
{
    static const bool once = [] {
        virSetErrorFunc(nullptr, [](void*, virErrorPtr) {}); // not on stderr every few seconds while libvirt is down
        return virEventRegisterDefaultImpl() == 0;
    }();
    (void)once;
    Worker* w = worker_.get();
    w->owner = this;
    w->uri = std::move(uri);
    w->tunnel = sshTarget(w->uri);
    w->tunnelKey = QUrlQuery(QUrl(QString::fromStdString(w->uri))).queryItemValue(QStringLiteral("keyfile"), QUrl::FullyDecoded).toStdString();
    w->timer = virEventAddTimeout(5000, [](int, void* self) { static_cast<Worker*>(self)->connect(); }, w, nullptr);
    w->statsTimer = virEventAddTimeout(2000, [](int, void* self) { static_cast<Worker*>(self)->sample(); }, w, nullptr);
    w->connect();
}

LibvirtVms::~LibvirtVms()
{
    worker_->disconnect(); // an open still under way closes its connection when it finds us gone
    virEventRemoveTimeout(worker_->timer);
    virEventRemoveTimeout(worker_->statsTimer);
}

std::vector<Connection> LibvirtVms::connections() const
{
    std::vector<Connection> displays;
    for (const Vm& vm : vms_)
        if (vm.display) displays.push_back(*vm.display);
    return displays;
}

const std::deque<float>* LibvirtVms::cpu(const std::string& uuid) const
{
    const auto it = cpu_.find(uuid);
    return it == cpu_.end() ? nullptr : &it->second;
}

std::unique_ptr<rdc::TerminalSession> LibvirtVms::serialConsole(const std::string& uuid, rdc::TerminalListener& listener) const
{
    if (!connected()) return nullptr;
    return std::make_unique<LibvirtConsoleSession>(listener, worker_->conn, uuid);
}

bool LibvirtVms::connected() const { return worker_->conn && !worker_->lost; }
bool LibvirtVms::readOnly() const { return worker_->readOnly; }
bool LibvirtVms::connecting() const { return worker_->opening; }
QString LibvirtVms::uri() const { return QString::fromStdString(worker_->uri); }
bool LibvirtVms::remote() const { return !worker_->tunnel.empty(); }
QString LibvirtVms::lastError() const { return QString::fromStdString(worker_->error); }

bool LibvirtVms::allowed(Action action, State state)
{
    switch (action) {
    case Action::Start: return state == State::ShutOff || state == State::Saved || state == State::Crashed;
    case Action::Shutdown:
    case Action::Reboot:
    case Action::Pause: return state == State::Running;
    case Action::Reset:
    case Action::Save: return state == State::Running || state == State::Paused;
    case Action::Resume: return state == State::Paused || state == State::Suspended;
    case Action::ForceOff:
        return state == State::Running || state == State::Paused || state == State::ShuttingDown || state == State::Crashed ||
               state == State::Suspended;
    }
    return false;
}

// ISO files in the host's active storage pools
static QStringList isoVolumes(virConnectPtr conn)
{
    QStringList isos;
    virStoragePoolPtr* pools = nullptr;
    const int n = virConnectListAllStoragePools(conn, &pools, VIR_CONNECT_LIST_STORAGE_POOLS_ACTIVE);
    for (int i = 0; i < n; ++i) {
        virStoragePoolRefresh(pools[i], 0); // files added since
        virStorageVolPtr* volumes = nullptr;
        const int m = virStoragePoolListAllVolumes(pools[i], &volumes, 0);
        for (int j = 0; j < m; ++j) {
            if (char* path = virStorageVolGetPath(volumes[j])) {
                if (const QString p = QString::fromUtf8(path); p.endsWith(QLatin1String(".iso"), Qt::CaseInsensitive)) isos << p;
                std::free(path);
            }
            virStorageVolFree(volumes[j]);
        }
        std::free(volumes);
        virStoragePoolFree(pools[i]);
    }
    std::free(pools);
    isos.sort(Qt::CaseInsensitive);
    return isos;
}

// shortcut: on the UI thread like the listing, a remote host's round trips block it briefly
std::optional<VmSettings> LibvirtVms::settings(const std::string& uuid, QString& error) const
{
    virDomainPtr dom = connected() ? virDomainLookupByUUIDString(worker_->conn, uuid.c_str()) : nullptr;
    char* xml = dom ? virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE | VIR_DOMAIN_XML_SECURE) : nullptr;
    virNodeInfo node{};
    if (!xml || virNodeGetInfo(worker_->conn, &node) < 0) {
        error = dom ? QString::fromUtf8(virGetLastErrorMessage()) : tr("The virtual machine is gone, or libvirt is not reachable.");
        std::free(xml);
        if (dom) virDomainFree(dom);
        return std::nullopt;
    }
    VmSettings s = domainSettings(QString::fromUtf8(xml));
    std::free(xml);
    s.maxCpus = int(node.cpus);
    s.maxMemoryMiB = int(node.memory / 1024);
    s.isos = isoVolumes(worker_->conn);
    s.localFiles = !remote();
    s.running = virDomainIsActive(dom) == 1;
    virDomainFree(dom);
    return s;
}

QString LibvirtVms::apply(const std::string& uuid, const VmSettings& was, const VmSettings& now)
{
    virDomainPtr dom = connected() ? virDomainLookupByUUIDString(worker_->conn, uuid.c_str()) : nullptr;
    if (!dom) return tr("The virtual machine is gone, or libvirt is not reachable.");
    auto failed = [dom] {
        const QString error = QString::fromUtf8(virGetLastErrorMessage());
        virDomainFree(dom);
        return error;
    };
    if (now.cpus != was.cpus || now.memoryMiB != was.memoryMiB || now.cdFirst != was.cdFirst) {
        char* xml = virDomainGetXMLDesc(dom, VIR_DOMAIN_XML_INACTIVE | VIR_DOMAIN_XML_SECURE); // SECURE: keeps its display password
        if (!xml) return failed();
        const QByteArray changed = withSettings(QString::fromUtf8(xml), now).toUtf8();
        std::free(xml);
        virDomainPtr defined = changed.isEmpty() ? nullptr : virDomainDefineXML(worker_->conn, changed.constData());
        if (!defined) return failed();
        virDomainFree(defined);
    }
    if (now.iso != was.iso) {
        // each from its own definition: the running one may boot in another order until its next start
        std::vector<unsigned> targets{VIR_DOMAIN_AFFECT_CONFIG};
        if (virDomainIsActive(dom) == 1) targets.push_back(unsigned(VIR_DOMAIN_AFFECT_LIVE) | VIR_DOMAIN_DEVICE_MODIFY_FORCE);
        for (const unsigned flags : targets) {
            char* xml = virDomainGetXMLDesc(dom, flags & VIR_DOMAIN_AFFECT_LIVE ? 0 : VIR_DOMAIN_XML_INACTIVE);
            if (!xml) return failed();
            const QByteArray disk = cdromXml(QString::fromUtf8(xml), now.iso).toUtf8();
            std::free(xml);
            if (disk.isEmpty() || virDomainUpdateDeviceFlags(dom, disk.constData(), flags) < 0) return failed();
        }
    }
    virDomainFree(dom);
    return {};
}

std::vector<VmSnapshot> LibvirtVms::snapshots(const std::string& uuid, QString& error) const
{
    virDomainPtr dom = connected() ? virDomainLookupByUUIDString(worker_->conn, uuid.c_str()) : nullptr;
    virDomainSnapshotPtr* list = nullptr;
    const int n = dom ? virDomainListAllSnapshots(dom, &list, 0) : -1;
    if (n < 0) error = dom ? QString::fromUtf8(virGetLastErrorMessage()) : tr("The virtual machine is gone, or libvirt is not reachable.");
    std::vector<VmSnapshot> snapshots;
    for (int i = 0; i < n; ++i) {
        if (char* xml = virDomainSnapshotGetXMLDesc(list[i], 0)) {
            VmSnapshot s = snapshotFromXml(QString::fromUtf8(xml));
            s.current = virDomainSnapshotIsCurrent(list[i], 0) == 1;
            snapshots.push_back(std::move(s));
            std::free(xml);
        }
        virDomainSnapshotFree(list[i]);
    }
    std::free(list);
    if (dom) virDomainFree(dom);
    std::sort(snapshots.begin(), snapshots.end(), [](const VmSnapshot& a, const VmSnapshot& b) { return a.created < b.created; });
    return snapshots;
}

void LibvirtVms::snapshot(const std::string& uuid, SnapshotOp op, const QString& name, std::function<void(const QString&)> done)
{
    virDomainPtr dom = connected() ? virDomainLookupByUUIDString(worker_->conn, uuid.c_str()) : nullptr;
    if (!dom) return done(tr("The virtual machine is gone, or libvirt is not reachable."));
    std::thread([dom, op, name, done = std::move(done)] {
        int rc = -1;
        if (op == SnapshotOp::Take) {
            const QByteArray xml = QStringLiteral("<domainsnapshot><name>%1</name></domainsnapshot>").arg(name.toHtmlEscaped()).toUtf8();
            virDomainSnapshotPtr s = virDomainSnapshotCreateXML(dom, xml.constData(), 0); // libvirt's default: with its memory while it runs
            rc = s ? 0 : -1;
            if (s) virDomainSnapshotFree(s);
        } else if (virDomainSnapshotPtr s = virDomainSnapshotLookupByName(dom, name.toUtf8().constData(), 0)) {
            rc = op == SnapshotOp::Revert ? virDomainRevertToSnapshot(s, 0) : virDomainSnapshotDelete(s, 0);
            virDomainSnapshotFree(s);
        }
        const QString error = rc < 0 ? QString::fromUtf8(virGetLastErrorMessage()) : QString();
        virDomainFree(dom);
        QMetaObject::invokeMethod(qApp, [done, error] { done(error); });
    }).detach();
}

void LibvirtVms::control(const std::string& uuid, Action action)
{
    virDomainPtr dom = connected() ? virDomainLookupByUUIDString(worker_->conn, uuid.c_str()) : nullptr;
    if (!dom) {
        emit failed(QString::fromStdString(uuid), QString(), tr("The virtual machine is gone, or libvirt is not reachable."));
        return;
    }
    const QString name = QString::fromUtf8(virDomainGetName(dom));
    // off the UI thread (a save writes memory to disk); libvirt connections are thread-safe
    std::thread([dom, action, name, id = QString::fromStdString(uuid), self = QPointer<LibvirtVms>(this)] {
        int rc = -1;
        switch (action) {
        case Action::Start: rc = virDomainCreate(dom); break;
        case Action::Shutdown: rc = virDomainShutdown(dom); break;
        case Action::Reboot: rc = virDomainReboot(dom, 0); break;
        case Action::Reset: rc = virDomainReset(dom, 0); break;
        case Action::ForceOff: rc = virDomainDestroy(dom); break;
        case Action::Pause: rc = virDomainSuspend(dom); break;
        case Action::Resume: {
            int state = 0, reason = 0;
            virDomainGetState(dom, &state, &reason, 0);
            rc = state == VIR_DOMAIN_PMSUSPENDED ? virDomainPMWakeup(dom, 0) : virDomainResume(dom);
            break;
        }
        case Action::Save: rc = virDomainManagedSave(dom, 0); break;
        }
        const QString error = rc < 0 ? QString::fromUtf8(virGetLastErrorMessage()) : QString(); // this thread's error
        virDomainFree(dom);
        if (!error.isEmpty())
            QMetaObject::invokeMethod(qApp, [self, id, name, error] {
                if (self) emit self->failed(id, name, error);
            });
    }).detach(); // not joined on quit: libvirt finishes the operation without us
}
