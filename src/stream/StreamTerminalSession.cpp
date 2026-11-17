#include "stream/StreamTerminalSession.h"

#ifndef _WIN32
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstring>
#include <map>

namespace rdc {

StreamTerminalSession::StreamTerminalSession(TerminalListener& listener, Connection connection)
    : listener_(listener), connection_(std::move(connection))
{
}

#ifdef _WIN32

void StreamTerminalSession::connect()
{
    listener_.onError("Telnet and serial connections are not available on Windows yet.");
    listener_.onStateChanged(SessionState::Error);
}
void StreamTerminalSession::disconnect() {}
void StreamTerminalSession::write(std::span<const std::byte>) {}
void StreamTerminalSession::resize(int, int) {}
void StreamTerminalSession::run(std::stop_token) {}
int StreamTerminalSession::openSocket(std::string&) { return -1; }
int StreamTerminalSession::openSerial(std::string&) { return -1; }
void StreamTerminalSession::send(std::string_view) {}

#else

void StreamTerminalSession::connect()
{
    listener_.onStateChanged(SessionState::Connecting);
    if (pipe(wake_) != 0) {
        listener_.onError(std::string("Could not start: ") + std::strerror(errno));
        listener_.onStateChanged(SessionState::Error);
        return;
    }
    fcntl(wake_[0], F_SETFD, FD_CLOEXEC);
    fcntl(wake_[1], F_SETFD, FD_CLOEXEC);
    worker_ = std::jthread([this](std::stop_token stop) { run(stop); });
}

int StreamTerminalSession::openSocket(std::string& error)
{
    addrinfo hints{}, *addrs = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const std::string port = std::to_string(connection_.effectivePort());
    if (int rc = getaddrinfo(connection_.host.c_str(), port.c_str(), &hints, &addrs); rc != 0) {
        error = "Cannot resolve " + connection_.host + ": " + gai_strerror(rc);
        return -1;
    }
    int fd = -1;
    std::string why = "no address";
    for (addrinfo* a = addrs; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
        if (fd < 0) continue;
        int rc = ::connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) { // the wake pipe interrupts it: closing the tab doesn't wait
            pollfd fds[2] = {{fd, POLLOUT, 0}, {wake_[0], POLLIN, 0}};
            rc = poll(fds, 2, 15000);
            int soError = rc == 0 ? ETIMEDOUT : 0;
            socklen_t len = sizeof soError;
            if (fds[1].revents) soError = ECANCELED;
            else if (rc > 0) getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len);
            rc = soError ? -1 : 0;
            errno = soError;
        }
        if (rc == 0) break;
        why = std::strerror(errno);
        const bool cancelled = errno == ECANCELED;
        close(fd);
        fd = -1;
        if (cancelled) break;
    }
    freeaddrinfo(addrs);
    if (fd < 0) error = "Cannot connect to " + connection_.host + ":" + port + ": " + why;
    else fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    return fd;
}

int StreamTerminalSession::openSerial(std::string& error)
{
    const std::string& device = connection_.host;
    const int fd = open(device.c_str(), O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (fd < 0) {
        error = "Cannot open " + device + ": " + std::strerror(errno);
        if (errno == EACCES) error += " (on most Linux systems: add yourself to the \"dialout\" group, then log in again)";
        return -1;
    }
    ioctl(fd, TIOCEXCL); // another program opening it would garble both
    static const std::map<int, speed_t> speeds = {{1200, B1200},     {2400, B2400},     {4800, B4800},     {9600, B9600},
                                                  {19200, B19200},   {38400, B38400},   {57600, B57600},   {115200, B115200},
                                                  {230400, B230400}, {460800, B460800}, {921600, B921600}};
    termios t{};
    tcgetattr(fd, &t);
    cfmakeraw(&t);
    const auto speed = speeds.find(connection_.number("baud"));
    if (speed == speeds.end()) {
        close(fd);
        error = "Unsupported speed: " + connection_.option("baud");
        return -1;
    }
    cfsetispeed(&t, speed->second);
    cfsetospeed(&t, speed->second);
    t.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
    t.c_cflag |= CLOCAL | CREAD;
    const int bits = connection_.number("dataBits");
    t.c_cflag |= bits == 5 ? CS5 : bits == 6 ? CS6 : bits == 7 ? CS7 : CS8;
    const std::string parity = connection_.option("parity"), flow = connection_.option("flowControl");
    if (parity != "none") t.c_cflag |= PARENB | (parity == "odd" ? PARODD : 0);
    if (connection_.number("stopBits") == 2) t.c_cflag |= CSTOPB;
    if (flow == "hardware") t.c_cflag |= CRTSCTS;
    if (flow == "software") t.c_iflag |= IXON | IXOFF;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        error = "Cannot set up " + device + ": " + std::strerror(errno);
        close(fd);
        return -1;
    }
    return fd;
}

void StreamTerminalSession::send(std::string_view bytes)
{
    const char* p = bytes.data();
    size_t left = bytes.size();
    while (left > 0 && fd_ >= 0) {
        const ssize_t n = ::write(fd_, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        p += n;
        left -= size_t(n);
    }
}

void StreamTerminalSession::run(std::stop_token stop)
{
    const bool serial = connection_.protocol == Protocol::Serial;
    std::string error;
    const int fd = serial ? openSerial(error) : openSocket(error);
    if (fd < 0) {
        if (stop.stop_requested()) return;
        listener_.onError(error);
        listener_.onStateChanged(SessionState::Error);
        return;
    }
    {
        std::lock_guard lock(mutex_);
        fd_ = fd;
        if (!serial && !connection_.flag("raw")) {
            telnet_.emplace(connection_.option("term"));
            telnet_->resize(cols_, rows_);
            send(telnet_->start());
        }
    }
    listener_.onStateChanged(SessionState::Connected);
    char buffer[16384];
    while (!stop.stop_requested()) {
        pollfd fds[2] = {{fd, POLLIN, 0}, {wake_[0], POLLIN, 0}};
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (fds[1].revents) break;
        const ssize_t n = read(fd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break; // closed by the server, or the device was unplugged
        std::string text;
        {
            std::lock_guard lock(mutex_);
            if (telnet_) send(telnet_->receive({buffer, size_t(n)}, text));
            else text.assign(buffer, size_t(n));
        }
        if (!text.empty()) listener_.onData(std::move(text));
    }
    if (!stop.stop_requested()) listener_.onStateChanged(SessionState::Disconnected);
}

void StreamTerminalSession::disconnect()
{
    if (!worker_.joinable()) return;
    worker_.request_stop();
    const char byte = 0;
    [[maybe_unused]] const ssize_t woke = ::write(wake_[1], &byte, 1);
    worker_.join();
    std::lock_guard lock(mutex_);
    for (int* fd : {&fd_, &wake_[0], &wake_[1]}) {
        if (*fd >= 0) close(*fd);
        *fd = -1;
    }
}

void StreamTerminalSession::write(std::span<const std::byte> data)
{
    // shortcut: blocking writes on the UI thread, a big paste waits; move to the reader if it stalls
    const std::string_view keys(reinterpret_cast<const char*>(data.data()), data.size());
    std::lock_guard lock(mutex_);
    send(telnet_ ? telnet_->encode(keys) : std::string(keys));
}

void StreamTerminalSession::resize(int cols, int rows)
{
    std::lock_guard lock(mutex_);
    cols_ = cols;
    rows_ = rows;
    if (telnet_) send(telnet_->resize(cols, rows));
}

#endif

} // namespace rdc
