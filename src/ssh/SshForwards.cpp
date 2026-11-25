#include "ssh/SshForwards.h"

#include <libssh/callbacks.h>
#include <libssh/libssh.h>

#include "core/Net.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <sstream>

#ifndef _WIN32
#include <sys/un.h>
#endif

namespace rdc {

static std::vector<std::string> splitFields(const std::string& spec)
{
    std::vector<std::string> fields;
    std::string field;
    bool bracket = false;
    for (char c : spec) {
        if (c == '[') bracket = true;
        else if (c == ']') bracket = false;
        else if (c == ':' && !bracket) {
            fields.push_back(field);
            field.clear();
        } else field += c;
    }
    fields.push_back(field);
    return fields;
}

static int portOf(const std::string& text)
{
    if (text.empty() || text.size() > 5 || !std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; })) return 0;
    const int port = std::stoi(text);
    return port <= 65535 ? port : 0;
}

std::vector<ForwardSpec> parseForwards(char kind, const std::string& text, std::string* error)
{
    std::vector<ForwardSpec> specs;
    std::string token;
    auto take = [&] {
        if (token.empty()) return;
        const std::vector<std::string> f = splitFields(token);
        ForwardSpec s;
        s.kind = kind;
        const size_t base = f.size() == 4 ? 1 : 0;
        if (f.size() == 4) s.bind = f[0];
        if ((f.size() == 3 || f.size() == 4) && (s.port = portOf(f[base])) && !f[base + 1].empty() && (s.hostPort = portOf(f[base + 2]))) {
            s.host = f[base + 1];
            specs.push_back(s);
        } else if (error) {
            *error += (error->empty() ? "" : "; ") + std::string("not [bind:]port:host:hostport: ") + token;
        }
        token.clear();
    };
    for (char c : text) {
        if (c == ',' || c == ' ' || c == '\n' || c == '\t') take();
        else token += c;
    }
    take();
    return specs;
}

static int listenOn(const std::string& bind, int port, std::string& error)
{
    addrinfo hints{}, *addrs = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    const std::string host = bind.empty() || bind == "localhost" ? "127.0.0.1" : bind == "*" ? "" : bind;
    if (int rc = getaddrinfo(host.empty() ? nullptr : host.c_str(), std::to_string(port).c_str(), &hints, &addrs); rc != 0) {
        error = gai_strerror(rc);
        return -1;
    }
    int fd = net::openSocket(addrs->ai_family, SOCK_STREAM, 0);
#ifndef _WIN32 // Windows: lets others steal the port
    const int on = 1;
    if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
#endif
    if (fd >= 0) net::setNonBlocking(fd, true);
    if (fd < 0 || ::bind(fd, addrs->ai_addr, int(addrs->ai_addrlen)) != 0 || listen(fd, 16) != 0) {
        error = net::errorText(net::lastError());
        if (fd >= 0) net::closeSocket(fd);
        fd = -1;
    }
    freeaddrinfo(addrs);
    return fd;
}

int x11Authorize(std::string& packet, const std::string& fake, const std::string& name, const std::string& data)
{
    if (packet.size() < 12) return 0;
    const bool big = packet[0] == 'B';
    auto get16 = [&](size_t at) { return size_t(uint8_t(packet[at + !big]) << 8 | uint8_t(packet[at + big])); };
    auto padded = [](size_t n) { return (n + 3) & ~size_t(3); };
    const size_t nameSize = get16(6), dataSize = get16(8), end = 12 + padded(nameSize) + padded(dataSize);
    if (packet.size() < end) return 0;
    if (packet.compare(12, nameSize, "MIT-MAGIC-COOKIE-1") != 0 || packet.compare(12 + padded(nameSize), dataSize, fake) != 0 ||
        dataSize != fake.size())
        return -1;
    std::string out = packet.substr(0, 6);
    for (size_t v : {name.size(), data.size()}) {
        out += char(big ? v >> 8 : v);
        out += char(big ? v : v >> 8);
    }
    out.append(2, '\0');
    out += name;
    out.append(padded(name.size()) - name.size(), '\0');
    out += data;
    out.append(padded(data.size()) - data.size(), '\0');
    packet = out + packet.substr(end);
    return 1;
}

#ifndef _WIN32
static int connectUnix(const std::string& path)
{
    sockaddr_un addr{};
    if (path.empty() || path.size() >= sizeof addr.sun_path) return -1;
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path, path.data(), path.size());
    const int fd = net::openSocket(AF_UNIX, SOCK_STREAM, 0);
    if (fd >= 0 && ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        net::closeSocket(fd);
        return -1;
    }
    return fd;
}
#endif

