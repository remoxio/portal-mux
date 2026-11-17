#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// GameStream HTTP: pairing, host info, app list, launch (from moonlight-qt 6.2.0).
namespace rdc::gamestream {

// Aborts a blocking request (pairing waits for the PIN).
struct Cancel {
    std::atomic<bool> cancelled{false};
    std::atomic<int> fd{-1};
    void cancel();
};

struct Identity {
    std::string uniqueId, certPem, keyPem;
};
bool loadIdentity(const std::string& dir, Identity& identity, std::string* error);

struct Host {
    std::string address;
    int httpPort = 47989, httpsPort = 47984;
    std::string serverCertPem; // pinned after pairing; HTTPS requests only accept this certificate
};

struct ServerInfo {
    std::string hostname, appVersion, gfeVersion;
    int httpsPort = 47984, codecModes = 0, currentGame = 0;
    bool paired = false;   // as the host sees us (only meaningful over HTTPS)
    bool sunshine = false;
    uint32_t apolloPermissions = 0; // over HTTPS, Apollo only
};

constexpr uint32_t apolloClipboardSet = 1u << 16, apolloClipboardRead = 1u << 17;

struct App {
    int id = 0;
    std::string title;
};

// timeoutSeconds 0 = wait indefinitely
std::string request(const Host& host, const Identity& id, bool https, const std::string& verb, const std::string& query,
                    int timeoutSeconds, Cancel& cancel, std::string* error);

bool serverInfo(const Host& host, const Identity& id, bool https, ServerInfo& info, Cancel& cancel, std::string* error);

// Apollo's text clipboard, only while this client streams: sends *send, or reads it into received
bool clipboard(const Host& host, const Identity& id, const std::string* send, std::string& received, Cancel& cancel,
               std::string* error);
std::vector<App> appList(const Host& host, const Identity& id, Cancel& cancel, std::string* error);

bool pair(const Host& host, const Identity& id, const std::string& pin, const std::string& appVersion,
          std::string& serverCertPem, Cancel& cancel, std::string* error);

std::string toHex(const std::string& bytes);

std::string xmlValue(const std::string& xml, const std::string& tag);
int xmlStatus(const std::string& xml, std::string* message);

} // namespace rdc::gamestream
