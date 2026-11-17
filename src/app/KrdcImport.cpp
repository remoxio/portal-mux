#include "app/KrdcImport.h"

#include <QSet>
#include <QSize>
#include <QUrl>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <algorithm>
#include <map>

using rdc::Connection;
using rdc::Protocol;

using Group = std::map<QString, QString>;

// KConfig INI: "[a][b]" headers, "key[$e]=value" lines, backslash escapes in values.
static std::map<QString, Group> parseKConfig(const QByteArray& data)
{
    std::map<QString, Group> groups;
    Group* group = &groups[QString()];
    for (const QByteArray& raw : data.split('\n')) {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty() || line.startsWith(u'#')) continue;
        if (line.startsWith(u'[')) {
            group = &groups[line];
            continue;
        }
        const qsizetype eq = line.indexOf(u'=');
        if (eq < 0) continue;
        QString key = line.left(eq).trimmed();
        if (const qsizetype b = key.indexOf(u'['); b >= 0) key.truncate(b);
        QString value;
        const QString v = line.mid(eq + 1).trimmed();
        for (qsizetype i = 0; i < v.size(); ++i) {
            if (v[i] != u'\\' || i + 1 == v.size()) {
                value += v[i];
                continue;
            }
            const QChar e = v[++i];
            value += e == u's' ? u' ' : e == u't' ? u'\t' : e == u'n' ? u'\n' : e == u'r' ? u'\r' : e;
        }
        (*group)[key] = value;
    }
    return groups;
}

static void applyRdp(Connection& c, const auto& get, const auto& num)
{
    const int resolution = num("resolution", 3);
    if (resolution != 3) { // 3 = "Current Window Size", our default
        static const QSize presets[] = {{1280, 720}, {1600, 900}, {1920, 1080}};
        const QSize size = resolution < 3 ? presets[resolution] : QSize(num("width", 0), num("height", 0));
        c.options["width"] = std::to_string(size.width());
        c.options["height"] = std::to_string(size.height());
        c.options["scaling"] = get("scaleToSize") == QLatin1String("true") ? "scale" : "none";
    }
    switch (num("acceleration", 0)) { // Best available, H.264, RemoteFX, Disabled
    case 2: c.options["h264"] = "false"; break;
    case 3: c.options["gfx"] = c.options["h264"] = "false"; break;
    }
    if (const int depth = num("colorDepth", 0); depth == 1 || depth == 2) c.options["colorDepth"] = depth == 1 ? "16" : "8";
    if (const int sound = num("sound", 0); sound == 1 || sound == 2) c.options["audio"] = sound == 1 ? "remote" : "off";
    if (get("consoleMode") == QLatin1String("true")) c.options["console"] = "true";
    if (!get("shareMedia").isEmpty()) c.options["drives"] = get("shareMedia").toStdString();

    const bool nla = get("securityNLA") != QLatin1String("false"), tls = get("securityTLS") != QLatin1String("false"),
               rdp = get("securityRDP") != QLatin1String("false"), ext = get("securityEXT") == QLatin1String("true");
    if (!(nla && tls && rdp)) c.options["security"] = nla ? "nla" : tls ? "tls" : rdp ? "rdp" : ext ? "ext" : "auto";
    if (const int level = num("tlsSecLevel", 1); level >= 0 && level <= 5) c.options["tlsSecLevel"] = std::to_string(level);

    if (const QString gw = get("gatewayServer"); !gw.isEmpty()) {
        const QUrl u(QStringLiteral("x://") + gw);
        c.options["gateway.host"] = u.host().toStdString();
        if (u.port() > 0) c.options["gateway.port"] = std::to_string(u.port());
        if (!get("gatewayUsername").isEmpty()) {
            c.options["gateway.sameCredentials"] = "false";
            c.options["gateway.user"] = get("gatewayUsername").toStdString();
            c.options["gateway.domain"] = get("gatewayDomain").toStdString();
        }
        if (const int t = num("gatewayTransportType", 0); t == 1 || t == 2) c.options["gateway.transport"] = t == 1 ? "http" : "rpc";
    }
}

static void applyVnc(Connection& c, const auto& get, const auto& num)
{
    // KRDC quality presets (1 High, 2 Medium, 3 Low) ~ our Tight settings
    switch (num("quality", 0)) {
    case 2: c.options["jpegQuality"] = "6"; break;
    case 3:
        c.options["jpegQuality"] = "1";
        c.options["compression"] = "9";
        break;
    }
    if (!get("windowedScale").isEmpty()) c.options["scaling"] = get("windowedScale") == QLatin1String("true") ? "scale" : "none";
}

