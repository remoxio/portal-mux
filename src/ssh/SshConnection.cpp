#include "ssh/SshConnection.h"

#include <libssh/callbacks.h>
#include <libssh/libssh.h>

#include "core/Net.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace rdc {

namespace {

std::string expandHome(const std::string& path)
{
    if (path.rfind("~/", 0) == 0)
        if (const char* home = getenv("HOME")) return home + path.substr(1);
    return path;
}

// libssh may ask for a key passphrase through this; never prompt on a terminal.
int noPassphrase(const char*, char*, size_t, int, int, void*) { return -1; }

} // namespace

SshConnection::Params SshConnection::fromConnection(const Connection& c, std::string knownHostsFile)
{
    Params p;
    p.host = c.host;
    p.user = c.username;
    p.password = c.password;
    p.keyFile = c.option("keyFile");
    p.port = c.effectivePort();
    p.knownHostsFile = std::move(knownHostsFile);
    p.jumpHost = c.option("jumpHost");
    p.compression = c.flag("compression");
    return p;
}

SshConnection::SshConnection(SessionListener& listener, Params params)
    : listener_(listener), p_(std::move(params)), session_(ssh_new())
{
}

SshConnection::~SshConnection()
{
    {
        std::lock_guard lock(fdMutex_);
        fd_ = -1; // libssh closes it below; abort() must not touch a reused descriptor
    }
    if (session_) {
        ssh_disconnect(session_);
        ssh_free(session_);
    }
}

void SshConnection::abort()
{
    std::lock_guard lock(fdMutex_);
    if (fd_ >= 0) shutdown(fd_, SHUT_RDWR);
}

std::string SshConnection::lastError() const { return session_ ? ssh_get_error(session_) : "out of memory"; }

bool SshConnection::open(const std::function<bool()>& cancelled)
{
    if (!session_) {
        error_ = "Out of memory.";
        return false;
    }
    ssh_options_set(session_, SSH_OPTIONS_HOST, p_.host.c_str());
    if (!p_.user.empty()) ssh_options_set(session_, SSH_OPTIONS_USER, p_.user.c_str());
    ssh_options_set(session_, SSH_OPTIONS_PORT, &p_.port);
    ssh_options_parse_config(session_, nullptr);
    if (!p_.jumpHost.empty()) {
        ssh_options_set(session_, SSH_OPTIONS_PROXYJUMP, p_.jumpHost.c_str());
        jumpCallbacks_ = {this, jumpBeforeConnection, jumpVerify, jumpAuthenticate};
        for (size_t i = 0, hops = size_t(std::count(p_.jumpHost.begin(), p_.jumpHost.end(), ',')) + 1; i < hops; ++i)
            ssh_options_set(session_, SSH_OPTIONS_PROXYJUMP_CB_LIST_APPEND, &jumpCallbacks_);
    }
    ssh_options_set(session_, SSH_OPTIONS_COMPRESSION, p_.compression ? "yes" : "no");
    ssh_options_set(session_, SSH_OPTIONS_KNOWNHOSTS, p_.knownHostsFile.c_str());
    const std::string userKnownHosts = expandHome("~/.ssh/known_hosts");
    ssh_options_set(session_, SSH_OPTIONS_GLOBAL_KNOWNHOSTS, userKnownHosts.c_str());
    char* user = nullptr;
    if (ssh_options_get(session_, SSH_OPTIONS_USER, &user) == SSH_OK) {
        p_.user = user;
        ssh_string_free_char(user);
    }

    // shortcut: with jump hosts libssh connects itself, so closing the tab waits for its timeout
    if (p_.jumpHost.empty() && !connectSocket(cancelled)) return false;
    if (ssh_connect(session_) != SSH_OK) {
        if (cancelled() || cancelled_) error_.clear();
        else error_ = error_.empty() ? "SSH connection failed: " + lastError() : "Jump host: " + error_;
        return false;
    }
    if (!verifyHost(session_, p_.host + ":" + std::to_string(p_.port))) {
        if (cancelled_) error_.clear();
        return false;
    }
    Auth a{session_, p_.user, p_.host, p_.password};
    return authenticate(a);
}