static int connectAgent()
{
#ifdef _WIN32
    return -1; // shortcut: Windows' agent is a named pipe, not reached yet
#else
    const char* path = std::getenv("SSH_AUTH_SOCK");
    return path ? connectUnix(path) : -1;
#endif
}

// $DISPLAY: ":0", "unix:0.0", a socket path (XQuartz: "/private/tmp/…:0") or "host:0" (TCP 6000 + display)
static int connectX11()
{
    const char* display = std::getenv("DISPLAY");
    const std::string d = display ? display : "";
    const size_t colon = d.rfind(':');
    if (colon == std::string::npos) return -1;
    const std::string host = d.substr(0, colon);
    const int number = std::atoi(d.c_str() + colon + 1);
#ifndef _WIN32
    if (!host.empty() && host[0] == '/') return connectUnix(d);
    if (host.empty() || host == "unix") return connectUnix("/tmp/.X11-unix/X" + std::to_string(number));
#endif
    addrinfo hints{}, *addrs = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.empty() ? "localhost" : host.c_str(), std::to_string(6000 + number).c_str(), &hints, &addrs) != 0) return -1;
    int fd = net::openSocket(addrs->ai_family, SOCK_STREAM, 0);
    if (fd >= 0 && ::connect(fd, addrs->ai_addr, int(addrs->ai_addrlen)) != 0) {
        net::closeSocket(fd);
        fd = -1;
    }
    freeaddrinfo(addrs);
    return fd;
}

// xauth's cookie for $DISPLAY; none: the X server takes clients without one
static void displayCookie(std::string& name, std::string& cookie)
{
#ifndef _WIN32
    const char* display = std::getenv("DISPLAY");
    if (!display || std::strchr(display, '\'')) return;
    FILE* f = popen(("xauth list '" + std::string(display) + "' 2>/dev/null").c_str(), "r");
    if (!f) return;
    char line[512];
    while (cookie.empty() && fgets(line, sizeof line, f)) {
        std::istringstream in(line);
        std::string where, proto, hex;
        if (!(in >> where >> proto >> hex) || proto != "MIT-MAGIC-COOKIE-1" || hex.size() != 32) continue;
        name = proto;
        for (size_t i = 0; i < hex.size(); i += 2) cookie += char(std::strtol(hex.substr(i, 2).c_str(), nullptr, 16));
    }
    pclose(f);
#endif
}

std::vector<std::string> SshForwards::share(ssh_channel shell, bool agent, bool x11)
{
    std::vector<std::string> notes;
    if (!agent && !x11) return notes;
    callbacks_ = std::make_unique<ssh_callbacks_struct>();
    *callbacks_ = {};
    ssh_callbacks_init(callbacks_.get());
    callbacks_->userdata = this;
    if (agent) {
        if (!std::getenv("SSH_AUTH_SOCK")) {
            notes.push_back("No SSH agent here to forward (SSH_AUTH_SOCK is not set)");
        } else if (ssh_channel_request_auth_agent(shell) == SSH_OK) {
            callbacks_->channel_open_request_auth_agent_function = agentOpened;
            notes.push_back("Forwarding the SSH agent");
        } else {
            notes.push_back(std::string("The server refused agent forwarding: ") + ssh_get_error(session_));
        }
    }
    if (x11) {
        const char* display = std::getenv("DISPLAY");
        std::random_device random;
        for (int i = 0; i < 16; ++i) fakeCookie_ += char(random());
        std::string hex;
        for (unsigned char c : fakeCookie_) hex += "0123456789abcdef"[c >> 4], hex += "0123456789abcdef"[c & 15];
        const char* dot = display ? std::strrchr(display, '.') : nullptr;
        const int screen = dot && dot > std::strrchr(display, ':') ? std::atoi(dot + 1) : 0;
        if (!display || !*display) {
            notes.push_back("No X display here (DISPLAY is not set)");
        } else if (ssh_channel_request_x11(shell, 0, "MIT-MAGIC-COOKIE-1", hex.c_str(), screen) == SSH_OK) {
            displayCookie(displayAuth_, displayCookie_);
            callbacks_->channel_open_request_x11_function = x11Opened;
            notes.push_back(std::string("X11 forwarding to ") + display);
        } else {
            notes.push_back(std::string("The server refused X11 forwarding: ") + ssh_get_error(session_));
        }
    }
    ssh_set_callbacks(session_, callbacks_.get());
    return notes;
}

ssh_channel SshForwards::agentOpened(ssh_session, void* self) { return static_cast<SshForwards*>(self)->opened(connectAgent(), false); }

ssh_channel SshForwards::x11Opened(ssh_session, const char*, int, void* self) { return static_cast<SshForwards*>(self)->opened(connectX11(), true); }

