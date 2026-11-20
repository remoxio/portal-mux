#pragma once

#include "app/VmSettings.h"
#include "core/Connection.h"
#include "core/Session.h"

#include <QObject>

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// One libvirt host's VMs, kept current by its events; reconnects every 5 s
class LibvirtVms : public QObject {
    Q_OBJECT

public:
    enum class State { Running, Paused, ShuttingDown, ShutOff, Saved, Crashed, Suspended, Other };
    enum class Action { Start, Shutdown, Reboot, Reset, ForceOff, Pause, Resume, Save };
    struct Vm {
        std::string uuid, name;
        State state = State::Other;
        std::optional<rdc::Connection> display;
    };

    LibvirtVms(QString name, std::string uri, QObject* parent = nullptr);
    ~LibvirtVms() override;

    const std::vector<Vm>& vms() const { return vms_; }
    std::vector<rdc::Connection> connections() const;
    bool connected() const;
    bool readOnly() const; // no permission to change anything: listed only
    bool connecting() const;
    QString uri() const;
    const QString& name() const { return name_; }
    bool remote() const;
    QString lastError() const; // why it isn't connected
    // Guest CPU use in % of its vCPUs, every 2 s, oldest first; null if not running
    const std::deque<float>* cpu(const std::string& uuid) const;

    static bool allowed(Action action, State state);
    std::unique_ptr<rdc::TerminalSession> serialConsole(const std::string& uuid, rdc::TerminalListener& listener) const; // null: unreachable
    // Asynchronous; new state via changed(), errors via failed()
    void control(const std::string& uuid, Action action);
    std::optional<VmSettings> settings(const std::string& uuid, QString& error) const;
    QString apply(const std::string& uuid, const VmSettings& was, const VmSettings& now); // the error, empty if done

signals:
    void changed();
    void statsChanged();
    void failed(const QString& uuid, const QString& name, const QString& message);

private:
    struct Worker;
    std::unique_ptr<Worker> worker_;
    QString name_;
    std::vector<Vm> vms_;
    std::map<std::string, std::deque<float>> cpu_;
};
