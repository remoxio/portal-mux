#pragma once

#include <map>
#include <string>
#include <string_view>

namespace rdc {

enum class Protocol { RDP, VNC, SPICE, SSH, Android, Moonlight, Local, Telnet, Serial };

inline int defaultPort(Protocol p)
{
    switch (p) {
    case Protocol::RDP: return 3389;
    case Protocol::VNC: return 5900;
    case Protocol::SPICE: return 5900;
    case Protocol::SSH: return 22;
    case Protocol::Android: return 0;
    case Protocol::Moonlight: return 47989;
    case Protocol::Local: return 0;
    case Protocol::Telnet: return 23;
    case Protocol::Serial: return 0;
    }
    return 0;
}

// secrets live in the OS keychain, never in the connection file
struct Connection {
    std::string id, name, group;
    Protocol protocol = Protocol::RDP;
    std::string host;
    int port = 0; // 0 = defaultPort(protocol)
    std::string username, password;
    std::map<std::string, std::string> options;

    std::string option(std::string_view key) const;
    bool flag(std::string_view key) const;
    int number(std::string_view key) const;

    int effectivePort() const
    {
        if (protocol == Protocol::VNC && port > 0 && port < 100) return 5900 + port; // VNC display number (:1 = 5901)
        return port > 0 ? port : defaultPort(protocol);
    }
};

} // namespace rdc