ssh_channel SshForwards::opened(int fd, bool x11)
{
    ssh_channel channel = fd >= 0 ? ssh_channel_new(session_) : nullptr;
    if (!channel) {
        if (fd >= 0) net::closeSocket(fd);
        return nullptr;
    }
    net::setNonBlocking(fd, true);
    Pipe p{fd, channel};
    p.x11 = x11;
    opened_.push_back(std::move(p));
    return channel;
}

SshForwards::~SshForwards()
{
    if (callbacks_) { // libssh keeps the pointer
        static ssh_callbacks_struct none = [] {
            ssh_callbacks_struct c{};
            ssh_callbacks_init(&c);
            return c;
        }();
        ssh_set_callbacks(session_, &none);
    }
    for (Pipe& p : opened_) close(p);
    for (Pipe& p : pipes_) close(p);
    for (const Listener& l : listeners_) net::closeSocket(l.fd);
    for (const ForwardSpec& r : remote_) ssh_channel_cancel_forward(session_, r.bind.empty() ? "localhost" : r.bind.c_str(), r.port);
}

std::vector<std::string> SshForwards::start(const std::vector<ForwardSpec>& specs)
{
    std::vector<std::string> notes;
    for (ForwardSpec s : specs) {
        const std::string target = s.host + ":" + std::to_string(s.hostPort);
        const std::string where = (s.bind.empty() ? "localhost" : s.bind) + ":" + std::to_string(s.port);
        if (s.kind == 'R') {
            int bound = 0;
            if (ssh_channel_listen_forward(session_, s.bind.empty() ? "localhost" : s.bind.c_str(), s.port, &bound) == SSH_OK) {
                if (bound > 0) s.port = bound;
                remote_.push_back(s);
                notes.push_back("Forwarding the server's " + where + " to " + target);
            } else {
                notes.push_back("The server refused to forward " + where + ": " + ssh_get_error(session_));
            }
            continue;
        }
        std::string error;
        const int fd = listenOn(s.bind, s.port, error);
        if (fd < 0) {
            notes.push_back("Cannot listen on " + where + ": " + error);
            continue;
        }
        listeners_.push_back({fd, s});
        notes.push_back(s.kind == 'D' ? "SOCKS proxy on " + where : "Forwarding " + where + " to " + target);
    }
    return notes;
}

void SshForwards::addPollFds(std::vector<pollfd>& fds) const
{
    for (const Listener& l : listeners_) fds.push_back({l.fd, POLLIN, 0});
    for (const Pipe& p : pipes_)
        if (p.fd >= 0) fds.push_back({p.fd, POLLIN, 0});
}

static bool writeAll(int fd, const char* data, size_t size)
{
    while (size > 0) {
        const long n = ::send(fd, data, int(size), MSG_NOSIGNAL);
        if (n < 0 && (net::wouldBlock(net::lastError()) || net::lastError() == EINTR)) {
            pollfd p{fd, POLLOUT, 0};
            if (poll(&p, 1, 1000) <= 0) return false; // the peer stopped reading
            continue;
        }
        if (n <= 0) return false;
        data += n;
        size -= size_t(n);
    }
    return true;
}

void SshForwards::accept(const Listener& listener)
{
    for (;;) {
        const int fd = int(::accept(listener.fd, nullptr, nullptr));
        if (fd < 0) return;
#ifndef _WIN32
        fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
        net::setNonBlocking(fd, true);
        if (listener.spec.kind == 'D') {
            pipes_.push_back({fd, nullptr, 1, {}});
            continue;
        }
        ssh_channel channel = ssh_channel_new(session_);
        if (channel && ssh_channel_open_forward(channel, listener.spec.host.c_str(), listener.spec.hostPort, "127.0.0.1", listener.spec.port) == SSH_OK) {
            pipes_.push_back({fd, channel});
        } else {
            if (channel) ssh_channel_free(channel);
            net::closeSocket(fd);
        }
    }
}

