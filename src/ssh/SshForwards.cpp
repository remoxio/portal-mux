#include "ssh/SshForwards.h"

#include <libssh/libssh.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

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
    int fd = socket(addrs->ai_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    const int on = 1;
    if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    if (fd < 0 || ::bind(fd, addrs->ai_addr, addrs->ai_addrlen) != 0 || listen(fd, 16) != 0) {
        error = std::strerror(errno);
        if (fd >= 0) ::close(fd);
        fd = -1;
    }
    freeaddrinfo(addrs);
    return fd;
}

SshForwards::~SshForwards()
{
    for (Pipe& p : pipes_) close(p);
    for (const Listener& l : listeners_) ::close(l.fd);
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
        const ssize_t n = ::write(fd, data, size);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
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
        const int fd = accept4(listener.fd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) return;
        if (listener.spec.kind == 'D') {
            pipes_.push_back({fd, nullptr, 1, {}});
            continue;
        }
        ssh_channel channel = ssh_channel_new(session_);
        if (channel && ssh_channel_open_forward(channel, listener.spec.host.c_str(), listener.spec.hostPort, "127.0.0.1", listener.spec.port) == SSH_OK) {
            pipes_.push_back({fd, channel});
        } else {
            if (channel) ssh_channel_free(channel);
            ::close(fd);
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
    if (p.fd >= 0) ::close(p.fd);
    p.fd = -1;
    if (p.channel) {
        ssh_channel_close(p.channel);
        ssh_channel_free(p.channel);
    }
    p.channel = nullptr;
}

void SshForwards::pump()
{
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
            fd = socket(addrs->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
            if (fd >= 0 && ::connect(fd, addrs->ai_addr, addrs->ai_addrlen) != 0) {
                ::close(fd);
                fd = -1;
            }
            freeaddrinfo(addrs);
        }
        if (fd < 0) {
            ssh_channel_close(channel);
            ssh_channel_free(channel);
            continue;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        pipes_.push_back({fd, channel});
    }
    char buffer[65536];
    for (size_t i = 0; i < pipes_.size(); ++i) {
        Pipe& p = pipes_[i];
        for (;;) { // local → server
            const ssize_t n = p.fd >= 0 ? ::read(p.fd, buffer, sizeof buffer) : -1;
            if (n < 0 && errno == EAGAIN) break;
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
            if (n > 0 && writeAll(p.fd, buffer, size_t(n))) continue;
            if (n < 0 || n > 0 || ssh_channel_is_eof(p.channel)) close(p);
            break;
        }
    }
    std::erase_if(pipes_, [](const Pipe& p) { return p.fd < 0; });
}

} // namespace rdc
