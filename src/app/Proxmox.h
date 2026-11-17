#pragma once

#include "core/Connection.h"

#include <QByteArray>
#include <QString>

#include <optional>
#include <string>
#include <vector>

// Proxmox VE API answers as plain data; no network (see ProxmoxClient).
namespace proxmox {

struct Server {
    std::string id, name, host;
    int port = 8006;
    std::string user;        // user@realm, or user@realm!tokenid
    std::string fingerprint; // pinned SHA-256, hex
};

bool isApiToken(const std::string& user);
bool parseAddress(const QString& text, std::string& host, int& port);

struct Node {
    std::string name;
    bool online = false;
    double cpu = 0; // 0..1
    int maxcpu = 0;
    long long mem = 0, maxmem = 0, uptime = 0;
};

struct Guest {
    std::string node, type; // type: "qemu" or "lxc"
    int vmid = 0;
    std::string name, status;
    double cpu = 0; // 0..1
    long long mem = 0, maxmem = 0;
    bool isTemplate = false;
    std::string key() const { return node + "/" + type + "/" + std::to_string(vmid); }
};

struct Resources {
    std::vector<Node> nodes;   // sorted by name
    std::vector<Guest> guests; // sorted by node, vmid
};

// GET /cluster/resources
std::optional<Resources> parseResources(const QByteArray& json, QString* error);
// POST …/spiceproxy; caFile: where spiceCa() was written.
std::optional<rdc::Connection> spiceConnection(const QByteArray& json, const std::string& name, const std::string& caFile,
                                               QString* error);
QByteArray spiceCa(const QByteArray& json); // the API escapes its newlines

// POST …/vncproxy websocket=1, or …/termproxy
struct VncTicket { int port = 0; std::string ticket, password, user; };
std::optional<VncTicket> parseVncProxy(const QByteArray& json, QString* error);
// termproxy over vncwebsocket, as Proxmox's xterm.js: "user:ticket\n" first, then these
QByteArray termInput(const QByteArray& bytes);
QByteArray termResize(int cols, int rows);
bool usesSpice(const QByteArray& statusJson); // GET …/status/current
QString errorText(const QByteArray& json);

} // namespace proxmox