// RFC 1928: no authentication, CONNECT only
void SshForwards::socks(Pipe& p)
{
    const auto& b = p.in;
    if (p.socksStage == 1) {
        if (b.size() < 2 || b.size() < size_t(2 + uint8_t(b[1]))) return;
        const bool noAuth = b[0] == 5 && std::memchr(b.data() + 2, 0, uint8_t(b[1])) != nullptr;
        writeAll(p.fd, noAuth ? "\x05\x00" : "\x05\xff", 2);
        if (!noAuth) return close(p);
        p.in.erase(0, 2 + uint8_t(b[1]));
        p.socksStage = 2;
    }
    if (p.socksStage != 2 || b.size() < 5) return;
    const uint8_t type = uint8_t(b[3]);
    const size_t addressSize = type == 1 ? 4 : type == 4 ? 16 : type == 3 ? 1 + uint8_t(b[4]) : 0;
    if (b[0] != 5 || b[1] != 1 || addressSize == 0) {
        writeAll(p.fd, "\x05\x07\x00\x01\x00\x00\x00\x00\x00\x00", 10); // command not supported
        return close(p);
    }
    if (b.size() < 4 + addressSize + 2) return;
    std::string host;
    char text[INET6_ADDRSTRLEN] = {};
    if (type == 3) host = b.substr(5, addressSize - 1);
    else host = inet_ntop(type == 1 ? AF_INET : AF_INET6, b.data() + 4, text, sizeof text) ? text : "";
    const int port = uint8_t(b[4 + addressSize]) << 8 | uint8_t(b[5 + addressSize]);
    p.in.erase(0, 4 + addressSize + 2);
    p.channel = ssh_channel_new(session_);
    if (!p.channel || ssh_channel_open_forward(p.channel, host.c_str(), port, "127.0.0.1", 0) != SSH_OK) {
        writeAll(p.fd, "\x05\x05\x00\x01\x00\x00\x00\x00\x00\x00", 10); // refused
        return close(p);
    }
    writeAll(p.fd, "\x05\x00\x00\x01\x00\x00\x00\x00\x00\x00", 10);
    p.socksStage = 0;
    if (!p.in.empty()) ssh_channel_write(p.channel, p.in.data(), uint32_t(p.in.size()));
    p.in.clear();
}

void SshForwards::close(Pipe& p)
{
    if (p.fd >= 0) net::closeSocket(p.fd);
    p.fd = -1;
    if (p.channel) {
        ssh_channel_close(p.channel);
        ssh_channel_free(p.channel);
    }
    p.channel = nullptr;
}

void SshForwards::pump()
{
    for (Pipe& p : opened_) pipes_.push_back(std::move(p));
    opened_.clear();
    for (const Listener& l : listeners_) accept(l);
    int port = 0;
    while (!remote_.empty()) { // connections to the server's forwarded ports
        ssh_channel channel = ssh_channel_open_forward_port(session_, 0, &port, nullptr, nullptr);
        if (!channel) break;
        const auto r = std::find_if(remote_.begin(), remote_.end(), [&](const ForwardSpec& s) { return s.port == port; });
        addrinfo hints{}, *addrs = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        int fd = -1;
        if (r != remote_.end() && getaddrinfo(r->host.c_str(), std::to_string(r->hostPort).c_str(), &hints, &addrs) == 0) {
            fd = net::openSocket(addrs->ai_family, SOCK_STREAM, 0);
            if (fd >= 0 && ::connect(fd, addrs->ai_addr, int(addrs->ai_addrlen)) != 0) {
                net::closeSocket(fd);
                fd = -1;
            }
            freeaddrinfo(addrs);
        }
        if (fd < 0) {
            ssh_channel_close(channel);
            ssh_channel_free(channel);
            continue;
        }
        net::setNonBlocking(fd, true);
        pipes_.push_back({fd, channel});
    }
    char buffer[65536];
    for (size_t i = 0; i < pipes_.size(); ++i) {
        Pipe& p = pipes_[i];
        for (;;) { // local → server
            const long n = p.fd >= 0 ? ::recv(p.fd, buffer, sizeof buffer, 0) : -1;
            if (n < 0 && net::wouldBlock(net::lastError())) break;
            if (n <= 0) {
                close(p);
                break;
            }
            if (p.socksStage) {
                p.in.append(buffer, size_t(n));
                socks(p);
            } else if (ssh_channel_write(p.channel, buffer, uint32_t(n)) < 0) {
                close(p);
            }
            if (p.fd < 0) break;
        }
        while (p.channel && !p.socksStage) { // server → local
            const int n = ssh_channel_read_nonblocking(p.channel, buffer, sizeof buffer, 0);
            if (n > 0 && p.x11) {
                p.in.append(buffer, size_t(n));
                const int done = x11Authorize(p.in, fakeCookie_, displayAuth_, displayCookie_);
                if (done < 0 || (done > 0 && !writeAll(p.fd, p.in.data(), p.in.size()))) {
                    close(p);
                    break;
                }
                if (done > 0) p.x11 = false, p.in.clear();
                continue;
            }
            if (n > 0 && writeAll(p.fd, buffer, size_t(n))) continue;
            if (n < 0 || n > 0 || ssh_channel_is_eof(p.channel)) close(p);
            break;
        }
    }
    std::erase_if(pipes_, [](const Pipe& p) { return p.fd < 0; });
}

} // namespace rdc
