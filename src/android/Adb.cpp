#include "android/Adb.h"

#include "core/Net.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 // macOS: SO_NOSIGPIPE is set in connectServer() instead
#endif

namespace fs = std::filesystem;

namespace rdc::adb {

Socket& Socket::operator=(Socket&& o) noexcept
{
    if (this != &o) {
        close();
        fd_ = std::exchange(o.fd_, -1);
    }
    return *this;
}

bool Socket::readAll(void* buf, size_t n)
{
    auto* p = static_cast<char*>(buf);
    while (n > 0) {
        const long r = readSome(p, n);
        if (r <= 0) return false;
        p += r;
        n -= size_t(r);
    }
    return true;
}

long Socket::readSome(void* buf, size_t n)
{
    for (;;) {
        const long r = ::recv(fd_, static_cast<char*>(buf), int(n), 0);
        if (r < 0 && errno == EINTR) continue;
        return long(r);
    }
}

bool Socket::writeAll(const void* buf, size_t n)
{
    auto* p = static_cast<const char*>(buf);
    while (n > 0) {
        const long r = ::send(fd_, p, int(n), MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return false;
        p += r;
        n -= size_t(r);
    }
    return true;
}

void Socket::shutdown()
{
    if (fd_ >= 0) ::shutdown(fd_, SHUT_RDWR);
}

void Socket::close()
{
    if (fd_ >= 0) net::closeSocket(std::exchange(fd_, -1));
}

std::string findAdb(const std::string& configured)
{
    if (!configured.empty()) return configured;
    std::vector<fs::path> sdks;
    for (const char* var : {"ANDROID_HOME", "ANDROID_SDK_ROOT"})
        if (const char* v = std::getenv(var)) sdks.emplace_back(v);
    if (const char* home = std::getenv("HOME")) {
        sdks.push_back(fs::path(home) / "Android/Sdk");
        sdks.push_back(fs::path(home) / "Library/Android/sdk");
    }
    for (const fs::path& sdk : sdks) // SDK's adb first: another version restarts the server
        if (std::error_code ec; fs::exists(sdk / "platform-tools/adb", ec)) return (sdk / "platform-tools/adb").string();
    return "adb";
}

static int serverPort()
{
    const char* p = std::getenv("ANDROID_ADB_SERVER_PORT");
    const int port = p ? std::atoi(p) : 0;
    return port > 0 ? port : 5037;
}

static Socket tryConnect()
{
    const int fd = int(::socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) return {};
#ifdef SO_NOSIGPIPE
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(serverPort()));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        net::closeSocket(fd);
        return {};
    }
    return Socket(fd);
}

static Socket connectServer(const std::string& adb, std::string* error)
{
    if (Socket s = tryConnect(); s.valid()) return s;
#ifdef _WIN32
    const int rc = std::system(("\"" + adb + "\" start-server >NUL 2>&1").c_str());
#else
    std::string quoted = "'";
    for (char c : adb) quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    quoted += "'";
    const int rc = std::system((quoted + " start-server >/dev/null 2>&1").c_str());
#endif
    if (Socket s = tryConnect(); s.valid()) return s;
    *error = rc == 0 ? "The adb server did not start." : "Could not start the adb server with \"" + adb +
                                                             "\". Install the Android platform tools or set the adb "
                                                             "path in the connection's Advanced tab.";
    return {};
}

static std::string readMessage(Socket& s) // 4 hex digits of length, then the text
{
    char len[5] = {};
    if (!s.readAll(len, 4)) return {};
    std::string text(size_t(std::strtoul(len, nullptr, 16)), '\0');
    if (!text.empty() && !s.readAll(text.data(), text.size())) return {};
    return text;
}

