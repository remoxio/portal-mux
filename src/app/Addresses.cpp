#include "app/Addresses.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QUrl>

#include <algorithm>

using rdc::Connection;
using rdc::Protocol;

static QString tr(const char* text) { return QCoreApplication::translate("Addresses", text); }

static const char* schemeOf(Protocol p)
{
    switch (p) {
    case Protocol::VNC: return "vnc";
    case Protocol::SPICE: return "spice";
    case Protocol::SSH: return "ssh";
    case Protocol::Telnet: return "telnet";
    default: return "rdp";
    }
}

static void splitHostPort(const QString& text, QString& host, int& port)
{
    port = 0;
    if (text.startsWith(QLatin1Char('['))) {
        const qsizetype end = text.indexOf(QLatin1Char(']'));
        host = text.mid(1, end - 1);
        if (end > 0 && text.mid(end + 1).startsWith(QLatin1Char(':'))) port = text.mid(end + 2).toInt();
    } else if (text.count(QLatin1Char(':')) == 1) {
        host = text.section(QLatin1Char(':'), 0, 0);
        port = text.section(QLatin1Char(':'), 1).toInt();
    } else {
        host = text;
    }
}

static QString displayName(const Connection& c)
{
    QString name = QString::fromStdString(c.host);
    if (name.contains(QLatin1Char(':'))) name = QLatin1Char('[') + name + QLatin1Char(']');
    if (c.port > 0) name += QLatin1Char(':') + QString::number(c.port);
    return c.username.empty() ? name : QString::fromStdString(c.username) + QLatin1Char('@') + name;
}

std::optional<Connection> connectionFromAddress(const QString& input, Protocol fallback, QString* error)
{
    const QString text = input.trimmed();
    const QUrl url(text.contains(QLatin1String("://")) ? text : QLatin1String(schemeOf(fallback)) + QLatin1String("://") + text,
                   QUrl::StrictMode);
    static const std::pair<const char*, Protocol> schemes[] = {
        {"rdp", Protocol::RDP}, {"vnc", Protocol::VNC}, {"spice", Protocol::SPICE}, {"ssh", Protocol::SSH}, {"telnet", Protocol::Telnet}};
    const auto scheme = std::find_if(std::begin(schemes), std::end(schemes),
                                     [&](const auto& s) { return url.scheme().compare(QLatin1String(s.first), Qt::CaseInsensitive) == 0; });
    if (text.isEmpty() || !url.isValid() || url.host().isEmpty() || scheme == std::end(schemes) || url.path().size() > 1) {
        *error = tr("\"%1\" is not an address to connect to. Examples: myhost, user@myhost:3390, vnc://myhost:1, "
                    "ssh://user@myhost").arg(text);
        return std::nullopt;
    }
    Connection c;
    c.protocol = scheme->second;
    c.host = url.host().toStdString();
    c.port = std::max(0, url.port(0));
    c.username = url.userName().toStdString();
    c.password = url.password().toStdString();
    c.name = displayName(c).toStdString();
    return c;
}

static std::optional<Connection> fromRdpFile(const QString& text, QString* error)
{
    Connection c;
    c.protocol = Protocol::RDP;
    int serverPort = 0;
    auto flag = [](const QString& v) { return v.toInt() ? "true" : "false"; };
    for (const QString& line : text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts)) {
        // name:type:value, the value may contain ':' itself
        const qsizetype first = line.indexOf(QLatin1Char(':')), second = line.indexOf(QLatin1Char(':'), first + 1);
        if (first < 0 || second < 0) continue;
        const QString key = line.left(first).trimmed().toLower(), value = line.mid(second + 1).trimmed();
        if (key == QLatin1String("full address")) {
            QString host;
            splitHostPort(value, host, c.port);
            c.host = host.toStdString();
        } else if (key == QLatin1String("server port")) {
            serverPort = value.toInt();
        } else if (key == QLatin1String("username")) {
            if (value.contains(QLatin1Char('\\'))) { // DOMAIN\user
                c.options["domain"] = value.section(QLatin1Char('\\'), 0, 0).toStdString();
                c.username = value.section(QLatin1Char('\\'), 1).toStdString();
            } else {
                c.username = value.toStdString();
            }
        } else if (key == QLatin1String("domain") && !value.isEmpty()) {
            c.options["domain"] = value.toStdString();
        } else if (key == QLatin1String("gatewayhostname") && !value.isEmpty()) {
            QString host;
            int port = 0;
            splitHostPort(value, host, port);
            c.options["gateway.host"] = host.toStdString();
            if (port > 0) c.options["gateway.port"] = std::to_string(port);
        } else if (key == QLatin1String("gatewayusagemethod")) {
            // 0 none, 1 always, 2 if direct fails, 3 default, 4 bypass for local addresses
            if (value == QLatin1String("1")) c.options["gateway.usage"] = "direct";
            if (value == QLatin1String("2") || value == QLatin1String("4")) c.options["gateway.usage"] = "detect";
            if (value == QLatin1String("0")) c.options["gateway.usage"] = "none";
        } else if (key == QLatin1String("audiomode")) {
            c.options["audio"] = value == QLatin1String("1") ? "remote" : value == QLatin1String("2") ? "off" : "local";
        } else if (key == QLatin1String("audiocapturemode")) {
            c.options["microphone"] = flag(value);
        } else if (key == QLatin1String("redirectclipboard")) {
            c.options["clipboard"] = flag(value);
        } else if (key == QLatin1String("redirectprinters")) {
            c.options["printers"] = flag(value);
        } else if (key == QLatin1String("redirectsmartcards")) {
            c.options["smartcards"] = flag(value);
        } else if (key == QLatin1String("administrative session") || key == QLatin1String("connect to console")) {
            if (value.toInt()) c.options["console"] = "true";
        } else if (key == QLatin1String("loadbalanceinfo") && !value.isEmpty()) {
            c.options["loadBalanceInfo"] = value.toStdString();
        } else if (key == QLatin1String("alternate shell") && !value.isEmpty()) {
            c.options["alternateShell"] = value.toStdString();
        } else if (key == QLatin1String("shell working directory") && !value.isEmpty()) {
            c.options["workingDir"] = value.toStdString();
        } else if (key == QLatin1String("pcb") && !value.isEmpty()) {
            c.options["preconnectionBlob"] = value.toStdString();
        } else if (key == QLatin1String("use multimon")) {
            c.options["multimon"] = flag(value);
        }
    }
    if (c.options["gateway.usage"] == "none") {
        c.options.erase("gateway.host");
        c.options.erase("gateway.usage");
    } else if (c.options["gateway.usage"].empty()) {
        c.options.erase("gateway.usage");
    }
    if (c.port == 0) c.port = serverPort == 3389 ? 0 : serverPort;
    if (c.host.empty()) {
        *error = tr("The file names no computer (\"full address\").");
        return std::nullopt;
    }
    return c;
}

