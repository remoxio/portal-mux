#pragma once

#include "core/Connection.h"

#include <QString>

#include <functional>
#include <vector>

class QObject;

// Saved connections in one JSON file; passwords live in the keychain
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

    const QString& path() const { return path_; }

private:
    QString path_;
    std::vector<rdc::Connection> connections_;
};

const char* protocolName(rdc::Protocol p);

namespace secrets {
// `done` runs on the UI thread; empty = no password
void read(const std::string& id, QObject* context, std::function<void(QString password)> done);
void write(const std::string& id, const QString& password, QObject* context, std::function<void(QString error)> done);
void remove(const std::string& id);
} // namespace secrets
