#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>
#include <string>
#include <vector>

// Hyper-V through one PowerShell over SSH: its scripts and answers as plain data; no network (see HyperVHost).
namespace hyperv {

enum class State { Running, Off, Saved, Paused, Starting, Stopping, Saving, Pausing, Resuming, Resetting, Other };
enum class Action { Start, Resume, Pause, Shutdown, Reboot, Save, TurnOff, Reset };

struct Vm {
    std::string id, name; // id: its GUID
    State state = State::Other;
    int cpu = 0;          // % of its vCPUs
    long long memory = 0; // assigned, bytes
    long long uptime = 0; // seconds
    int generation = 0;
    std::string status, ip;
};

struct Listing {
    std::vector<Vm> vms;   // sorted by name
    QStringList errors;    // of actions that finished since the last listing
    bool noModule = false; // Hyper-V's PowerShell module is missing
};

// The remote command: reads base64 scripts, one per line; each answer ends with a line answerEnd() recognizes.
std::string loopCommand();
QByteArray request(const QString& script);
// "<<portal-mux ok>>" or "<<portal-mux error>>" (the answer is then the message)
std::optional<bool> answerEnd(const QByteArray& line);

QString listScript();
QString actionScript(Action action, const std::string& id); // runs as a job: errors come with the next listing
QString installModuleScript();
std::optional<Listing> parseListing(const QByteArray& json, QString* error);

bool allowed(Action action, State state);
State parseState(const QString& text);

} // namespace hyperv
