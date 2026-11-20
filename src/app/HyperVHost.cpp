#include "app/HyperVHost.h"

#include "ssh/SshTunnel.h"

#include <libssh/libssh.h>

#include <QWidget>

#include <chrono>

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

HyperVHost::HyperVHost(SshServer server, QString password, std::string knownHostsFile, QWidget* dialogParent, QObject* parent)
    : QObject(parent), server_(std::move(server)), dialogParent_(dialogParent), prompts_(this, [] {})
{
    params_ = rdc::SshTunnel::parse(server_.address().toStdString());
    if (server_.auth == QLatin1String("publicKey")) params_.keyFile = server_.keyFile.toStdString();
    else params_.password = password.toStdString();
    params_.knownHostsFile = std::move(knownHostsFile);
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

HyperVHost::~HyperVHost()
{
    prompts_.close();
    worker_.request_stop();
    {
        std::lock_guard lock(mutex_);
        if (ssh_) ssh_->abort();
    }
    worker_.join();
}

const std::deque<float>* HyperVHost::cpu(const std::string& id) const
{
    const auto it = cpu_.find(id);
    return it == cpu_.end() ? nullptr : &it->second;
}

void HyperVHost::control(const std::string& id, hyperv::Action action)
{
    const QString script = hyperv::actionScript(action, id);
    if (script.isEmpty()) return;
    {
        std::lock_guard lock(mutex_);
        scripts_.push_back(script);
    }
    wake_.notify_all();
}

void HyperVHost::installModule()
{
    {
        std::lock_guard lock(mutex_);
        scripts_.push_back(hyperv::installModuleScript());
    }
    wake_.notify_all();
}

void HyperVHost::reconnect()
{
    {
        std::lock_guard lock(mutex_);
        retry_ = true;
    }
    wake_.notify_all();
}

void HyperVHost::post(std::function<void()> f) { QMetaObject::invokeMethod(this, std::move(f), Qt::QueuedConnection); }

void HyperVHost::setStatus(Status status, const QString& error)
{
    if (status != Status::Connected && status != Status::Refused) {
        vms_.clear();
        cpu_.clear();
    }
    status_ = status;
    error_ = error;
    emit changed();
}

void HyperVHost::apply(hyperv::Listing listing)
{
    if (listing.noModule) return setStatus(Status::NoModule, tr("Hyper-V's PowerShell module is missing on the server."));
    for (const QString& e : std::as_const(listing.errors)) emit failed(e);
    for (const hyperv::Vm& vm : listing.vms) {
        if (vm.state != hyperv::State::Running) {
            cpu_.erase(vm.id);
            continue;
        }
        std::deque<float>& history = cpu_[vm.id];
        history.push_back(float(vm.cpu));
        if (history.size() > 40) history.pop_front();
    }
    const bool same = status_ == Status::Connected && std::equal(vms_.begin(), vms_.end(), listing.vms.begin(), listing.vms.end(),
                                                                 [](const hyperv::Vm& a, const hyperv::Vm& b) {
                                                                     return a.id == b.id && a.name == b.name && a.state == b.state && a.ip == b.ip;
                                                                 });
    vms_ = std::move(listing.vms);
    if (same) emit statsChanged();
    else setStatus(Status::Connected, {});
}

bool HyperVHost::askTrust(const std::string& title, const std::string& details, bool* always)
{
    return prompts_.ask([this, t = QString::fromStdString(title), d = QString::fromStdString(details), always] {
        return prompts::trust(dialogParent_, t, d, always);
    });
}

bool HyperVHost::askCredentials(const std::string& prompt, rdc::Credentials& creds)
{
    return prompts_.ask([this, p = QString::fromStdString(prompt), &creds] { return prompts::credentials(dialogParent_, p, creds); });
}

bool HyperVHost::exchange(void* ch, const QString& script, QByteArray& answer, bool& ok, std::stop_token stop)
{
    auto channel = static_cast<ssh_channel>(ch);
    const QByteArray line = hyperv::request(script);
    if (ssh_channel_write(channel, line.constData(), uint32_t(line.size())) != int(line.size())) return false;
    answer.clear();
    QByteArray pending;
    char buffer[16384];
    const auto deadline = Clock::now() + 10min; // installing the module takes minutes; the rest seconds
    while (!stop.stop_requested() && Clock::now() < deadline) {
        while (ssh_channel_read_nonblocking(channel, buffer, sizeof buffer, 1) > 0) {} // stderr: unused, but must not fill up
        const int n = ssh_channel_read_timeout(channel, buffer, sizeof buffer, 0, 250);
        if (n == SSH_ERROR || (n <= 0 && ssh_channel_is_eof(channel))) return false;
        if (n <= 0) continue; // SSH_AGAIN: nothing within 250 ms
        pending.append(buffer, n);
        for (qsizetype eol; (eol = pending.indexOf('\n')) >= 0;) {
            QByteArray text = pending.left(eol);
            pending.remove(0, eol + 1);
            if (text.endsWith('\r')) text.chop(1);
            if (const std::optional<bool> end = hyperv::answerEnd(text)) {
                ok = *end;
                answer = answer.trimmed();
                return true;
            }
            answer += text + '\n';
        }
    }
    return false;
}

void HyperVHost::run(std::stop_token stop)
{
    std::stop_callback wakeOnStop(stop, [this] { wake_.notify_all(); });
    bool retrying = false; // keeps "not reachable" while trying again
    while (!stop.stop_requested()) {
        if (!retrying) post([this] { setStatus(Status::Connecting, {}); });
        rdc::SshConnection ssh(*this, params_);
        {
            std::lock_guard lock(mutex_);
            ssh_ = &ssh;
        }
        QString failure;
        bool network = false;
        ssh_channel channel = nullptr;
        if (!ssh.open([&] { return stop.stop_requested(); })) {
            failure = ssh.error().empty() ? tr("Cancelled.") : QString::fromStdString(ssh.error());
            for (const char* prefix : {"Cannot connect", "Cannot resolve", "SSH connection failed", "SSH connection lost"})
                network = network || failure.startsWith(QLatin1String(prefix));
        } else if (channel = ssh_channel_new(ssh.session());
                   !channel || ssh_channel_open_session(channel) != SSH_OK ||
                   ssh_channel_request_exec(channel, hyperv::loopCommand().c_str()) != SSH_OK) {
            failure = tr("Could not start PowerShell: %1").arg(QString::fromStdString(ssh.lastError()));
            network = true;
        } else {
            auto next = Clock::now();
            while (!stop.stop_requested()) {
                QString script;
                {
                    std::unique_lock lock(mutex_);
                    wake_.wait_until(lock, next, [&] { return !scripts_.empty() || stop.stop_requested(); });
                    if (!scripts_.empty()) {
                        script = scripts_.front();
                        scripts_.pop_front();
                    }
                }
                if (stop.stop_requested()) break;
                QByteArray answer;
                bool ok = false;
                if (!script.isEmpty()) {
                    if (!exchange(channel, script, answer, ok, stop)) break;
                    if (!ok) post([this, message = QString::fromUtf8(answer)] { emit failed(message); });
                    next = Clock::now(); // its new state at once
                    continue;
                }
                if (!exchange(channel, hyperv::listScript(), answer, ok, stop)) break;
                next = Clock::now() + 2s;
                QString error;
                if (!ok) post([this, message = QString::fromUtf8(answer)] { setStatus(Status::Refused, message); });
                else if (auto listing = hyperv::parseListing(answer, &error)) post([this, l = std::move(*listing)]() mutable { apply(std::move(l)); });
                else post([this, error] { setStatus(Status::Refused, error); });
            }
            failure = tr("Connection lost: %1").arg(QString::fromStdString(ssh.lastError()));
            network = true;
        }
        if (channel) {
            ssh_channel_close(channel);
            ssh_channel_free(channel);
        }
        {
            std::lock_guard lock(mutex_);
            ssh_ = nullptr;
        }
        if (stop.stop_requested()) break;
        post([this, failure, network] { setStatus(network ? Status::Unreachable : Status::LoginFailed, failure); });
        retrying = network;
        std::unique_lock lock(mutex_);
        if (network) wake_.wait_for(lock, 5s, [&] { return retry_ || stop.stop_requested(); });
        else wake_.wait(lock, [&] { return retry_ || stop.stop_requested(); }); // no retries: lockouts, fail2ban
        retry_ = false;
    }
}
