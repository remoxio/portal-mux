#pragma once

#include "ssh/SshConnection.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

typedef struct sftp_session_struct* sftp_session;

namespace rdc {

// Every callback runs on the worker; the caller marshals.
class SftpClient {
public:
    struct Entry {
        std::string name;
        uint64_t size = 0;
        int64_t mtime = 0;
        bool directory = false;
    };
    using Done = std::function<void(std::string error)>;

    SftpClient(SessionListener& listener, SshConnection::Params params);
    ~SftpClient() { stop(); }

    void start(Done opened);
    void stop();
    std::string home() const { return home_; }

    void list(std::string path, std::function<void(std::vector<Entry>, std::string error)> done);
    void download(std::string remote, std::string local, Done done);
    void upload(std::string local, std::string remote, Done done);

private:
    void post(std::function<void()> task);
    void run(std::stop_token stop, Done opened);
    std::string error(const std::string& what) const;

    SessionListener& listener_;
    SshConnection::Params params_;
    std::unique_ptr<SshConnection> ssh_;
    sftp_session sftp_ = nullptr;
    std::string home_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::vector<std::function<void()>> tasks_;
    std::jthread worker_;
};

} // namespace rdc
