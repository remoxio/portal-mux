#pragma once

#include "app/Secrets.h"
#include "core/Connection.h"

#include <QString>

#include <functional>
#include <vector>

class QJsonArray;
class QObject;

inline constexpr char localTerminalId[] = "local-terminal"; // this computer's terminal: always there, never deleted

// Saved connections in one JSON file; passwords are in secrets
class ConnectionStore {
public:
    explicit ConnectionStore(QString path) : path_(std::move(path)) {}

    // Unreadable file is copied to <path>.broken so save() can't destroy it
    bool load(QString* error);
    bool save(QString* error) const;

    const std::vector<rdc::Connection>& all() const { return connections_; }
    const rdc::Connection* find(const std::string& id) const;
    std::string upsert(rdc::Connection connection);
    void remove(const std::string& id);
    // the list's order is the user's
    void swap(const std::string& a, const std::string& b);
    void moveToEnd(const std::string& id);
    void sortByName();
    bool ensureLocalTerminal(); // the first local terminal made by hand becomes it, else a new one; true: changed

    const QString& path() const { return path_; }

    // the file's "connections" array; exports use it too
    static std::vector<rdc::Connection> fromJson(const QJsonArray& list);
    static QJsonArray toJson(const std::vector<rdc::Connection>& connections);

private:
    QString path_;
    std::vector<rdc::Connection> connections_;
};

const char* protocolName(rdc::Protocol p);
