#pragma once

#include <string>
#include <vector>

struct pollfd;
typedef struct ssh_session_struct* ssh_session;
typedef struct ssh_channel_struct* ssh_channel;

namespace rdc {

struct ForwardSpec {
    char kind = 'L'; // 'L' local, 'R' remote, 'D' SOCKS
    std::string bind; // empty: loopback
    int port = 0;
    std::string host;
    int hostPort = 0;
};

// OpenSSH's -L/-R form: [bind:]port:host:hostport, separated by commas or spaces; IPv6 in brackets
std::vector<ForwardSpec> parseForwards(char kind, const std::string& text, std::string* error);

// Port forwards over one SSH session, pumped from that session's worker thread.
// shortcut: blocking writes to local sockets and blocking connects for -R; a slow peer stalls the others
class SshForwards {
public:
    explicit SshForwards(ssh_session session) : session_(session) {}
    ~SshForwards();

    std::vector<std::string> start(const std::vector<ForwardSpec>& specs); // notes: what runs, what failed
    void addPollFds(std::vector<pollfd>& fds) const;
    void pump();

private:
    struct Listener {
        int fd;
        ForwardSpec spec;
    };
    struct Pipe {
        int fd;
        ssh_channel channel = nullptr; // SOCKS: null until its request
        int socksStage = 0;            // 1 greeting, 2 request, 0 data
        std::string in;                // SOCKS bytes not handled yet
    };
    void accept(const Listener& listener);
    void socks(Pipe& pipe);
    void close(Pipe& pipe);

    ssh_session session_;
    std::vector<Listener> listeners_;
    std::vector<ForwardSpec> remote_; // with the port the server bound
    std::vector<Pipe> pipes_;
};

} // namespace rdc