static bool request(Socket& s, const std::string& service, std::string* error)
{
    char len[5];
    std::snprintf(len, sizeof len, "%04zx", service.size());
    char status[4];
    if (!s.writeAll(len, 4) || !s.writeAll(service.data(), service.size()) || !s.readAll(status, 4)) {
        *error = "The adb server closed the connection.";
        return false;
    }
    if (std::memcmp(status, "OKAY", 4) == 0) return true;
    *error = std::memcmp(status, "FAIL", 4) == 0 ? "adb: " + readMessage(s) : "adb: unexpected reply";
    return false;
}

Socket open(const std::string& adb, const std::string& serial, const std::string& service, std::string* error)
{
    Socket s = connectServer(adb, error);
    if (!s.valid()) return {};
    if (!request(s, serial.empty() ? "host:transport-any" : "host:transport:" + serial, error)) return {};
    if (!request(s, service, error)) return {};
    return s;
}

std::vector<std::string> devices(const std::string& adb, std::string* error)
{
    std::vector<std::string> result;
    Socket s = connectServer(adb, error);
    if (!s.valid() || !request(s, "host:devices", error)) return result;
    const std::string list = readMessage(s); // "<serial>\t<state>\n" per device
    for (size_t at = 0; at < list.size();) {
        size_t end = list.find('\n', at);
        if (end == std::string::npos) end = list.size();
        const std::string line = list.substr(at, end - at);
        const size_t tab = line.find('\t');
        if (tab != std::string::npos && line.substr(tab + 1) == "device") result.push_back(line.substr(0, tab));
        at = end + 1;
    }
    return result;
}

bool connectDevice(const std::string& adb, const std::string& hostPort, std::string* error)
{
    Socket s = connectServer(adb, error);
    if (!s.valid() || !request(s, "host:connect:" + hostPort, error)) return false;
    const std::string reply = readMessage(s);
    if (reply.starts_with("connected to") || reply.starts_with("already connected to")) return true;
    *error = "adb connect " + hostPort + ": " + reply;
    return false;
}

static void putLe32(std::string& out, uint32_t v)
{
    for (int i = 0; i < 4; ++i) out += char((v >> (8 * i)) & 0xFF);
}

bool push(const std::string& adb, const std::string& serial, const std::string& local, const std::string& remote,
          std::string* error)
{
    std::ifstream in(local, std::ios::binary);
    if (!in) {
        *error = "Cannot read " + local;
        return false;
    }
    Socket s = open(adb, serial, "sync:", error);
    if (!s.valid()) return false;
    auto lost = [&] {
        *error = "adb push: connection lost";
        return false;
    };
    const std::string spec = remote + "," + std::to_string(0100644); // path,st_mode (regular file, rw-r--r--)
    std::string head = "SEND";
    putLe32(head, uint32_t(spec.size()));
    head += spec;
    if (!s.writeAll(head.data(), head.size())) return lost();
    std::vector<char> chunk(64 * 1024); // the protocol's maximum DATA size
    while (in) {
        in.read(chunk.data(), std::streamsize(chunk.size()));
        const auto n = uint32_t(in.gcount());
        if (!n) break;
        std::string data = "DATA";
        putLe32(data, n);
        if (!s.writeAll(data.data(), data.size()) || !s.writeAll(chunk.data(), n)) return lost();
    }
    std::string done = "DONE";
    putLe32(done, uint32_t(std::time(nullptr)));
    char reply[8];
    if (!s.writeAll(done.data(), done.size()) || !s.readAll(reply, 8)) return lost();
    if (std::memcmp(reply, "OKAY", 4) != 0) {
        const uint32_t len = uint8_t(reply[4]) | uint8_t(reply[5]) << 8 | uint8_t(reply[6]) << 16 | uint32_t(uint8_t(reply[7])) << 24;
        std::string message(std::min<uint32_t>(len, 4096), '\0');
        s.readAll(message.data(), message.size());
        *error = "adb push " + remote + ": " + message;
        return false;
    }
    std::string quit = "QUIT";
    putLe32(quit, 0);
    s.writeAll(quit.data(), quit.size());
    return true;
}

} // namespace rdc::adb