// our own non-blocking connect: closing a tab can interrupt it, libssh's cannot be
bool SshConnection::connectSocket(const std::function<bool()>& cancelled)
{
    char* host = nullptr;
    unsigned int port = 0;
    ssh_options_get(session_, SSH_OPTIONS_HOST, &host);
    ssh_options_get_port(session_, &port);
    const std::string hostName = host ? host : p_.host;
    ssh_string_free_char(host);

    addrinfo hints{}, *addrs = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (int rc = getaddrinfo(hostName.c_str(), std::to_string(port).c_str(), &hints, &addrs); rc != 0) {
        error_ = "Cannot resolve " + hostName + ": " + gai_strerror(rc);
        return false;
    }
    int fd = -1;
    std::string why = "no address";
    for (addrinfo* a = addrs; a && fd < 0 && !cancelled(); a = a->ai_next) {
        fd = net::openSocket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        {
            std::lock_guard lock(fdMutex_);
            fd_ = fd;
        }
        net::setNonBlocking(fd, true);
        int rc = ::connect(fd, a->ai_addr, int(a->ai_addrlen));
        int soError = rc < 0 ? net::lastError() : 0;
        if (rc < 0 && net::connectPending(soError)) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            pollfd pfd{fd, POLLOUT, 0};
            while ((rc = poll(&pfd, 1, 100)) == 0 && !cancelled() && std::chrono::steady_clock::now() < deadline) {}
            soError = rc > 0 ? net::socketError(fd) : net::timedOut;
            rc = soError ? -1 : 0;
        }
        if (rc == 0) {
            net::setNonBlocking(fd, false);
            break;
        }
        why = net::errorText(soError);
        std::lock_guard lock(fdMutex_);
        fd_ = -1;
        net::closeSocket(fd);
        fd = -1;
    }
    freeaddrinfo(addrs);
    if (fd < 0) {
        error_ = cancelled() ? "" : "Cannot connect to " + hostName + ": " + why;
        return false;
    }
    socket_t sock = fd; // 64-bit on Windows
    ssh_options_set(session_, SSH_OPTIONS_FD, &sock); // libssh owns and closes it from here on
    return true;
}

bool SshConnection::verifyHost(ssh_session session, const std::string& host)
{
    ssh_key key = nullptr;
    if (ssh_get_server_publickey(session, &key) != SSH_OK) {
        error_ = "No host key: " + std::string(ssh_get_error(session));
        return false;
    }
    unsigned char* hash = nullptr;
    size_t hashLen = 0;
    std::string details = "Host: " + host + "\nKey type: " + ssh_key_type_to_char(ssh_key_type(key));
    if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hashLen) == 0) {
        char* fingerprint = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hashLen);
        details += std::string("\nFingerprint: ") + (fingerprint ? fingerprint : "?");
        ssh_string_free_char(fingerprint);
        ssh_clean_pubkey_hash(&hash);
    }
    ssh_key_free(key);

    switch (ssh_session_is_known_server(session)) {
    case SSH_KNOWN_HOSTS_OK:
        return true;
    case SSH_KNOWN_HOSTS_CHANGED:
    case SSH_KNOWN_HOSTS_OTHER:
        // Like OpenSSH: never offer to accept a changed key from inside the connection.
        error_ = "The SSH host key of " + host + " has CHANGED. Someone may be intercepting the connection.\n\n" +
                 details + "\n\nIf the change is expected, remove the old key from " + p_.knownHostsFile +
                 " or ~/.ssh/known_hosts.";
        return false;
    case SSH_KNOWN_HOSTS_UNKNOWN:
    case SSH_KNOWN_HOSTS_NOT_FOUND:
        if (!listener_.askTrust("The SSH host key of " + host + " is not known. Trust it?", details)) {
            cancelled_ = true;
            return false;
        }
        if (ssh_session_update_known_hosts(session) != SSH_OK) {
            error_ = "Could not save the host key: " + std::string(ssh_get_error(session));
            return false;
        }
        return true;
    default:
        error_ = "Host key check failed: " + std::string(ssh_get_error(session));
        return false;
    }
}

bool SshConnection::ask(const std::string& prompt, std::string& answer)
{
    Credentials c;
    c.needsUser = false;
    if (!listener_.askCredentials(prompt, c)) {
        cancelled_ = true;
        return false;
    }
    answer = c.password;
    return true;
}

bool SshConnection::authenticate(Auth& a)
{
    int rc = ssh_userauth_none(a.session, nullptr);
    if (rc == SSH_AUTH_SUCCESS) return true;
    if (rc == SSH_AUTH_ERROR) {
        error_ = "Authentication failed: " + std::string(ssh_get_error(a.session));
        return false;
    }
    const int methods = ssh_userauth_list(a.session, nullptr);
    if (methods & SSH_AUTH_METHOD_PUBLICKEY) {
        if (!p_.keyFile.empty() && authKeyFile(a)) return true;
        if (!cancelled_ && getenv("SSH_AUTH_SOCK") && ssh_userauth_agent(a.session, nullptr) == SSH_AUTH_SUCCESS) return true;
        if (!cancelled_ && p_.keyFile.empty() && ssh_userauth_publickey_auto(a.session, nullptr, nullptr) == SSH_AUTH_SUCCESS)
            return true;
    }
    if (!cancelled_ && error_.empty() && (methods & SSH_AUTH_METHOD_INTERACTIVE) && authKeyboardInteractive(a)) return true;
    if (!cancelled_ && error_.empty() && (methods & SSH_AUTH_METHOD_PASSWORD) && authPassword(a)) return true;

    if (cancelled_) {
        error_.clear();
    } else if (ssh_get_error_code(a.session) == SSH_FATAL) {
        error_ = "SSH connection lost: " + std::string(ssh_get_error(a.session));
    } else if (error_.empty()) {
        std::string offered;
        if (methods & SSH_AUTH_METHOD_PUBLICKEY) offered += " publickey";
        if (methods & SSH_AUTH_METHOD_INTERACTIVE) offered += " keyboard-interactive";
        if (methods & SSH_AUTH_METHOD_PASSWORD) offered += " password";
        error_ = "Authentication failed for " + a.user + "@" + a.host + " (server accepts:" + offered + ").";
    }
    return false;
}

