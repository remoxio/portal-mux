#pragma once

#include "app/Proxmox.h"

#include <QNetworkAccessManager>
#include <QObject>

#include <chrono>
#include <deque>
#include <functional>
#include <map>

class QNetworkReply;
class QNetworkRequest;
class QSslCertificate;
class QUrlQuery;

// One Proxmox VE server's API: ticket or token auth, pinned certificate.
class ProxmoxClient : public QObject {
    Q_OBJECT

public:
    explicit ProxmoxClient(proxmox::Server server, QObject* parent = nullptr);

    const proxmox::Server& server() const { return server_; }
    void setServer(proxmox::Server server);
    void setSecret(const QString& secret); // password or token secret
    bool hasSecret() const { return !secret_.isEmpty(); }
    // Login refused: don't poll (fail2ban, lockouts).
    bool rejected() const { return rejected_; }

    const proxmox::Resources& resources() const { return resources_; } // the last known, also while problem() says why not current
    const QString& lastError() const { return lastError_; }
    enum class Problem { None, NoSecret, Refused, Certificate, Unreachable, Other };
    Problem problem() const { return problem_; }
    bool loaded() const { return loaded_; } // the last refresh listed the cluster
    const std::deque<float>* cpu(const std::string& key) const; // percent, one per refresh; key: a guest's, or "node/<name>"

    void refresh();
    void retry(); // also asks about an untrusted certificate again
    // Server errors (login, certificate, unreachable) also emit failed().
    using Fail = std::function<void(const QString& message)>;
    void control(const proxmox::Guest& guest, const QString& action, Fail onFail);
    void requestSpice(const proxmox::Guest& guest, std::function<void(QByteArray json)> done, Fail onFail);
    void checkSpice(const proxmox::Guest& guest, std::function<void(bool spice)> done, Fail onFail);
    void requestVnc(const proxmox::Guest& guest, std::function<void(QByteArray json)> done, Fail onFail);
    void requestTerminal(const proxmox::Guest& guest, std::function<void(QByteArray json)> done, Fail onFail);
    QNetworkRequest vncWebSocket(const proxmox::Guest& guest, int port, const QString& ticket) const;
    bool trusts(const QSslCertificate& certificate) const;

signals:
    void resourcesChanged();
    void failed(const QString& message);
    void certificateUnknown(const QString& fingerprint, const QString& subject);

private:
    using Done = std::function<void(QByteArray body)>;
    void send(const QByteArray& method, const QString& path, const QByteArray& form, Done done, Fail onFail = {}, bool retried = false);
    void login(std::function<void()> then, Fail onFail);
    QNetworkRequest request(const QString& scheme, const QString& path, const QUrlQuery& query, bool write) const;
    void fail(Problem problem, const QString& message, const Fail& onFail = {});

    proxmox::Server server_;
    QString secret_;
    QString ticket_, csrf_;
    QNetworkAccessManager network_;
    proxmox::Resources resources_;
    std::map<std::string, std::deque<float>> cpu_;
    std::chrono::steady_clock::time_point sampled_;
    QString lastError_;
    Problem problem_ = Problem::None;
    bool loaded_ = false;
    bool asked_ = false; // certificateUnknown() emitted
    bool rejected_ = false;
};
