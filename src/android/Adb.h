#pragma once

#include <string>
#include <utility>
#include <vector>

namespace rdc::adb {

// Blocking socket to the local adb server; shutdown() may be called from another thread.
class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) : fd_(fd) {}
    Socket(Socket&& o) noexcept : fd_(std::exchange(o.fd_, -1)) {}
    Socket& operator=(Socket&& o) noexcept;
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { close(); }

    bool valid() const { return fd_ >= 0; }
    bool readAll(void* buf, size_t n);
    long readSome(void* buf, size_t n);  // 0 = EOF, < 0 = error
    bool writeAll(const void* buf, size_t n);
    void shutdown();
    void close();

private:
    int fd_ = -1;
};

std::string findAdb(const std::string& configured);

Socket open(const std::string& adb, const std::string& serial, const std::string& service, std::string* error);

std::vector<std::string> devices(const std::string& adb, std::string* error);

bool connectDevice(const std::string& adb, const std::string& hostPort, std::string* error);

bool push(const std::string& adb, const std::string& serial, const std::string& local, const std::string& remote,
          std::string* error);

} // namespace rdc::adb
