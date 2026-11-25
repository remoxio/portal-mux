#include "ssh/SshTerminalSession.h"

#include "ssh/SshForwards.h"

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include "core/Net.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fcntl.h>
#include <random>
#include <string_view>

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
    for (int i = 0; uploaded_ && !finished_ && i < 100; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(20)); // removes them
    ssh_->abort();
    worker_.join();
}

void SshTerminalSession::upload(std::string data, std::string extension, std::function<void(std::string, std::string)> done)
{
    tasks_.post([this, data = std::move(data), extension = std::move(extension), done = std::move(done)] {
        std::string error;
        std::string path = channel_ ? store(data, extension, error) : std::string();
        if (!channel_) error = "Not connected.";
        done(path, error);
    });
}

void SshTerminalSession::installKey(std::string key, std::function<void(std::string, bool)> done)
{
    tasks_.post([this, key = std::move(key), done = std::move(done)] {
        std::string error = channel_ ? "" : "Not connected.";
        const bool added = channel_ && addAuthorizedKey(key, error);
        done(error, added);
    });
}

std::string SshTerminalSession::sftpError(const std::string& what) const
{
    return what + ": " + (sftp_ ? "SFTP error " + std::to_string(sftp_get_error(sftp_)) + ", " : "") + ssh_->lastError();
}

std::string SshTerminalSession::sftpHome(std::string& error)
{
    if (!sftp_) {
        sftp_ = sftp_new(ssh_->session());
        if (!sftp_ || sftp_init(sftp_) != SSH_OK) {
            error = sftpError("Could not start SFTP");
            if (sftp_) sftp_free(sftp_);
            sftp_ = nullptr;
            return {};
        }
    }
    char* home = sftp_canonicalize_path(sftp_, ".");
    if (!home) {
        error = sftpError("No home folder");
        return {};
    }
    std::string path = home;
    ssh_string_free_char(home);
    return path;
}

bool SshTerminalSession::addAuthorizedKey(const std::string& key, std::string& error)
{
    const std::string home = sftpHome(error);
    if (home.empty()) return false;
    const std::string path = home + "/.ssh/authorized_keys";
    sftp_mkdir(sftp_, (home + "/.ssh").c_str(), 0700); // may exist
    std::string text;
    if (sftp_file in = sftp_open(sftp_, path.c_str(), O_RDONLY, 0)) {
        char buffer[16384];
        for (ssize_t n; text.size() < (4 << 20) && (n = sftp_read(in, buffer, sizeof buffer)) > 0;) text.append(buffer, size_t(n));
        sftp_close(in);
    }
    const size_t from = key.find(' ') + 1, to = key.find(' ', from);
    if (text.find(key.substr(from, to - from)) != std::string::npos) return false; // its base64: the comment may differ
    sftp_file out = sftp_open(sftp_, path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (!out) {
        error = sftpError("Cannot write " + path);
        return false;
    }
    const std::string line = (text.empty() || text.back() == '\n' ? "" : "\n") + key + "\n";
    const bool written = sftp_write(out, line.data(), line.size()) == ssize_t(line.size());
    sftp_close(out);
    if (!written) error = sftpError("Cannot write " + path);
    return written;
}

// ~/.cache/portal-mux/paste/paste-<time>-<random><extension>, 0600; files there older than a day go
std::string SshTerminalSession::store(const std::string& data, const std::string& extension, std::string& error)
{
    auto fail = [&](const std::string& what) {
        error = sftpError(what);
        return std::string();
    };
    std::string dir = sftpHome(error);
    if (dir.empty()) return {};
    for (const char* part : {"/.cache", "/portal-mux", "/paste"}) sftp_mkdir(sftp_, (dir += part).c_str(), 0700); // may exist
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&now));
    char random[16];
    std::snprintf(random, sizeof random, "%06x", unsigned(std::random_device{}() & 0xffffff));
    const std::string path = dir + "/paste-" + stamp + "-" + random + extension;
    sftp_file file = sftp_open(sftp_, path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (!file) return fail("Cannot create " + path);
    bool written = true;
    for (size_t at = 0; written && at < data.size();) {
        const ssize_t n = sftp_write(file, data.data() + at, std::min<size_t>(data.size() - at, 32768));
        written = n > 0;
        at += written ? size_t(n) : 0;
    }
    sftp_close(file);
    if (!written) {
        sftp_unlink(sftp_, path.c_str());
        return fail("Cannot write " + path);
    }
    uploads_.push_back(path);
    uploaded_ = true;
    if (sftp_attributes mine = sftp_stat(sftp_, path.c_str())) { // the server's clock
        const uint64_t dayAgo = mine->mtime > 86400 ? uint64_t(mine->mtime) - 86400 : 0;
        sftp_attributes_free(mine);
        if (sftp_dir listing = sftp_opendir(sftp_, dir.c_str())) {
            while (sftp_attributes a = sftp_readdir(sftp_, listing)) {
                if (a->name && std::string_view(a->name).starts_with("paste-") && a->mtime < dayAgo)
                    sftp_unlink(sftp_, (dir + "/" + a->name).c_str());
                sftp_attributes_free(a);
            }
            sftp_closedir(listing);
        }
    }
    // Windows' SFTP: /C:/Users/…
    return path.size() > 3 && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == ':' ? path.substr(1) : path;
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
    SshForwards forwards(ssh_->session());
    std::vector<std::string> notes;
    ssh_channel channel = ssh_channel_new(ssh_->session());
    bool started = channel && ssh_channel_open_session(channel) == SSH_OK &&
                   ssh_channel_request_pty_size(channel, connection_.option("term").c_str(), cols_, rows_) == SSH_OK;
    if (started) {
        notes = forwards.share(channel, connection_.flag("forwardAgent"), connection_.flag("forwardX11"));
        started = (command.empty() ? ssh_channel_request_shell(channel) : ssh_channel_request_exec(channel, command.c_str())) == SSH_OK;
    }
    if (!started) {
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
    for (const std::string& note : forwards.start(specs)) notes.push_back(note);
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
        // shortcut: typed input waits for the 20 ms poll
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
    if (sftp_) {
        if (!failed)
            for (const std::string& path : uploads_) sftp_unlink(sftp_, path.c_str());
        sftp_free(sftp_);
        sftp_ = nullptr;
    }
    finished_ = true;
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
