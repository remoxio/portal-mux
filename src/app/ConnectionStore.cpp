#include "app/ConnectionStore.h"

#include "core/Properties.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUuid>


#include <utility>

#include <algorithm>

using rdc::Connection;
using rdc::Protocol;

static constexpr std::pair<Protocol, const char*> protocolNames[] = {
    {Protocol::RDP, "rdp"}, {Protocol::VNC, "vnc"}, {Protocol::SPICE, "spice"}, {Protocol::SSH, "ssh"}, {Protocol::Android, "android"},
                                                     {Protocol::Moonlight, "moonlight"}, {Protocol::Local, "local"},
                                                     {Protocol::Telnet, "telnet"}, {Protocol::Serial, "serial"}};

const char* protocolName(Protocol p)
{
    for (auto [proto, name] : protocolNames)
        if (proto == p) return name;
    return "rdp";
}

static Protocol protocolFromName(const QString& s)
{
    for (auto [proto, name] : protocolNames)
        if (s == QLatin1String(name)) return proto;
    return Protocol::RDP;
}

static std::string str(const QJsonObject& o, const char* key) { return o.value(QLatin1String(key)).toString().toStdString(); }

bool ConnectionStore::load(QString* error)
{
    connections_.clear();
    QFile file(path_);
    if (!file.exists()) return true;
    QJsonParseError parseError{};
    QJsonDocument doc;
    if (file.open(QIODevice::ReadOnly)) doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!file.isOpen() || parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        *error = file.isOpen() ? parseError.errorString() : file.errorString();
        file.close();
        QFile::remove(path_ + ".broken");
        QFile::copy(path_, path_ + ".broken");
        return false;
    }
    connections_ = fromJson(doc.object().value(QLatin1String("connections")).toArray());
    return true;
}

bool ConnectionStore::save(QString* error) const
{
    const QJsonArray list = toJson(connections_);
    QJsonObject root{{QLatin1String("version"), 1}, {QLatin1String("connections"), list}};

    QSaveFile file(path_); // atomic: a crash keeps the old file
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(root).toJson()) < 0 || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    return true;
}

std::vector<Connection> ConnectionStore::fromJson(const QJsonArray& list)
{
    std::vector<Connection> connections;
    for (const QJsonValue& v : list) {
        const QJsonObject o = v.toObject();
        Connection c;
        c.id = str(o, "id");
        c.name = str(o, "name");
        c.group = str(o, "group");
        c.protocol = protocolFromName(o.value(QLatin1String("protocol")).toString());
        c.host = str(o, "host");
        c.port = o.value(QLatin1String("port")).toInt();
        c.username = str(o, "username");
        const QJsonObject options = o.value(QLatin1String("options")).toObject();
        for (auto it = options.begin(); it != options.end(); ++it)
            if (rdc::findProperty(c.protocol, it.key().toStdString()))
                c.options[it.key().toStdString()] = it.value().toString().toStdString();
        if (c.id.empty()) c.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
        connections.push_back(std::move(c));
    }
    return connections;
}

QJsonArray ConnectionStore::toJson(const std::vector<Connection>& connections)
{
    QJsonArray list;
    for (const Connection& c : connections) {
        QJsonObject o;
        auto put = [&](const char* key, const std::string& value) {
            if (!value.empty()) o.insert(QLatin1String(key), QString::fromStdString(value));
        };
        put("id", c.id);
        put("name", c.name);
        put("group", c.group);
        put("protocol", protocolName(c.protocol));
        put("host", c.host);
        if (c.port > 0) o.insert(QLatin1String("port"), c.port);
        put("username", c.username);
        QJsonObject options;
        for (const auto& [key, value] : c.options) {
            const rdc::PropertyDef* d = rdc::findProperty(c.protocol, key);
            // defaults aren't stored, so a changed default applies
            if (d && d->kind != rdc::PropertyKind::Secret && value != d->defaultValue)
                options.insert(QString::fromStdString(key), QString::fromStdString(value));
        }
        if (!options.isEmpty()) o.insert(QLatin1String("options"), options);
        list.append(o);
    }
    return list;
}

const Connection* ConnectionStore::find(const std::string& id) const
{
    auto it = std::find_if(connections_.begin(), connections_.end(), [&](const Connection& c) { return c.id == id; });
    return it == connections_.end() ? nullptr : &*it;
}

std::string ConnectionStore::upsert(Connection connection)
{
    connection.password.clear(); // in secrets
    for (const rdc::PropertyDef& d : rdc::properties(connection.protocol))
        if (d.kind == rdc::PropertyKind::Secret) connection.options.erase(d.key);
    if (connection.id.empty()) connection.id = QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
    const std::string id = connection.id;
    auto it = std::find_if(connections_.begin(), connections_.end(), [&](const Connection& c) { return c.id == id; });
    if (it == connections_.end())
        connections_.push_back(std::move(connection));
    else
        *it = std::move(connection);
    return id;
}

void ConnectionStore::remove(const std::string& id)
{
    std::erase_if(connections_, [&](const Connection& c) { return c.id == id; });
}

bool ConnectionStore::ensureLocalTerminal()
{
    if (find(localTerminalId)) return false;
    const auto made = std::find_if(connections_.begin(), connections_.end(), [](const Connection& c) { return c.protocol == rdc::Protocol::Local; });
    if (made != connections_.end()) {
        made->id = localTerminalId;
        return true;
    }
    Connection c;
    c.id = localTerminalId;
    c.name = "localhost";
    c.protocol = rdc::Protocol::Local;
    connections_.insert(connections_.begin(), std::move(c));
    return true;
}
