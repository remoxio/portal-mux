#include "moonlight/GameStream.h"

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

namespace fs = std::filesystem;

namespace rdc::gamestream {
namespace {

template <class T, auto Free>
struct Deleter {
    void operator()(T* p) const { Free(p); }
};
using X509Ptr = std::unique_ptr<X509, Deleter<X509, X509_free>>;
using KeyPtr = std::unique_ptr<EVP_PKEY, Deleter<EVP_PKEY, EVP_PKEY_free>>;
using BioPtr = std::unique_ptr<BIO, Deleter<BIO, BIO_free_all>>;
using SslCtxPtr = std::unique_ptr<SSL_CTX, Deleter<SSL_CTX, SSL_CTX_free>>;
using SslPtr = std::unique_ptr<SSL, Deleter<SSL, SSL_free>>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, Deleter<EVP_MD_CTX, EVP_MD_CTX_free>>;
using CipherPtr = std::unique_ptr<EVP_CIPHER_CTX, Deleter<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free>>;

X509Ptr readCert(const std::string& pem)
{
    BioPtr bio(BIO_new_mem_buf(pem.data(), int(pem.size())));
    return X509Ptr(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
}

KeyPtr readKey(const std::string& pem)
{
    BioPtr bio(BIO_new_mem_buf(pem.data(), int(pem.size())));
    return KeyPtr(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
}

std::string bioText(BIO* bio)
{
    char* data = nullptr;
    const long n = BIO_get_mem_data(bio, &data);
    return std::string(data, size_t(n));
}

std::string hex(const std::string& bytes)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : bytes) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

std::string unhex(const std::string& text)
{
    std::string out;
    for (size_t i = 0; i + 1 < text.size(); i += 2) out += char(std::strtoul(text.substr(i, 2).c_str(), nullptr, 16));
    return out;
}

std::string randomBytes(size_t n)
{
    std::string s(n, '\0');
    RAND_bytes(reinterpret_cast<unsigned char*>(s.data()), int(n));
    return s;
}

std::string sslError()
{
    char text[256] = "unknown error";
    if (const unsigned long e = ERR_get_error()) ERR_error_string_n(e, text, sizeof text);
    return text;
}

std::string certSignature(const X509* cert)
{
    const ASN1_BIT_STRING* sig = nullptr;
    X509_get0_signature(&sig, nullptr, cert);
    return sig ? std::string(reinterpret_cast<const char*>(ASN1_STRING_get0_data(sig)), size_t(ASN1_STRING_length(sig))) : std::string();
}

std::string readFile(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

int connectTcp(const std::string& host, int port, int timeoutSeconds, Cancel& cancel, std::string* error)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addrs = nullptr;
    if (int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &addrs); rc != 0) {
        *error = "Cannot resolve " + host + ": " + gai_strerror(rc);
        return -1;
    }
    int fd = -1;
    for (addrinfo* a = addrs; a && fd < 0 && !cancel.cancelled; a = a->ai_next) {
        fd = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        cancel.fd = fd;
        const int flags = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = ::connect(fd, a->ai_addr, a->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            int soError = ETIMEDOUT;
            socklen_t len = sizeof soError;
            if (poll(&p, 1, 10'000) == 1) getsockopt(fd, SOL_SOCKET, SO_ERROR, &soError, &len);
            rc = soError ? -1 : 0;
            errno = soError;
        }
        if (rc != 0) {
            *error = "Cannot connect to " + host + ":" + std::to_string(port) + ": " + std::strerror(errno);
            cancel.fd = -1;
            ::close(fd);
            fd = -1;
            continue;
        }
        fcntl(fd, F_SETFL, flags);
        timeval tv{timeoutSeconds, 0}; // 0 = no timeout
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    }
    freeaddrinfo(addrs);
    if (fd >= 0 && cancel.cancelled) {
        cancel.fd = -1;
        ::close(fd);
        fd = -1;
    }
    return fd;
}

std::string httpBody(const std::string& raw, int* status)
{
    *status = 0;
    if (raw.compare(0, 5, "HTTP/") != 0) return {};
    const size_t sp = raw.find(' ');
    *status = sp == std::string::npos ? 0 : std::atoi(raw.c_str() + sp + 1);
    const size_t end = raw.find("\r\n\r\n");
    if (end == std::string::npos) return {};
    std::string headers = raw.substr(0, end);
    for (char& c : headers) c = char(std::tolower(static_cast<unsigned char>(c)));
    std::string body = raw.substr(end + 4);
    if (headers.find("transfer-encoding: chunked") != std::string::npos) {
        std::string out;
        for (size_t at = 0; at < body.size();) {
            const size_t eol = body.find("\r\n", at);
            if (eol == std::string::npos) break;
            const size_t n = std::strtoul(body.substr(at, eol - at).c_str(), nullptr, 16);
            if (n == 0) break;
            out += body.substr(eol + 2, n);
            at = eol + 2 + n + 2;
        }
        return out;
    }
    if (const size_t cl = headers.find("content-length:"); cl != std::string::npos) {
        const size_t n = std::strtoul(headers.c_str() + cl + 15, nullptr, 10);
        if (body.size() > n) body.resize(n);
    }
    return body;
}

std::string unescapeXml(std::string s)
{
    static const std::pair<const char*, char> entities[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'}};
    for (auto [entity, c] : entities)
        for (size_t at; (at = s.find(entity)) != std::string::npos;) s.replace(at, std::strlen(entity), 1, c);
    return s;
}

} // namespace

std::string toHex(const std::string& bytes) { return hex(bytes); }

void Cancel::cancel()
{
    cancelled = true;
    if (const int f = fd; f >= 0) ::shutdown(f, SHUT_RDWR);
}

std::string xmlValue(const std::string& xml, const std::string& tag)
{
    const std::string open = "<" + tag + ">", close = "</" + tag + ">";
    const size_t start = xml.find(open);
    if (start == std::string::npos) return {};
    const size_t end = xml.find(close, start + open.size());
    if (end == std::string::npos) return {};
    return unescapeXml(xml.substr(start + open.size(), end - start - open.size()));
}

int xmlStatus(const std::string& xml, std::string* message)
{
    const size_t root = xml.find("<root");
    if (root == std::string::npos) return -1;
    const std::string head = xml.substr(root, xml.find('>', root) - root);
    auto attribute = [&](const char* name) {
        const std::string key = std::string(name) + "=\"";
        const size_t at = head.find(key);
        if (at == std::string::npos) return std::string();
        return unescapeXml(head.substr(at + key.size(), head.find('"', at + key.size()) - at - key.size()));
    };
    if (message) *message = attribute("status_message");
    return int(std::strtoul(attribute("status_code").c_str(), nullptr, 10)); // GFE may send 0xFFFFFFFF: unsigned
}

bool loadIdentity(const std::string& dir, Identity& id, std::string* error)
{
    id.certPem = readFile(fs::path(dir) / "client.pem");
    id.keyPem = readFile(fs::path(dir) / "key.pem");
    id.uniqueId = readFile(fs::path(dir) / "uniqueid");
    if (id.uniqueId.size() == 16 && readCert(id.certPem) && readKey(id.keyPem)) return true;

    KeyPtr key(EVP_RSA_gen(2048));
    X509Ptr cert(X509_new());
    if (!key || !cert) {
        *error = "Could not create the client certificate: " + sslError();
        return false;
    }
    X509_set_version(cert.get(), 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 0);
    X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert.get()), 60L * 60 * 24 * 365 * 20);
    X509_set_pubkey(cert.get(), key.get());
    std::unique_ptr<X509_NAME, Deleter<X509_NAME, X509_NAME_free>> name(X509_NAME_new()); // OpenSSL 4: subject is const
    X509_NAME_add_entry_by_txt(name.get(), "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("NVIDIA GameStream Client"),
                               -1, -1, 0);
    X509_set_subject_name(cert.get(), name.get());
    X509_set_issuer_name(cert.get(), name.get());
    if (!X509_sign(cert.get(), key.get(), EVP_sha256())) {
        *error = "Could not sign the client certificate: " + sslError();
        return false;
    }
    BioPtr certBio(BIO_new(BIO_s_mem())), keyBio(BIO_new(BIO_s_mem()));
    PEM_write_bio_X509(certBio.get(), cert.get());
    PEM_write_bio_PrivateKey(keyBio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr);
    id.certPem = bioText(certBio.get());
    id.keyPem = bioText(keyBio.get());
    uint64_t uid = 0;
    RAND_bytes(reinterpret_cast<unsigned char*>(&uid), sizeof uid);
    char text[17];
    std::snprintf(text, sizeof text, "%016llX", static_cast<unsigned long long>(uid));
    id.uniqueId = text;

    std::error_code ec;
    fs::create_directories(dir, ec);
    const fs::path keyPath = fs::path(dir) / "key.pem";
    std::ofstream(keyPath, std::ios::binary) << id.keyPem;
    fs::permissions(keyPath, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    std::ofstream(fs::path(dir) / "client.pem", std::ios::binary) << id.certPem;
    std::ofstream(fs::path(dir) / "uniqueid", std::ios::binary) << id.uniqueId;
    if (readFile(keyPath) != id.keyPem) {
        *error = "Could not save the client certificate in " + dir;
        return false;
    }
    return true;
}

// POST when content is given
static bool exchange(const Host& host, const Identity& id, bool https, const std::string& verb, const std::string& query,
                     const std::string* content, int timeoutSeconds, Cancel& cancel, std::string& body, int& status,
                     std::string* error)
{
    const int port = https ? host.httpsPort : host.httpPort;
    const std::string path = "/" + verb + "?uniqueid=" + id.uniqueId + "&uuid=" + hex(randomBytes(16)) +
                             (query.empty() ? "" : "&" + query);
    const bool v6 = host.address.find(':') != std::string::npos;
    const std::string req = (content ? "POST " : "GET ") + path + " HTTP/1.1\r\nHost: " +
                            (v6 ? "[" + host.address + "]" : host.address) + ":" + std::to_string(port) +
                            "\r\nUser-Agent: portal-mux\r\n" +
                            (content ? "Content-Type: text/plain; charset=utf-8\r\nContent-Length: " +
                                           std::to_string(content->size()) + "\r\n"
                                     : std::string()) +
                            "Connection: close\r\n\r\n" + (content ? *content : std::string());
    const int fd = connectTcp(host.address, port, timeoutSeconds, cancel, error);
    if (fd < 0) return false;
    struct Closer {
        int fd;
        Cancel& cancel;
        ~Closer()
        {
            cancel.fd = -1;
            ::close(fd);
        }
    } closer{fd, cancel};

    auto failed = [&](std::string why) {
        *error = std::move(why);
        return false;
    };
    std::string raw;
    char buf[16384];
    if (!https) {
        for (size_t at = 0; at < req.size();) {
            const ssize_t n = ::send(fd, req.data() + at, req.size() - at, MSG_NOSIGNAL);
            if (n <= 0) return failed("The host closed the connection.");
            at += size_t(n);
        }
        for (ssize_t n; (n = ::recv(fd, buf, sizeof buf, 0)) > 0;) raw.append(buf, size_t(n));
    } else {
        SslCtxPtr ctx(SSL_CTX_new(TLS_client_method()));
        X509Ptr cert = readCert(id.certPem);
        KeyPtr key = readKey(id.keyPem);
        if (!ctx || SSL_CTX_use_certificate(ctx.get(), cert.get()) != 1 || SSL_CTX_use_PrivateKey(ctx.get(), key.get()) != 1)
            return failed("TLS setup failed: " + sslError());
        SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_NONE, nullptr); // the pinned certificate is compared below
        SslPtr ssl(SSL_new(ctx.get()));
        SSL_set_fd(ssl.get(), fd);
        if (SSL_connect(ssl.get()) != 1) return failed("TLS connection to the host failed: " + sslError());
        X509Ptr peer(SSL_get1_peer_certificate(ssl.get()));
        X509Ptr pinned = readCert(host.serverCertPem);
        if (!peer || !pinned || X509_cmp(peer.get(), pinned.get()) != 0)
            return failed("The host's certificate is not the one it had when paired.");
        if (SSL_write(ssl.get(), req.data(), int(req.size())) != int(req.size()))
            return failed("The host closed the connection.");
        for (int n; (n = SSL_read(ssl.get(), buf, sizeof buf)) > 0;) raw.append(buf, size_t(n));
    }
    if (cancel.cancelled) return failed("Cancelled.");
    if (raw.empty()) return failed("The host did not answer " + verb + " (timed out or closed).");
    body = httpBody(raw, &status);
    return true;
}

std::string request(const Host& host, const Identity& id, bool https, const std::string& verb, const std::string& query,
                    int timeoutSeconds, Cancel& cancel, std::string* error)
{
    std::string body;
    int status = 0;
    if (!exchange(host, id, https, verb, query, nullptr, timeoutSeconds, cancel, body, status, error)) return {};
    std::string message;
    const int xmlCode = xmlStatus(body, &message);
    if (xmlCode != 200) {
        *error = (message.empty() ? "The host refused " + verb : message) + " (status " +
                 std::to_string(xmlCode == -1 ? status : xmlCode) + ")";
        return {};
    }
    return body;
}

bool serverInfo(const Host& host, const Identity& id, bool https, ServerInfo& info, Cancel& cancel, std::string* error)
{
    const std::string xml = request(host, id, https, "serverinfo", "", 10, cancel, error);
    if (xml.empty()) return false;
    info.hostname = xmlValue(xml, "hostname");
    info.appVersion = xmlValue(xml, "appversion");
    info.gfeVersion = xmlValue(xml, "GfeVersion");
    if (const int p = std::atoi(xmlValue(xml, "HttpsPort").c_str()); p > 0) info.httpsPort = p;
    info.codecModes = std::atoi(xmlValue(xml, "ServerCodecModeSupport").c_str());
    info.currentGame = std::atoi(xmlValue(xml, "currentgame").c_str());
    info.paired = xmlValue(xml, "PairStatus") == "1";
    info.sunshine = xmlValue(xml, "state").starts_with("SUNSHINE");
    info.apolloPermissions = uint32_t(std::strtoul(xmlValue(xml, "Permission").c_str(), nullptr, 10));
    return true;
}

bool clipboard(const Host& host, const Identity& id, const std::string* send, std::string& received, Cancel& cancel,
               std::string* error)
{
    int status = 0;
    if (!exchange(host, id, true, "actions/clipboard", "type=text", send, 10, cancel, received, status, error)) return false;
    if (status == 200) return true;
    *error = "The host refused the clipboard (status " + std::to_string(status) + ")";
    return false;
}

std::vector<App> appList(const Host& host, const Identity& id, Cancel& cancel, std::string* error)
{
    std::vector<App> apps;
    const std::string xml = request(host, id, true, "applist", "", 10, cancel, error);
    for (size_t at = 0; (at = xml.find("<App>", at)) != std::string::npos;) {
        const size_t end = xml.find("</App>", at);
        const std::string block = xml.substr(at, end == std::string::npos ? std::string::npos : end - at);
        apps.push_back({std::atoi(xmlValue(block, "ID").c_str()), xmlValue(block, "AppTitle")});
        at += 5;
    }
    return apps;
}

bool pair(const Host& host, const Identity& id, const std::string& pin, const std::string& appVersion,
          std::string& serverCertPem, Cancel& cancel, std::string* error)
{
    const bool gen7 = std::atoi(appVersion.c_str()) >= 7; // GFE 3+ and Sunshine: SHA-256; older: SHA-1
    const EVP_MD* md = gen7 ? EVP_sha256() : EVP_sha1();
    const size_t hashLength = gen7 ? 32 : 20;
    auto hash = [&](const std::string& data) {
        unsigned char out[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        EVP_Digest(data.data(), data.size(), out, &len, md, nullptr);
        return std::string(reinterpret_cast<char*>(out), len);
    };
    const std::string salt = randomBytes(16);
    const std::string aesKey = hash(salt + pin).substr(0, 16);
    auto aes = [&](const std::string& in, bool encrypt) {
        CipherPtr c(EVP_CIPHER_CTX_new());
        std::string out(in.size() + 16, '\0');
        int len = 0, tail = 0;
        EVP_CipherInit_ex(c.get(), EVP_aes_128_ecb(), nullptr, reinterpret_cast<const unsigned char*>(aesKey.data()), nullptr,
                          encrypt);
        EVP_CIPHER_CTX_set_padding(c.get(), 0);
        EVP_CipherUpdate(c.get(), reinterpret_cast<unsigned char*>(out.data()), &len,
                         reinterpret_cast<const unsigned char*>(in.data()), int(in.size()));
        EVP_CipherFinal_ex(c.get(), reinterpret_cast<unsigned char*>(out.data()) + len, &tail);
        out.resize(size_t(len + tail));
        return out;
    };
    auto fail = [&](const std::string& why) {
        Cancel ignored;
        std::string e;
        request(host, id, false, "unpair", "", 5, ignored, &e);
        *error = why;
        return false;
    };
    auto step = [&](const std::string& query, bool https, int timeout) {
        Host h = host;
        h.serverCertPem = serverCertPem;
        std::string xml = request(h, id, https, "pair", "devicename=portal-mux&updateState=1&" + query, timeout, cancel, error);
        if (!xml.empty() && xmlValue(xml, "paired") != "1") {
            *error = "The host refused to pair.";
            xml.clear();
        }
        return xml;
    };

    // 1. certificates; the host waits for the PIN
    std::string xml = step("phrase=getservercert&salt=" + hex(salt) + "&clientcert=" + hex(id.certPem), false, 0);
    if (xml.empty()) return false;
    serverCertPem = unhex(xmlValue(xml, "plaincert"));
    X509Ptr serverCert = readCert(serverCertPem);
    if (!serverCert) return fail("The host is already pairing with another device; try again.");
    X509Ptr clientCert = readCert(id.certPem);
    KeyPtr clientKey = readKey(id.keyPem);

    const std::string challenge = randomBytes(16);
    if ((xml = step("clientchallenge=" + hex(aes(challenge, true)), false, 10)).empty()) return fail(*error);
    const std::string response = aes(unhex(xmlValue(xml, "challengeresponse")), false);
    if (response.size() < hashLength + 16) return fail("The host's pairing answer is malformed.");
    const std::string serverResponse = response.substr(0, hashLength), serverChallenge = response.substr(hashLength, 16);

    const std::string clientSecret = randomBytes(16);
    std::string answer = hash(serverChallenge + certSignature(clientCert.get()) + clientSecret);
    answer.resize(32, '\0');
    if ((xml = step("serverchallengeresp=" + hex(aes(answer, true)), false, 10)).empty()) return fail(*error);
    const std::string pairingSecret = unhex(xmlValue(xml, "pairingsecret"));
    if (pairingSecret.size() <= 16) return fail("The host's pairing secret is malformed.");
    const std::string serverSecret = pairingSecret.substr(0, 16), serverSignature = pairingSecret.substr(16);
    MdCtxPtr verify(EVP_MD_CTX_new());
    const bool signedByHost =
        EVP_DigestVerifyInit(verify.get(), nullptr, EVP_sha256(), nullptr, X509_get0_pubkey(serverCert.get())) == 1 &&
        EVP_DigestVerify(verify.get(), reinterpret_cast<const unsigned char*>(serverSignature.data()), serverSignature.size(),
                         reinterpret_cast<const unsigned char*>(serverSecret.data()), serverSecret.size()) == 1;
    if (!signedByHost) return fail("The host's pairing secret is not signed by its certificate (possible interception).");
    if (hash(challenge + certSignature(serverCert.get()) + serverSecret) != serverResponse)
        return fail("Wrong PIN: the host did not confirm it.");

    MdCtxPtr sign(EVP_MD_CTX_new());
    size_t sigLen = 0;
    EVP_DigestSignInit(sign.get(), nullptr, EVP_sha256(), nullptr, clientKey.get());
    EVP_DigestSign(sign.get(), nullptr, &sigLen, reinterpret_cast<const unsigned char*>(clientSecret.data()), clientSecret.size());
    std::string signature(sigLen, '\0');
    EVP_DigestSign(sign.get(), reinterpret_cast<unsigned char*>(signature.data()), &sigLen,
                   reinterpret_cast<const unsigned char*>(clientSecret.data()), clientSecret.size());
    signature.resize(sigLen);
    if (step("clientpairingsecret=" + hex(clientSecret + signature), false, 10).empty()) return fail(*error);

    if (step("phrase=pairchallenge", true, 10).empty()) return fail(*error);
    return true;
}

} // namespace rdc::gamestream
