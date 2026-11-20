#include "app/HyperV.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace hyperv {

std::string loopCommand()
{
    // $input, not [Console]::In: Windows PowerShell's host reads the redirected stdin too
    static const QString loop = QStringLiteral(R"ps(
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$ProgressPreference = 'SilentlyContinue'
$ErrorActionPreference = 'Stop'
foreach ($line in $input) {
    $status = 'ok'
    try { $out = & ([ScriptBlock]::Create([Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($line)))) | Out-String -Width 65536 }
    catch { $status = 'error'; $out = $_.Exception.Message }
    [Console]::Out.Write("$out".Trim() + "`n<<portal-mux $status>>`n")
    [Console]::Out.Flush()
}
)ps");
    const QByteArray utf16(reinterpret_cast<const char*>(loop.utf16()), loop.size() * 2); // -EncodedCommand: UTF-16LE
    return "powershell -NoLogo -NoProfile -NonInteractive -EncodedCommand " + utf16.toBase64().toStdString();
}

QByteArray request(const QString& script) { return script.toUtf8().toBase64() + '\n'; }

std::optional<bool> answerEnd(const QByteArray& line)
{
    if (line == "<<portal-mux ok>>") return true;
    if (line == "<<portal-mux error>>") return false;
    return std::nullopt;
}

QString listScript()
{
    return QStringLiteral(R"ps(
if (-not (Get-Module Hyper-V)) { Import-Module Hyper-V -ErrorAction SilentlyContinue }
if (-not (Get-Module Hyper-V)) { '{"noModule":true}'; return }
$errors = @(foreach ($job in @(Get-Job | Where-Object State -in 'Completed', 'Failed', 'Stopped')) {
    $e = $null
    Receive-Job $job -ErrorAction SilentlyContinue -ErrorVariable e | Out-Null
    if ($e) { $e | ForEach-Object { $_.Exception.Message } }
    elseif ($job.State -eq 'Failed') { $job.ChildJobs | ForEach-Object { $_.JobStateInfo.Reason.Message } }
    Remove-Job $job -Force
})
$vms = @(Get-VM | Sort-Object Name | ForEach-Object {
    [ordered]@{ id = $_.Id.Guid; name = $_.Name; state = "$($_.State)"; cpu = $_.CPUUsage; memory = $_.MemoryAssigned
                uptime = [long]$_.Uptime.TotalSeconds; generation = $_.Generation; status = "$($_.Status)"
                ip = "$(@($_.NetworkAdapters.IPAddresses | Where-Object { $_ -like '*.*' })[0])" } })
ConvertTo-Json -Compress -Depth 3 -InputObject @{ vms = $vms; errors = $errors }
)ps");
}

QString actionScript(Action action, const std::string& id)
{
    static const QRegularExpression guid(QStringLiteral("^[0-9A-Fa-f]{8}(-[0-9A-Fa-f]{4}){3}-[0-9A-Fa-f]{12}$"));
    const QString vm = QString::fromStdString(id);
    if (!guid.match(vm).hasMatch()) return {};
    const char* command = "";
    switch (action) {
    case Action::Start: command = "Start-VM -VM $vm"; break;
    case Action::Resume: command = "Resume-VM -VM $vm"; break;
    case Action::Pause: command = "Suspend-VM -VM $vm"; break;
    case Action::Shutdown: command = "Stop-VM -VM $vm -Force"; break;
    case Action::Reboot: command = "Restart-VM -VM $vm -Type Reboot -Force"; break;
    case Action::Save: command = "Save-VM -VM $vm"; break;
    case Action::TurnOff: command = "Stop-VM -VM $vm -TurnOff -Force"; break;
    case Action::Reset: command = "Restart-VM -VM $vm -Type Reset -Force"; break;
    }
    return QStringLiteral("$vm = Get-VM -Id '%1'\n%2 -Confirm:$false -AsJob | Out-Null").arg(vm, QLatin1String(command));
}

QString installModuleScript()
{
    return QStringLiteral(R"ps(
if (Get-Command Install-WindowsFeature -ErrorAction SilentlyContinue) {
    if (-not (Install-WindowsFeature Hyper-V-PowerShell).Success) { throw 'Install-WindowsFeature Hyper-V-PowerShell failed.' }
} else {
    Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-Management-PowerShell -All -NoRestart | Out-Null
}
Import-Module Hyper-V
)ps");
}

State parseState(const QString& text)
{
    QString s = text;
    if (s.endsWith(QLatin1String("Critical"))) s.chop(8);
    if (s == QLatin1String("Running")) return State::Running;
    if (s == QLatin1String("Off")) return State::Off;
    if (s == QLatin1String("Saved") || s == QLatin1String("FastSaved")) return State::Saved;
    if (s == QLatin1String("Paused")) return State::Paused;
    if (s == QLatin1String("Starting")) return State::Starting;
    if (s == QLatin1String("Stopping")) return State::Stopping;
    if (s == QLatin1String("Saving") || s == QLatin1String("FastSaving")) return State::Saving;
    if (s == QLatin1String("Pausing")) return State::Pausing;
    if (s == QLatin1String("Resuming")) return State::Resuming;
    if (s == QLatin1String("Reset")) return State::Resetting;
    return State::Other;
}

std::optional<Listing> parseListing(const QByteArray& json, QString* error)
{
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = QStringLiteral("Not a listing: ") + QString::fromUtf8(json.left(200));
        return std::nullopt;
    }
    const QJsonObject root = doc.object();
    Listing listing;
    listing.noModule = root.value(QLatin1String("noModule")).toBool();
    for (const QJsonValue& v : root.value(QLatin1String("vms")).toArray()) {
        const QJsonObject o = v.toObject();
        Vm vm;
        vm.id = o.value(QLatin1String("id")).toString().toStdString();
        vm.name = o.value(QLatin1String("name")).toString().toStdString();
        vm.state = parseState(o.value(QLatin1String("state")).toString());
        vm.cpu = o.value(QLatin1String("cpu")).toInt();
        vm.memory = qint64(o.value(QLatin1String("memory")).toDouble());
        vm.uptime = qint64(o.value(QLatin1String("uptime")).toDouble());
        vm.generation = o.value(QLatin1String("generation")).toInt();
        vm.status = o.value(QLatin1String("status")).toString().toStdString();
        vm.ip = o.value(QLatin1String("ip")).toString().toStdString();
        if (!vm.id.empty()) listing.vms.push_back(vm);
    }
    for (const QJsonValue& v : root.value(QLatin1String("errors")).toArray())
        if (!v.toString().isEmpty()) listing.errors << v.toString();
    return listing;
}

bool allowed(Action action, State state)
{
    switch (action) {
    case Action::Start: return state == State::Off || state == State::Saved;
    case Action::Resume: return state == State::Paused;
    case Action::Pause:
    case Action::Shutdown:
    case Action::Reboot:
    case Action::Reset: return state == State::Running;
    case Action::Save: return state == State::Running || state == State::Paused;
    case Action::TurnOff: return state != State::Off && state != State::Saved && state != State::Other;
    }
    return false;
}

} // namespace hyperv
