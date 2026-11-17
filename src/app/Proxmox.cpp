#include "app/Proxmox.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace proxmox {

static QString tr(const char* text) { return QCoreApplication::translate("Proxmox", text); }

bool isApiToken(const std::string& user) { return user.find('!') != std::string::npos; }

bool parseAddress(const QString& input, std::string& host, int& port)
{
    const QString text = input.trimmed();
    port = 8006;
    QString h = text;
    if (text.startsWith(QLatin1Char('['))) { // [v6]:port
        const qsizetype end = text.indexOf(QLatin1Char(']'));
        if (end < 0) return false;
        h = text.mid(1, end - 1);
        if (text.mid(end + 1).startsWith(QLatin1Char(':'))) port = text.mid(end + 2).toInt();
    } else if (text.count(QLatin1Char(':')) == 1) {
        h = text.section(QLatin1Char(':'), 0, 0);
        port = text.section(QLatin1Char(':'), 1).toInt();
    }
    host = h.trimmed().toStdString();
    return !host.empty() && port > 0 && port < 65536;
}

static QJsonValue data(const QByteArray& json, QString* error)
{
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = tr("Proxmox sent an answer that isn't JSON.");
        return {};
    }
    return doc.object().value(QLatin1String("data"));
}

std::optional<Resources> parseResources(const QByteArray& json, QString* error)
{
    const QJsonValue value = data(json, error);
    if (!value.isArray()) {
        if (error && error->isEmpty()) *error = tr("Proxmox sent no resource list.");
        return std::nullopt;
    }
    Resources r;
    for (const QJsonValue& item : value.toArray()) {
        const QJsonObject o = item.toObject();
        const QString type = o.value(QLatin1String("type")).toString();
        if (type == QLatin1String("node")) {
            r.nodes.push_back({o.value(QLatin1String("node")).toString().toStdString(),
                               o.value(QLatin1String("status")).toString() == QLatin1String("online"), o.value(QLatin1String("cpu")).toDouble(),
                               o.value(QLatin1String("maxcpu")).toInt(), o.value(QLatin1String("mem")).toInteger(),
                               o.value(QLatin1String("maxmem")).toInteger(), o.value(QLatin1String("uptime")).toInteger()});
        } else if (type == QLatin1String("qemu") || type == QLatin1String("lxc")) {
            Guest g;
            g.node = o.value(QLatin1String("node")).toString().toStdString();
            g.type = type.toStdString();
            g.vmid = o.value(QLatin1String("vmid")).toInt();
            g.name = o.value(QLatin1String("name")).toString().toStdString();
            g.status = o.value(QLatin1String("status")).toString().toStdString();
            g.cpu = o.value(QLatin1String("cpu")).toDouble();
            g.mem = o.value(QLatin1String("mem")).toInteger();
            g.maxmem = o.value(QLatin1String("maxmem")).toInteger();
            g.isTemplate = o.value(QLatin1String("template")).toInt() == 1;
            if (g.name.empty()) g.name = std::to_string(g.vmid);
            r.guests.push_back(std::move(g));
        }
    }
    std::sort(r.nodes.begin(), r.nodes.end(), [](const Node& a, const Node& b) { return a.name < b.name; });
    std::sort(r.guests.begin(), r.guests.end(), [](const Guest& a, const Guest& b) {
        return a.node != b.node ? a.node < b.node : a.vmid < b.vmid;
    });
    return r;
}

QByteArray spiceCa(const QByteArray& json)
{
    QString ca = data(json, nullptr).toObject().value(QLatin1String("ca")).toString();
    ca.replace(QLatin1String("\\n"), QLatin1String("\n"));
    return ca.toUtf8();
}

std::optional<rdc::Connection> spiceConnection(const QByteArray& json, const std::string& name, const std::string& caFile,
                                               QString* error)
{
    const QJsonObject o = data(json, error).toObject();
    const QString host = o.value(QLatin1String("host")).toString();
    if (host.isEmpty()) {
        if (error && error->isEmpty()) *error = tr("Proxmox sent no SPICE details.");
        return std::nullopt;
    }
    rdc::Connection c;
    c.protocol = rdc::Protocol::SPICE;
    c.name = name;
    c.host = host.toStdString(); // "pvespiceproxy:…", resolved by the proxy
    c.port = o.value(QLatin1String("port")).toInt();
    c.password = o.value(QLatin1String("password")).toString().toStdString(); // one-time ticket
    const QJsonValue tls = o.value(QLatin1String("tls-port"));
    if (const int port = tls.isString() ? tls.toString().toInt() : tls.toInt(); port > 0) c.options["tlsPort"] = std::to_string(port);
    if (const QString proxy = o.value(QLatin1String("proxy")).toString(); !proxy.isEmpty()) c.options["proxy"] = proxy.toStdString();
    if (const QString subject = o.value(QLatin1String("host-subject")).toString(); !subject.isEmpty())
        c.options["hostSubject"] = subject.toStdString();
    if (!caFile.empty() && !o.value(QLatin1String("ca")).toString().isEmpty()) c.options["caFile"] = caFile;
    return c;
}

std::optional<VncTicket> parseVncProxy(const QByteArray& json, QString* error)
{
    const QJsonObject o = data(json, error).toObject();
    VncTicket t;
    const QJsonValue port = o.value(QLatin1String("port")); // number or string
    t.port = port.isString() ? port.toString().toInt() : port.toInt();
    t.ticket = o.value(QLatin1String("ticket")).toString().toStdString();
    t.password = o.value(QLatin1String("password")).toString().toStdString();
    t.user = o.value(QLatin1String("user")).toString().toStdString();
    if (t.password.empty()) t.password = t.ticket;
    if (t.port <= 0 || t.ticket.empty()) {
        if (error && error->isEmpty()) *error = tr("Proxmox sent no VNC ticket.");
        return std::nullopt;
    }
    return t;
}

QByteArray termInput(const QByteArray& bytes) { return "0:" + QByteArray::number(bytes.size()) + ':' + bytes; }

QByteArray termResize(int cols, int rows) { return "1:" + QByteArray::number(cols) + ':' + QByteArray::number(rows) + ':'; }

bool usesSpice(const QByteArray& statusJson)
{
    const QJsonValue spice = data(statusJson, nullptr).toObject().value(QLatin1String("spice"));
    return spice.toBool() || spice.toInt() == 1;
}

QString errorText(const QByteArray& json)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    QString text = o.value(QLatin1String("message")).toString().trimmed();
    const QJsonObject errors = o.value(QLatin1String("errors")).toObject();
    for (auto it = errors.begin(); it != errors.end(); ++it)
        text += (text.isEmpty() ? QString() : QStringLiteral("; ")) + it.key() + QStringLiteral(": ") + it.value().toString().trimmed();
    return text;
}

} // namespace proxmox
