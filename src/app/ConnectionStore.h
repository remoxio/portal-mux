#pragma once

#include "app/Secrets.h"
#include "core/Connection.h"

#include <QString>

#include <functional>
#include <vector>

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
    bool ensureLocalTerminal(); // the first local terminal made by hand becomes it, else a new one; true: changed

    const QString& path() const { return path_; }

private:
    QString path_;
    std::vector<rdc::Connection> connections_;
};

const char* protocolName(rdc::Protocol p);