bool SshConnection::authKeyFile(Auth& a)
{
    const std::string path = expandHome(p_.keyFile);
    ssh_key key = nullptr;
    int rc = ssh_pki_import_privkey_file(path.c_str(), nullptr, noPassphrase, nullptr, &key);
    for (int tries = 0; rc != SSH_OK && tries < 3; ++tries) {
        std::string passphrase;
        if (!ask("Passphrase for " + path, passphrase)) return false;
        rc = ssh_pki_import_privkey_file(path.c_str(), passphrase.c_str(), noPassphrase, nullptr, &key);
    }
    if (rc != SSH_OK) {
        error_ = "Could not load the private key " + path + ".";
        return false;
    }
    rc = ssh_userauth_publickey(a.session, nullptr, key);
    ssh_key_free(key);
    return rc == SSH_AUTH_SUCCESS;
}

bool SshConnection::authKeyboardInteractive(Auth& a)
{
    for (int round = 0; round < 10; ++round) {
        const int rc = ssh_userauth_kbdint(a.session, nullptr, nullptr);
        if (rc == SSH_AUTH_SUCCESS) return true;
        if (rc != SSH_AUTH_INFO) return false;
        for (int i = 0, n = ssh_userauth_kbdint_getnprompts(a.session); i < n; ++i) {
            char echo = 0;
            const char* prompt = ssh_userauth_kbdint_getprompt(a.session, unsigned(i), &echo);
            std::string answer;
            if (!echo && !a.password.empty() && !a.passwordUsed) {
                answer = a.password;
                a.passwordUsed = true;
            } else if (!ask(prompt && *prompt ? prompt : "Password:", answer)) {
                return false;
            }
            if (ssh_userauth_kbdint_setanswer(a.session, unsigned(i), answer.c_str()) < 0) return false;
        }
    }
    return false;
}

bool SshConnection::authPassword(Auth& a)
{
    for (int tries = 0; tries < 3; ++tries) {
        std::string password;
        if (!a.password.empty() && !a.passwordUsed) {
            password = a.password;
            a.passwordUsed = true;
        } else if (!ask("Password for " + a.user + "@" + a.host, password)) {
            return false;
        }
        const int rc = ssh_userauth_password(a.session, nullptr, password.c_str());
        if (rc == SSH_AUTH_SUCCESS) return true;
        if (rc == SSH_AUTH_ERROR) return false;
    }
    return false;
}

// ---- jump hosts (libssh ProxyJump runs these on its own thread while ssh_connect waits) ----

static std::string option(ssh_session s, ssh_options_e key)
{
    char* value = nullptr;
    std::string result;
    if (ssh_options_get(s, key, &value) == SSH_OK && value) result = value;
    ssh_string_free_char(value);
    return result;
}

int SshConnection::jumpBeforeConnection(ssh_session s, void* self)
{
    auto* c = static_cast<SshConnection*>(self);
    const std::string userKnownHosts = expandHome("~/.ssh/known_hosts");
    ssh_options_set(s, SSH_OPTIONS_KNOWNHOSTS, c->p_.knownHostsFile.c_str());
    ssh_options_set(s, SSH_OPTIONS_GLOBAL_KNOWNHOSTS, userKnownHosts.c_str());
    return SSH_OK;
}

int SshConnection::jumpVerify(ssh_session s, void* self)
{
    return static_cast<SshConnection*>(self)->verifyHost(s, option(s, SSH_OPTIONS_HOST) + " (jump host)") ? SSH_OK : SSH_ERROR;
}

int SshConnection::jumpAuthenticate(ssh_session s, void* self)
{
    Auth a{s, option(s, SSH_OPTIONS_USER), option(s, SSH_OPTIONS_HOST), {}}; // the saved password is the target's
    return static_cast<SshConnection*>(self)->authenticate(a) ? SSH_OK : SSH_ERROR;
}

} // namespace rdc
