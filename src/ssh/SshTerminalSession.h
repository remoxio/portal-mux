#pragma once

#include "core/Connection.h"
#include "core/Session.h"
#include "core/TaskQueue.h"
#include "ssh/SshConnection.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

typedef struct ssh_channel_struct* ssh_channel;
typedef struct sftp_session_struct* sftp_session;

namespace rdc {

class SshTerminalSession : public TerminalSession {
public:
    SshTerminalSession(TerminalListener& listener, Connection connection, std::string knownHostsFile);
    ~SshTerminalSession() override { disconnect(); }

    void connect() override;
    void disconnect() override;
    void write(std::span<const std::byte> data) override;
    void resize(int cols, int rows) override;
    bool canUpload() const override { return true; }
    void upload(std::string data, std::string extension, std::function<void(std::string, std::string)> done) override;
    // a public key line into ~/.ssh/authorized_keys; done(error, added): false if it was there
    void installKey(std::string key, std::function<void(std::string, bool)> done);

private:
    void run(std::stop_token stop);
    std::string store(const std::string& data, const std::string& extension, std::string& error);
    std::string sftpHome(std::string& error); // opens SFTP once
    std::string sftpError(const std::string& what) const;
    bool addAuthorizedKey(const std::string& key, std::string& error);

    TerminalListener& listener_;
    Connection connection_;
    std::string knownHostsFile_;
    TaskQueue tasks_;
    std::unique_ptr<SshConnection> ssh_;
    std::jthread worker_;
    ssh_channel channel_ = nullptr;
    int cols_ = 80, rows_ = 24;
    sftp_session sftp_ = nullptr;
    std::vector<std::string> uploads_; // removed when the session ends
    std::atomic<bool> uploaded_ = false, finished_ = false;
};

} // namespace rdc
