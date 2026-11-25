#pragma once

#include <memory>
#include <string>
#include <vector>

struct pollfd;
struct ssh_callbacks_struct;
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

// An X client's setup packet that came with `fake` as its MIT-MAGIC-COOKIE-1, given the display's own authorization
// (name and data; empty: none) instead. 1: done, in place; 0: more bytes needed; -1: not the fake cookie
int x11Authorize(std::string& packet, const std::string& fake, const std::string& name, const std::string& data);

// Port forwards on the session's worker; shortcut: blocking local I/O, a slow peer stalls the rest
class SshForwards {
public:
    explicit SshForwards(ssh_session session) : session_(session) {}
    ~SshForwards();

    std::vector<std::string> start(const std::vector<ForwardSpec>& specs); // notes: what runs, what failed
    // agent and X11 forwarding (ssh -A, -X), asked for on the shell's channel before its shell starts; notes as start()'s
    std::vector<std::string> share(ssh_channel shell, bool agent, bool x11);
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
        std::string in;                // SOCKS bytes not handled yet, or the X client's setup
        bool x11 = false;              // its setup not passed on yet
    };
    static ssh_channel agentOpened(ssh_session, void* self);
    static ssh_channel x11Opened(ssh_session, const char*, int, void* self);
    ssh_channel opened(int fd, bool x11); // a channel the server opens, to fd; -1: refused
    void accept(const Listener& listener);
    void socks(Pipe& pipe);
    void close(Pipe& pipe);

    ssh_session session_;
    std::vector<Listener> listeners_;
    std::vector<ForwardSpec> remote_; // with the port the server bound
    std::vector<Pipe> pipes_;
    std::vector<Pipe> opened_; // by the server, inside libssh: pump() takes them
    std::unique_ptr<ssh_callbacks_struct> callbacks_;
    std::string fakeCookie_, displayAuth_, displayCookie_;
};

} // namespace rdc
