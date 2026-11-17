#include "local/LocalTerminalSession.h"

#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif
#endif

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern char** environ;

namespace rdc {

LocalTerminalSession::LocalTerminalSession(TerminalListener& listener, Connection connection)
    : listener_(listener), connection_(std::move(connection))
{
}

#ifdef _WIN32

void LocalTerminalSession::connect()
{
    listener_.onError("A local terminal is not available on Windows yet.");
    listener_.onStateChanged(SessionState::Error);
}
void LocalTerminalSession::disconnect() {}
void LocalTerminalSession::write(std::span<const std::byte>) {}
void LocalTerminalSession::resize(int, int) {}
void LocalTerminalSession::run(std::stop_token) {}

#else

void LocalTerminalSession::connect()
{
    listener_.onStateChanged(SessionState::Connecting);
    // only exec-safe calls after fork()
    const passwd* user = getpwuid(getuid());
    const char* shellEnv = std::getenv("SHELL");
    const std::string chosen = connection_.option("shell");
    const std::string shell = !chosen.empty()         ? chosen
                              : shellEnv && *shellEnv ? shellEnv
                              : user && user->pw_shell && *user->pw_shell ? user->pw_shell
                                                                          : "/bin/sh";
    const std::string command = connection_.option("command");
    std::string dir = connection_.option("workingDir");
    if (dir.empty()) dir = user && user->pw_dir ? user->pw_dir : "/";
    std::vector<std::string> args;
    std::string program;
    if (command.empty()) { // login shell: argv[0] "-bash"
        program = shell;
        args = {"-" + shell.substr(shell.rfind('/') + 1)};
    } else {
        program = chosen.empty() ? "/bin/sh" : chosen;
        args = {program.substr(program.rfind('/') + 1), "-c", command};
    }
    std::vector<std::string> env;
    for (char** e = environ; *e; ++e)
        if (std::strncmp(*e, "TERM=", 5) != 0 && std::strncmp(*e, "COLORTERM=", 10) != 0) env.emplace_back(*e);
    env.push_back("TERM=" + connection_.option("term"));
    env.push_back("COLORTERM=truecolor");
    std::vector<char*> argv, envp;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);
    for (std::string& e : env) envp.push_back(e.data());
    envp.push_back(nullptr);

    if (pipe(wake_) != 0) {
        listener_.onError(std::string("Could not start the terminal: ") + std::strerror(errno));
        listener_.onStateChanged(SessionState::Error);
        return;
    }
    fcntl(wake_[0], F_SETFD, FD_CLOEXEC); // shell must not inherit it
    fcntl(wake_[1], F_SETFD, FD_CLOEXEC);
    winsize size{};
    size.ws_col = static_cast<unsigned short>(cols_);
    size.ws_row = static_cast<unsigned short>(rows_);
    const pid_t pid = forkpty(&master_, nullptr, nullptr, &size);
    if (pid == 0) {
        if (chdir(dir.c_str()) != 0 && chdir("/") != 0) _exit(126);
        execve(program.c_str(), argv.data(), envp.data());
        _exit(127);
    }
    if (pid < 0) {
        listener_.onError(std::string("Could not start the terminal: ") + std::strerror(errno));
        listener_.onStateChanged(SessionState::Error);
        return;
    }
    fcntl(master_, F_SETFD, FD_CLOEXEC); // not into later shells
    child_ = pid;
    listener_.onStateChanged(SessionState::Connected);
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

void LocalTerminalSession::run(std::stop_token stop)
{
    char buffer[16384];
    while (!stop.stop_requested()) {
        pollfd fds[2] = {{master_, POLLIN, 0}, {wake_[0], POLLIN, 0}};
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents) break;
        const ssize_t n = read(master_, buffer, sizeof buffer);
        if (n > 0) listener_.onData(std::string(buffer, size_t(n)));
        else if (n < 0 && errno == EINTR) continue;
        else break; // EIO: shell exited
    }
    if (stop.stop_requested()) return; // disconnect() reaps the child
    if (const pid_t pid = child_.exchange(-1); pid > 0) waitpid(pid, nullptr, 0);
    listener_.onStateChanged(SessionState::Disconnected);
}

void LocalTerminalSession::disconnect()
{
    if (!worker_.joinable()) return;
    worker_.request_stop(); // first: the reader then skips the hang-up report
    if (const pid_t pid = child_.exchange(-1); pid > 0) {
        kill(pid, SIGHUP);
        for (int i = 0; i < 20 && waitpid(pid, nullptr, WNOHANG) == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        if (waitpid(pid, nullptr, WNOHANG) == 0) {
            kill(pid, SIGKILL);
            waitpid(pid, nullptr, 0);
        }
    }
    const char byte = 0;
    [[maybe_unused]] const ssize_t woke = ::write(wake_[1], &byte, 1);
    worker_.join();
    for (int* fd : {&master_, &wake_[0], &wake_[1]}) {
        if (*fd >= 0) close(*fd);
        *fd = -1;
    }
}

void LocalTerminalSession::write(std::span<const std::byte> data)
{
    // shortcut: blocking writes on the UI thread, a big paste waits; move to the reader if it stalls
    const char* p = reinterpret_cast<const char*>(data.data());
    size_t left = data.size();
    while (left > 0 && master_ >= 0) {
        const ssize_t n = ::write(master_, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        p += n;
        left -= size_t(n);
    }
}

void LocalTerminalSession::resize(int cols, int rows)
{
    cols_ = cols;
    rows_ = rows;
    if (master_ < 0) return;
    winsize size{};
    size.ws_col = static_cast<unsigned short>(cols);
    size.ws_row = static_cast<unsigned short>(rows);
    ioctl(master_, TIOCSWINSZ, &size);
}

#endif

} // namespace rdc
