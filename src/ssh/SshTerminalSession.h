#pragma once

#include "core/Connection.h"
#include "core/Session.h"
#include "core/TaskQueue.h"
#include "ssh/SshConnection.h"

#include <memory>
#include <thread>

typedef struct ssh_channel_struct* ssh_channel;

namespace rdc {

class SshTerminalSession : public TerminalSession {
public:
    SshTerminalSession(TerminalListener& listener, Connection connection, std::string knownHostsFile);
    ~SshTerminalSession() override { disconnect(); }

    void connect() override;
    void disconnect() override;
    void write(std::span<const std::byte> data) override;
    void resize(int cols, int rows) override;

private:
    void run(std::stop_token stop);

    TerminalListener& listener_;
    Connection connection_;
    std::string knownHostsFile_;
    TaskQueue tasks_;
    std::unique_ptr<SshConnection> ssh_;
    std::jthread worker_;
    ssh_channel channel_ = nullptr;
    int cols_ = 80, rows_ = 24;
};

} // namespace rdc
