#pragma once

#include "core/Connection.h"
#include "core/Session.h"

#include <libssh/callbacks.h>

#include <functional>
#include <mutex>
#include <string>

typedef struct ssh_session_struct* ssh_session;

namespace rdc {

// One libssh session for terminal, SFTP and tunnels; open() and session() on the worker only
class SshConnection {
public:
    struct Params {
        std::string host, user, password, keyFile;
        int port = 22;
        std::string knownHostsFile; // written on trust; ~/.ssh/known_hosts is also accepted, read-only
        std::string jumpHost;       // ProxyJump list; libssh then connects (not interruptible)
        bool compression = false;
    };
    static Params fromConnection(const Connection& c, std::string knownHostsFile);

    SshConnection(SessionListener& listener, Params params);
    ~SshConnection();

    // error() is empty when the user cancelled
    bool open(const std::function<bool()>& cancelled);
    void abort(); // shuts the socket down so a blocking call on the worker returns

    ssh_session session() const { return session_; }
    const std::string& error() const { return error_; }
    std::string lastError() const;

private:
    struct Auth {
        ssh_session session;
        std::string user, host, password;
        bool passwordUsed = false;
    };
    bool connectSocket(const std::function<bool()>& cancelled);
    bool verifyHost(ssh_session session, const std::string& host);
    bool authenticate(Auth& a);
    bool authKeyFile(Auth& a);
    bool authKeyboardInteractive(Auth& a);
    bool authPassword(Auth& a);
    bool ask(const std::string& prompt, std::string& answer);
    static int jumpBeforeConnection(ssh_session s, void* self);
    static int jumpVerify(ssh_session s, void* self);
    static int jumpAuthenticate(ssh_session s, void* self);

    SessionListener& listener_;
    Params p_;
    ssh_session session_;
    std::string error_;
    bool cancelled_ = false; // the user dismissed a prompt: stop trying, report no error
    ssh_jump_callbacks_struct jumpCallbacks_{};
    std::mutex fdMutex_;
    int fd_ = -1;
};

} // namespace rdc
