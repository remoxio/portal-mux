#include "ssh/SshTunnel.h"

#include <libssh/libssh.h>

#include "core/Net.h"

#include <vector>

namespace rdc {

SshTunnel::SshTunnel(SessionListener& listener, SshConnection::Params jump, std::string targetHost, int targetPort)
    : listener_(listener), jump_(std::move(jump)), targetHost_(std::move(targetHost)), targetPort_(targetPort)
{
}

SshConnection::Params SshTunnel::parse(const std::string& spec)
{
    SshConnection::Params p;
    std::string rest = spec;
    if (const auto at = rest.find('@'); at != std::string::npos) {
        p.user = rest.substr(0, at);
        rest = rest.substr(at + 1);
    }
    if (const auto colon = rest.rfind(':'); colon != std::string::npos && rest.find(':') == colon) {
        p.port = std::atoi(rest.substr(colon + 1).c_str());
        rest = rest.substr(0, colon);
    }
    p.host = rest;
    if (p.port <= 0) p.port = 22;
    return p;
}

int SshTunnel::listen(int preferredPort)
{
    for (int port : {preferredPort, 0}) {
        const int fd = int(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        if (fd < 0) return 0;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // never reachable from the network
        addr.sin_port = htons(uint16_t(port));
        socklen_t len = sizeof addr;
        if (bind(fd, reinterpret_cast<sockaddr*>(&addr), len) == 0 && ::listen(fd, 4) == 0 &&
            getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            listenFd_ = fd;
            localPort_ = ntohs(addr.sin_port);
            return localPort_;
        }
        net::closeSocket(fd);
    }
    return 0;
}

void SshTunnel::start(std::function<void()> onReady, std::function<void(std::string)> onFailed)
{
    if (worker_.joinable() || listenFd_ < 0) return;
    ssh_ = std::make_unique<SshConnection>(listener_, jump_);
    worker_ = std::jthread([this, onReady = std::move(onReady), onFailed = std::move(onFailed)](std::stop_token st) {
        run(st, onReady, onFailed);
    });
}

void SshTunnel::stop()
{
    if (worker_.joinable()) {
        worker_.request_stop();
        ssh_->abort();
        worker_.join();
    }
    if (listenFd_ >= 0) net::closeSocket(listenFd_);
    listenFd_ = -1;
}

void SshTunnel::run(std::stop_token stop, std::function<void()> onReady, std::function<void(std::string)> onFailed)
{
    if (!ssh_->open([&] { return stop.stop_requested(); })) {
        onFailed(stop.stop_requested() ? std::string() : ssh_->error());
        return;
    }
    onReady();

    struct Pipe {
        int fd;
        ssh_channel channel;
    };
    std::vector<Pipe> pipes;
    auto closePipe = [](Pipe& p) {
        net::closeSocket(p.fd);
        ssh_channel_close(p.channel);
        ssh_channel_free(p.channel);
        p.fd = -1;
    };
    ssh_session session = ssh_->session();
    char buffer[65536];

    // shortcut: one thread, blocking writes; a slow local reader stalls the other forwards
    while (!stop.stop_requested()) {
        std::vector<pollfd> fds{{listenFd_, POLLIN, 0}, {ssh_get_fd(session), POLLIN, 0}};
        for (const Pipe& p : pipes) fds.push_back({p.fd, POLLIN, 0});
        if (poll(fds.data(), fds.size(), 50) < 0) break;
        const size_t polled = pipes.size(); // an accept below appends a pipe that has no pollfd yet

        if (fds[0].revents & POLLIN) {
            const int client = int(accept(listenFd_, nullptr, nullptr));
#ifndef _WIN32
            if (client >= 0) fcntl(client, F_SETFD, FD_CLOEXEC);
#endif
            ssh_channel channel = client >= 0 ? ssh_channel_new(session) : nullptr;
            if (channel && ssh_channel_open_forward(channel, targetHost_.c_str(), targetPort_, "127.0.0.1", localPort_) == SSH_OK) {
                pipes.push_back({client, channel});
            } else {
                if (channel) ssh_channel_free(channel);
                if (client >= 0) net::closeSocket(client);
            }
        }
        for (size_t i = 0; i < polled; ++i) {
            if (!(fds[i + 2].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const long n = recv(pipes[i].fd, buffer, sizeof buffer, 0);
            if (n <= 0 || ssh_channel_write(pipes[i].channel, buffer, uint32_t(n)) < 0) closePipe(pipes[i]);
        }
        for (Pipe& p : pipes) {
            if (p.fd < 0) continue;
            int n;
            while ((n = ssh_channel_read_nonblocking(p.channel, buffer, sizeof buffer, 0)) > 0) {
                for (int off = 0; off < n;) {
                    const long w = send(p.fd, buffer + off, n - off, MSG_NOSIGNAL);
                    if (w <= 0) {
                        n = SSH_ERROR;
                        break;
                    }
                    off += int(w);
                }
                if (n == SSH_ERROR) break;
            }
            if (n == SSH_ERROR || ssh_channel_is_eof(p.channel)) closePipe(p);
        }
        std::erase_if(pipes, [](const Pipe& p) { return p.fd < 0; });
        if (ssh_get_status(session) & (SSH_CLOSED | SSH_CLOSED_ERROR)) break;
    }
    for (Pipe& p : pipes) closePipe(p);
}

void TunneledDisplaySession::connect()
{
    listener_.onStateChanged(SessionState::Connecting);
    tunnel_->start([this] { inner_->connect(); },
                   [this](std::string error) {
                       if (!error.empty()) listener_.onError("SSH tunnel: " + error);
                       listener_.onStateChanged(error.empty() ? SessionState::Disconnected : SessionState::Error);
                   });
}

} // namespace rdc