static bool fromUrl(const QString& href, Connection& c)
{
    const QUrl url(href);
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("rdp")) c.protocol = Protocol::RDP;
    else if (scheme == QLatin1String("vnc")) c.protocol = Protocol::VNC;
    else return false;
    if (url.host().isEmpty()) return false;
    c.host = url.host().toStdString();
    c.port = url.port() == rdc::defaultPort(c.protocol) ? 0 : std::max(url.port(), 0);
    c.username = url.userName().toStdString();
    if (const QString domain = QUrlQuery(url).queryItemValue(QStringLiteral("domain")); !domain.isEmpty())
        c.options["domain"] = domain.toStdString();
    return true;
}

std::vector<Connection> importKrdc(const QByteArray& bookmarksXml, const QByteArray& krdcrc)
{
    struct Entry {
        QString href, title, group;
        bool history;
    };
    std::vector<Entry> entries;
    struct Folder {
        QString title;
        bool history = false;
    };
    std::vector<Folder> folders;
    QStringList stack;
    QXmlStreamReader xml(bookmarksXml);
    while (!xml.atEnd()) {
        const auto token = xml.readNext();
        if (token == QXmlStreamReader::EndElement) {
            if (stack.takeLast() == QLatin1String("folder")) folders.pop_back();
            continue;
        }
        if (token != QXmlStreamReader::StartElement) continue;
        const QString name = xml.name().toString();
        if (name == QLatin1String("title") && !stack.isEmpty()) {
            const QString text = xml.readElementText(); // consumes </title>
            if (stack.last() == QLatin1String("bookmark")) entries.back().title = text;
            else if (stack.last() == QLatin1String("folder")) folders.back().title = text;
            continue;
        }
        if (name == QLatin1String("folder")) folders.push_back({});
        else if (name == QLatin1String("krdc-history") && !folders.empty()) folders.back().history = true;
        else if (name == QLatin1String("bookmark"))
            entries.push_back({xml.attributes().value(QLatin1String("href")).toString(), {},
                               folders.empty() ? QString() : folders.back().title,
                               std::any_of(folders.begin(), folders.end(), [](const Folder& f) { return f.history; })});
        stack << name;
    }

    // Bookmarks first, so a history entry for the same URL is dropped as a duplicate.
    std::stable_partition(entries.begin(), entries.end(), [](const Entry& e) { return !e.history; });

    const std::map<QString, Group> config = parseKConfig(krdcrc);
    std::vector<Connection> result;
    QSet<QString> seen;
    for (const Entry& e : entries) {
        Connection c;
        if (!fromUrl(e.href, c) || seen.contains(e.href.toLower())) continue;
        seen << e.href.toLower();
        c.name = (e.title.isEmpty() || e.title == e.href ? QString::fromStdString(c.host) : e.title).toStdString();
        c.group = (e.history ? QStringLiteral("KRDC history") : e.group).toStdString();

        // KRDC keys host settings by the URL; older versions left the user name out.
        QUrl noUser(e.href);
        noUser.setUserName({});
        auto it = config.find(QStringLiteral("[hostpreferences][%1]").arg(e.href));
        if (it == config.end()) it = config.find(QStringLiteral("[hostpreferences][%1]").arg(noUser.toString()));
        static const Group none;
        const Group& prefs = it == config.end() ? none : it->second;
        auto get = [&](const char* key) {
            const auto v = prefs.find(QLatin1String(key));
            return v == prefs.end() ? QString() : v->second;
        };
        auto num = [&](const char* key, int fallback) {
            bool ok = false;
            const int n = get(key).toInt(&ok);
            return ok ? n : fallback;
        };
        if (c.protocol == Protocol::RDP) applyRdp(c, get, num);
        else applyVnc(c, get, num);

        if (get("use_ssh_tunnel") == QLatin1String("true")) { // KRDC tunnels through SSH on the target host itself
            std::string jump = c.host;
            if (!get("ssh_tunnel_user_name").isEmpty()) jump = get("ssh_tunnel_user_name").toStdString() + "@" + jump;
            if (num("ssh_tunnel_port", 22) != 22) jump += ":" + std::to_string(num("ssh_tunnel_port", 22));
            c.options["tunnel"] = jump;
            if (get("use_ssh_tunnel_loopback") == QLatin1String("true")) c.host = "localhost";
        }
        result.push_back(std::move(c));
    }
    return result;
}
