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
        unsigned permissions = 0; // 07777
    };
    using Done = std::function<void(std::string error)>;

    SftpClient(SessionListener& listener, SshConnection::Params params);
    ~SftpClient() { stop(); }

    void start(Done opened);
    void setPassword(std::string password) { params_.password = std::move(password); } // before start()
    void stop();
    std::string home() const { return home_; }

    void list(std::string path, std::function<void(std::vector<Entry>, std::string error)> done);
    void download(std::string remote, std::string local, Done done);
    void upload(std::string local, std::string remote, Done done);
    void rename(std::string from, std::string to, Done done);
    void remove(std::string path, Done done); // a folder with everything in it
    void makeFolder(std::string path, Done done);
    void makeFile(std::string path, Done done);
    void setPermissions(std::string path, unsigned mode, Done done);

private:
    void post(std::function<void()> task);
    void run(std::stop_token stop, Done opened);
    std::string error(const std::string& what) const;
    void call(std::function<int()> op, std::string failure, Done done); // op returns 0 on success
    int removeTree(const std::string& path);

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
