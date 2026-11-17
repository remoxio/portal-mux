#pragma once

// BSD sockets or Winsock; sockets stay int

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#define SHUT_RDWR SD_BOTH
#define MSG_NOSIGNAL 0
#define SOCK_CLOEXEC 0

inline int poll(pollfd* fds, size_t n, int timeoutMs) { return WSAPoll(fds, ULONG(n), timeoutMs); }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <string>
#include <system_error>

namespace rdc::net {

#ifdef _WIN32
inline const bool winsockStarted = [] {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
}();

constexpr int timedOut = WSAETIMEDOUT;
inline int lastError() { return WSAGetLastError(); }
inline bool connectPending(int error) { return error == WSAEWOULDBLOCK; }
inline bool wouldBlock(int error) { return error == WSAEWOULDBLOCK; }
inline void closeSocket(int fd) { closesocket(SOCKET(fd)); }
inline void setNonBlocking(int fd, bool on)
{
    u_long mode = on;
    ioctlsocket(SOCKET(fd), FIONBIO, &mode);
}
inline void setReceiveTimeout(int fd, int seconds)
{
    const DWORD ms = DWORD(seconds) * 1000; // 0 = no timeout
    setsockopt(SOCKET(fd), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof ms);
}
#else
constexpr int timedOut = ETIMEDOUT;
inline int lastError() { return errno; }
inline bool connectPending(int error) { return error == EINPROGRESS; }
inline bool wouldBlock(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
inline void closeSocket(int fd) { ::close(fd); }
inline void setNonBlocking(int fd, bool on)
{
    const int flags = fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
}
inline void setReceiveTimeout(int fd, int seconds)
{
    timeval tv{seconds, 0}; // 0 = no timeout
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}
#endif

// close-on-exec; macOS lacks SOCK_CLOEXEC: set right after
inline int openSocket(int family, int type, int protocol)
{
#ifdef SOCK_CLOEXEC
    return int(socket(family, type | SOCK_CLOEXEC, protocol));
#else
    const int fd = socket(family, type, protocol);
    if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
#endif
}

inline std::string errorText(int error) { return std::system_category().message(error); }

inline int socketError(int fd)
{
    int error = 0;
    socklen_t len = sizeof error;
    getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &len);
    return error;
}

} // namespace rdc::net
