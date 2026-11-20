#pragma once

#include "app/HyperV.h"
#include "app/Prompts.h"
#include "app/SshServers.h"
#include "ssh/SshConnection.h"

#include <QObject>

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

class QWidget;

// One Hyper-V host over SSH: a PowerShell kept open, VMs listed every 2 s; reconnects after network errors
class HyperVHost : public QObject, private rdc::SessionListener {
    Q_OBJECT

public:
    enum class Status { Connecting, Connected, Unreachable, LoginFailed, NoModule, Refused }; // Refused: Hyper-V's answer, e.g. no permission

    HyperVHost(SshServer server, QString password, std::string knownHostsFile, QWidget* dialogParent, QObject* parent = nullptr);
    ~HyperVHost() override;

    const SshServer& server() const { return server_; }
    const std::vector<hyperv::Vm>& vms() const { return vms_; }
    Status status() const { return status_; }
    const QString& lastError() const { return error_; }
    const std::deque<float>* cpu(const std::string& id) const; // %, every 2 s, oldest first; null if not running

    void control(const std::string& id, hyperv::Action action); // errors via failed()
    void installModule();
    void reconnect(); // after a refused login or a declined prompt

signals:
    void changed();      // the list or the status
    void statsChanged(); // CPU use only
    void failed(const QString& message);

private:
    void run(std::stop_token stop);
    bool exchange(void* channel, const QString& script, QByteArray& answer, bool& ok, std::stop_token stop);
    void post(std::function<void()> f);
    void setStatus(Status status, const QString& error);
    void apply(hyperv::Listing listing);

    void onStateChanged(rdc::SessionState) override {}
    void onError(std::string) override {}
    bool askTrust(const std::string& title, const std::string& details, bool* always) override;
    bool askCredentials(const std::string& prompt, rdc::Credentials& creds) override;

    SshServer server_;
    rdc::SshConnection::Params params_;
    QWidget* dialogParent_;
    PromptBridge prompts_;
    std::mutex mutex_; // guards the members below it, shared with the worker
    std::condition_variable wake_;
    std::deque<QString> scripts_;
    bool retry_ = false;
    rdc::SshConnection* ssh_ = nullptr;
    std::jthread worker_;

    std::vector<hyperv::Vm> vms_;
    std::map<std::string, std::deque<float>> cpu_;
    Status status_ = Status::Connecting;
    QString error_;
};