static std::optional<Connection> fromVncFile(const QString& text, QString* error)
{
    Connection c;
    c.protocol = Protocol::VNC;
    for (const QString& line : text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts)) {
        const QString key = line.section(QLatin1Char('='), 0, 0).trimmed(), value = line.section(QLatin1Char('='), 1).trimmed();
        if (key.compare(QLatin1String("Host"), Qt::CaseInsensitive) == 0 || // RealVNC
            key.compare(QLatin1String("ServerName"), Qt::CaseInsensitive) == 0) { // TigerVNC
            // host, host:display or host::port
            QString host = value;
            if (const qsizetype at = value.lastIndexOf(QLatin1String("::")); at > 0 && !value.startsWith(QLatin1Char('['))) {
                host = value.left(at);
                c.port = value.mid(at + 2).toInt();
            } else {
                int display = 0;
                splitHostPort(value, host, display);
                c.port = display > 0 ? 5900 + display : 0;
            }
            c.host = host.toStdString();
        } else if (key.compare(QLatin1String("UserName"), Qt::CaseInsensitive) == 0) {
            c.username = value.toStdString();
        }
    }
    if (c.host.empty()) {
        *error = tr("The file names no computer (\"Host\" or \"ServerName\").");
        return std::nullopt;
    }
    return c;
}

std::optional<Connection> connectionFromFile(const QString& path, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = tr("Could not read %1: %2").arg(path, file.errorString());
        return std::nullopt;
    }
    const QByteArray data = file.read(1 << 20);
    QStringDecoder decode(QStringConverter::encodingForData(data).value_or(QStringConverter::Utf8)); // Windows writes UTF-16
    const QString text = decode(data);
    const QString suffix = QFileInfo(path).suffix().toLower();
    std::optional<Connection> c;
    if (suffix == QLatin1String("rdp")) {
        c = fromRdpFile(text, error);
    } else if (suffix == QLatin1String("vnc") || suffix == QLatin1String("tigervnc")) {
        c = fromVncFile(text, error);
    } else {
        *error = tr("%1 is not an .rdp or .vnc file.").arg(path);
        return std::nullopt;
    }
    if (c) c->name = QFileInfo(path).completeBaseName().toStdString();
    else *error = QFileInfo(path).fileName() + QLatin1String(": ") + *error;
    return c;
}

QByteArray magicPacket(const QString& mac)
{
    QString hex = mac.trimmed();
    hex.remove(QRegularExpression(QStringLiteral("[:\\-.]")));
    if (!QRegularExpression(QStringLiteral("^[0-9A-Fa-f]{12}$")).match(hex).hasMatch()) return {};
    const QByteArray address = QByteArray::fromHex(hex.toLatin1());
    QByteArray packet(6, char(0xFF)); // then the address 16 times
    for (int i = 0; i < 16; ++i) packet += address;
    return packet;
}

QString titleFolder(const QString& title, const QString& home)
{
    static const QRegularExpression prompt(QStringLiteral("^[^@\\s]+@[^:\\s]+:\\s*((?:~|/).*)$"));
    const QRegularExpressionMatch m = prompt.match(title);
    if (!m.hasMatch()) return {};
    QString path = m.captured(1).trimmed();
    if (!path.startsWith(QLatin1Char('~'))) return path;
    if ((path.size() > 1 && path[1] != QLatin1Char('/')) || home.isEmpty()) return {}; // ~user: not known here
    return home + path.mid(1);
}

QString shellQuoted(const QString& path)
{
    static const QRegularExpression plain(QStringLiteral("^[A-Za-z0-9_./+:@%,=-]+$"));
    if (plain.match(path).hasMatch()) return path;
    return QLatin1Char('\'') + QString(path).replace(QLatin1Char('\''), QStringLiteral("'\\''")) + QLatin1Char('\'');
}
