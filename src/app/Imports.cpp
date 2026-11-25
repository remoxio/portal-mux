#include "app/Imports.h"

#include <QUrl>
#include <QXmlStreamReader>

#include <map>

using rdc::Connection;
using rdc::Protocol;

namespace {

struct Section {
    QString name;
    std::vector<std::pair<QString, QString>> values; // in order, keys as written
};

std::vector<Section> iniSections(const QString& text)
{
    std::vector<Section> sections(1);
    for (QString line : text.split(QLatin1Char('\n'))) {
        line = line.trimmed();
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) sections.push_back({line.mid(1, line.size() - 2), {}});
        else if (const qsizetype eq = line.indexOf(QLatin1Char('=')); eq > 0 && !line.startsWith(QLatin1Char(';')))
            sections.back().values.emplace_back(line.left(eq).trimmed(), line.mid(eq + 1));
    }
    return sections;
}

std::map<QString, QString> keyValues(const Section& s)
{
    std::map<QString, QString> m;
    for (const auto& [key, value] : s.values) m.emplace(key.toLower(), value.trimmed());
    return m;
}

// "host", "host:port", "[v6]:port"
void setAddress(Connection& c, const QString& address)
{
    QString host = address.trimmed();
    int port = 0;
    if (host.startsWith(QLatin1Char('['))) {
        const qsizetype close = host.indexOf(QLatin1Char(']'));
        if (host.mid(close + 1).startsWith(QLatin1Char(':'))) port = host.mid(close + 2).toInt();
        host = host.mid(1, close - 1);
    } else if (host.count(QLatin1Char(':')) == 1) {
        port = host.section(QLatin1Char(':'), 1).toInt();
        host = host.section(QLatin1Char(':'), 0, 0);
    }
    c.host = host.toStdString();
    c.port = port == rdc::defaultPort(c.protocol) ? 0 : port;
}

} // namespace

std::vector<Connection> importRemmina(const QByteArray& file)
{
    for (const Section& s : iniSections(QString::fromUtf8(file))) {
        if (s.name != QLatin1String("remmina")) continue;
        auto v = keyValues(s);
        static const std::map<QString, Protocol> protocols{
            {QStringLiteral("RDP"), Protocol::RDP}, {QStringLiteral("VNC"), Protocol::VNC}, {QStringLiteral("SSH"), Protocol::SSH}, {QStringLiteral("SPICE"), Protocol::SPICE}};
        const auto p = protocols.find(v[QStringLiteral("protocol")].toUpper());
        if (p == protocols.end() || v[QStringLiteral("server")].isEmpty()) return {};
        Connection c;
        c.protocol = p->second;
        setAddress(c, v[QStringLiteral("server")]);
        c.name = (v[QStringLiteral("name")].isEmpty() ? QString::fromStdString(c.host) : v[QStringLiteral("name")]).toStdString();
        c.group = v[QStringLiteral("group")].toStdString();
        c.username = v[QStringLiteral("username")].toStdString();
        if (c.protocol == Protocol::RDP && !v[QStringLiteral("domain")].isEmpty()) c.options["domain"] = v[QStringLiteral("domain")].toStdString();
        return {c};
    }
    return {};
}

std::vector<Connection> importPutty(const QString& name, const QByteArray& file)
{
    const QString title = QUrl::fromPercentEncoding(name.toUtf8());
    if (title == QLatin1String("Default Settings")) return {};
    auto v = keyValues(iniSections(QString::fromUtf8(file)).front());
    Connection c;
    c.name = title.toStdString();
    const QString protocol = v[QStringLiteral("protocol")];
    if (protocol == QLatin1String("serial")) {
        c.protocol = Protocol::Serial;
        c.host = v[QStringLiteral("serialline")].toStdString();
        if (!v[QStringLiteral("serialspeed")].isEmpty()) c.options["baud"] = v[QStringLiteral("serialspeed")].toStdString();
        return c.host.empty() ? std::vector<Connection>{} : std::vector<Connection>{c};
    }
    if (protocol != QLatin1String("ssh") && protocol != QLatin1String("telnet")) return {};
    c.protocol = protocol == QLatin1String("ssh") ? Protocol::SSH : Protocol::Telnet;
    QString host = v[QStringLiteral("hostname")];
    if (host.contains(QLatin1Char('@'))) { // user@host works in PuTTY's host field
        c.username = host.section(QLatin1Char('@'), 0, 0).toStdString();
        host = host.section(QLatin1Char('@'), 1);
    }
    if (host.isEmpty()) return {};
    c.host = host.toStdString();
    const int port = v[QStringLiteral("portnumber")].toInt();
    c.port = port == rdc::defaultPort(c.protocol) ? 0 : port;
    if (!v[QStringLiteral("username")].isEmpty()) c.username = v[QStringLiteral("username")].toStdString();
    return {c};
}

