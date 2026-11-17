#include "ssh/SshTerminalSession.h"

#include "ssh/SshForwards.h"

#include <libssh/libssh.h>

#include <poll.h>

#include <chrono>

namespace rdc {

SshTerminalSession::SshTerminalSession(TerminalListener& listener, Connection connection, std::string knownHostsFile)
    : listener_(listener), connection_(std::move(connection)), knownHostsFile_(std::move(knownHostsFile))
{
}

void SshTerminalSession::connect()
{
    if (worker_.joinable()) return;
    ssh_ = std::make_unique<SshConnection>(listener_, SshConnection::fromConnection(connection_, knownHostsFile_));
    worker_ = std::jthread([this](std::stop_token st) { run(st); });
}

void SshTerminalSession::disconnect()
{
    if (!worker_.joinable()) return;
    worker_.request_stop();
    ssh_->abort();
    worker_.join();
}

void SshTerminalSession::write(std::span<const std::byte> data)
{
    tasks_.post([this, bytes = std::string(reinterpret_cast<const char*>(data.data()), data.size())] {
        if (channel_) ssh_channel_write(channel_, bytes.data(), uint32_t(bytes.size()));
    });
}

void SshTerminalSession::resize(int cols, int rows)
{
    tasks_.post([this, cols, rows] {
        cols_ = cols;
        rows_ = rows;
        if (channel_) ssh_channel_change_pty_size(channel_, cols, rows);
    });
}

void SshTerminalSession::run(std::stop_token stop)
{
    listener_.onStateChanged(SessionState::Connecting);
    tasks_.drain(); // writes before connect are dropped (channel_ is null)
    auto cancelled = [&] { return stop.stop_requested(); };
    if (!ssh_->open(cancelled)) {
        if (!ssh_->error().empty() && !cancelled()) {
            listener_.onError(ssh_->error());
            listener_.onStateChanged(SessionState::Error);
        } else {
            listener_.onStateChanged(SessionState::Disconnected);
        }
        return;
    }

    const std::string command = connection_.option("command");
    ssh_channel channel = ssh_channel_new(ssh_->session());
    if (!channel || ssh_channel_open_session(channel) != SSH_OK ||
        ssh_channel_request_pty_size(channel, connection_.option("term").c_str(), cols_, rows_) != SSH_OK ||
        (command.empty() ? ssh_channel_request_shell(channel) : ssh_channel_request_exec(channel, command.c_str())) != SSH_OK) {
        if (!cancelled()) listener_.onError("Could not start a shell: " + ssh_->lastError());
        if (channel) ssh_channel_free(channel);
        listener_.onStateChanged(cancelled() ? SessionState::Disconnected : SessionState::Error);
        return;
    }
    channel_ = channel;
    listener_.onStateChanged(SessionState::Connected);

    std::string error;
    std::vector<ForwardSpec> specs = parseForwards('L', connection_.option("localForwards"), &error);
    for (ForwardSpec& s : parseForwards('R', connection_.option("remoteForwards"), &error)) specs.push_back(s);
    if (const int socks = connection_.number("socksPort"); socks > 0) specs.push_back({'D', {}, socks, {}, 0});
    SshForwards forwards(ssh_->session());
    std::vector<std::string> notes = forwards.start(specs);
    if (!error.empty()) notes.push_back("Forwarding settings: " + error);
    for (const std::string& note : notes) listener_.onData("\x1b[2m[" + note + "]\x1b[0m\r\n");

    bool failed = false;
    char buffer[16384];
    const auto keepalive = std::chrono::seconds(connection_.number("keepalive"));
    auto lastTraffic = std::chrono::steady_clock::now();
    while (!cancelled()) {
        if (keepalive.count() > 0 && std::chrono::steady_clock::now() - lastTraffic > keepalive) {
            ssh_send_ignore(ssh_->session(), "keepalive"); // keeps NAT/firewall state alive
            lastTraffic = std::chrono::steady_clock::now();
        }
        // shortcut: 20 ms poll timeout also polls typed input, use ssh_event + wake fd if idle wakeups matter
        std::vector<pollfd> fds{{ssh_get_fd(ssh_->session()), POLLIN, 0}};
        forwards.addPollFds(fds);
        poll(fds.data(), fds.size(), 20);
        const int n = ssh_channel_read_nonblocking(channel, buffer, sizeof buffer, 0);
        if (n == SSH_ERROR) {
            failed = true;
            break;
        }
        forwards.pump();
        if (n > 0) {
            listener_.onData(std::string(buffer, size_t(n)));
            lastTraffic = std::chrono::steady_clock::now();
        }
        if (ssh_channel_is_eof(channel)) break;
        tasks_.drain();
    }
    channel_ = nullptr;
    ssh_channel_close(channel);
    ssh_channel_free(channel);
    if (failed && !cancelled()) {
        listener_.onError("SSH connection lost: " + ssh_->lastError());
        listener_.onStateChanged(SessionState::Error);
    } else {
        listener_.onStateChanged(SessionState::Disconnected);
    }
}

} // namespace rdc
