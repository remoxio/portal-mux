#pragma once

#include "core/Connection.h"
#include "core/Session.h"
#include "stream/Telnet.h"

#include <mutex>
#include <optional>
#include <thread>

namespace rdc {

// Telnet, raw TCP (Telnet with "raw") or a serial port (host = its device) into the terminal.
// shortcut: POSIX only, like the local terminal; add Winsock and Win32 COM ports with the Windows port
class StreamTerminalSession : public TerminalSession {
public:
    StreamTerminalSession(TerminalListener& listener, Connection connection);
    ~StreamTerminalSession() override { disconnect(); }

    void connect() override;
    void disconnect() override;
    void write(std::span<const std::byte> data) override;
    void resize(int cols, int rows) override;

private:
    void run(std::stop_token stop);
    int openSocket(std::string& error);
    int openSerial(std::string& error);
    void send(std::string_view bytes); // under mutex_

    TerminalListener& listener_;
    Connection connection_;
    std::jthread worker_;
    int wake_[2] = {-1, -1};
    std::mutex mutex_; // fd_, telnet_
    int fd_ = -1;
    std::optional<telnet::Protocol> telnet_;
    int cols_ = 80, rows_ = 24;
};

} // namespace rdc
