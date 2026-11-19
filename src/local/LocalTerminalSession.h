#pragma once

#include "core/Connection.h"
#include "core/Session.h"

#include <atomic>
#include <thread>

namespace rdc {

// A login shell or "command" on a pty; shortcut: POSIX only, Windows needs ConPTY
class LocalTerminalSession : public TerminalSession {
public:
    LocalTerminalSession(TerminalListener& listener, Connection connection);
    ~LocalTerminalSession() override { disconnect(); }

    void connect() override;
    void disconnect() override;
    void write(std::span<const std::byte> data) override;
    void resize(int cols, int rows) override;

private:
    void run(std::stop_token stop);

    TerminalListener& listener_;
    Connection connection_;
    std::jthread worker_;
    int master_ = -1;
    int wake_[2] = {-1, -1}; // wakes the reader
    std::atomic<int> child_{-1};
    int cols_ = 80, rows_ = 24;
};

} // namespace rdc