// [Bookmarks], [Bookmarks_N]: SubRep= the folder ("A\B"), then name=#icon#type%host%port%user%…#…
std::vector<Connection> importMobaXterm(const QByteArray& file)
{
    std::vector<Connection> found;
    for (const Section& s : iniSections(QString::fromLatin1(file))) { // Windows-1252
        if (!s.name.startsWith(QLatin1String("Bookmarks"))) continue;
        QString folder;
        for (const auto& [key, value] : s.values)
            if (key == QLatin1String("SubRep")) folder = value.trimmed().replace(QLatin1Char('\\'), QLatin1Char('/'));
        for (const auto& [key, value] : s.values) {
            const QStringList parts = value.split(QLatin1Char('#'));
            if (key == QLatin1String("SubRep") || key == QLatin1String("ImgNum") || parts.size() < 3) continue;
            const QStringList f = parts[2].split(QLatin1Char('%'));
            static const std::map<QString, Protocol> types{{QStringLiteral("0"), Protocol::SSH}, {QStringLiteral("4"), Protocol::RDP}, {QStringLiteral("5"), Protocol::VNC}};
            const auto type = types.find(f.value(0));
            if (type == types.end() || f.value(1).isEmpty()) continue;
            Connection c;
            c.protocol = type->second;
            c.name = key.toStdString();
            c.group = folder.toStdString();
            c.host = f[1].toStdString();
            const int port = f.value(2).toInt();
            c.port = port == rdc::defaultPort(c.protocol) ? 0 : port;
            if (c.protocol != Protocol::VNC && f.value(3) != QLatin1String("<default>")) c.username = f.value(3).toStdString();
            found.push_back(std::move(c));
        }
    }
    return found;
}

// <Connections><Node Type="Container" Name=…><Node Type="Connection" Protocol="RDP" Hostname=… Port=… Username=… Domain=…/>
std::vector<Connection> importMremoteng(const QByteArray& xml, QString* error)
{
    std::vector<Connection> found;
    std::vector<QString> folders; // per open Node: its name if a container, else empty
    QXmlStreamReader r(xml);
    static const std::map<QString, Protocol> protocols{{QStringLiteral("RDP"), Protocol::RDP}, {QStringLiteral("VNC"), Protocol::VNC},
                                                       {QStringLiteral("SSH1"), Protocol::SSH}, {QStringLiteral("SSH2"), Protocol::SSH},
                                                       {QStringLiteral("Telnet"), Protocol::Telnet}};
    while (!r.atEnd()) {
        r.readNext();
        if (r.isEndElement() && r.name() == QLatin1String("Node") && !folders.empty()) folders.pop_back();
        if (!r.isStartElement()) continue;
        const QXmlStreamAttributes a = r.attributes();
        if (r.name() == QLatin1String("Connections") && a.value(QLatin1String("FullFileEncryption")) == QLatin1String("True")) {
            *error = QStringLiteral("The file is encrypted; export it from mRemoteNG without full file encryption.");
            return {};
        }
        if (r.name() != QLatin1String("Node")) continue;
        const bool container = a.value(QLatin1String("Type")) == QLatin1String("Container");
        folders.push_back(container ? a.value(QLatin1String("Name")).toString() : QString());
        const auto p = protocols.find(a.value(QLatin1String("Protocol")).toString());
        if (container || p == protocols.end() || a.value(QLatin1String("Hostname")).isEmpty()) continue;
        Connection c;
        c.protocol = p->second;
        c.name = a.value(QLatin1String("Name")).toString().toStdString();
        QStringList path;
        for (const QString& f : folders)
            if (!f.isEmpty()) path << f;
        c.group = path.join(QLatin1Char('/')).toStdString();
        c.host = a.value(QLatin1String("Hostname")).toString().toStdString();
        const int port = a.value(QLatin1String("Port")).toInt();
        c.port = port == rdc::defaultPort(c.protocol) ? 0 : port;
        c.username = a.value(QLatin1String("Username")).toString().toStdString();
        if (c.protocol == Protocol::RDP && !a.value(QLatin1String("Domain")).isEmpty())
            c.options["domain"] = a.value(QLatin1String("Domain")).toString().toStdString();
        found.push_back(std::move(c));
    }
    if (r.hasError()) *error = r.errorString();
    return r.hasError() ? std::vector<Connection>{} : found;
}
