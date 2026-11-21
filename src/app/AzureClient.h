#pragma once

#include "app/Azure.h"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>

#include <functional>
#include <vector>

class QTcpServer;
class QTcpSocket;

// One Azure account's Resource Manager API: sign-in in the system browser, its subscriptions and VMs, power actions
class AzureClient : public QObject {
    Q_OBJECT

public:
    explicit AzureClient(azure::Account account, QObject* parent = nullptr);

    const azure::Account& account() const { return account_; }
    void setAccount(azure::Account account); // another directory or client: signed out
    void setRefreshToken(const QString& token);
    bool signedIn() const { return !refresh_.isEmpty(); }
    void signIn(); // while one waits: its page again
    bool signingIn() const { return !listeners_.empty(); }

    enum class Problem { None, SignIn, Unreachable, Other };
    Problem problem() const { return problem_; }
    const QString& lastError() const { return lastError_; }
    bool loaded() const { return loaded_; } // the last refresh listed them
    const std::vector<azure::Subscription>& subscriptions() const { return subscriptions_; }
    const std::vector<azure::Vm>& vms() const { return vms_; } // the last known, also while problem() says why not current
    const azure::Vm* vm(const std::string& key) const;

    void refresh();
    using Fail = std::function<void(const QString& message)>;
    void control(const azure::Vm& vm, const QString& action, Fail onFail); // start, restart, deallocate, powerOff
    // its serial console's websocket, and the token to send it first
    void serialConsole(const azure::Vm& vm, std::function<void(QUrl url, QString token)> done, Fail onFail);

signals:
    void changed();                                // its VMs, its state, or the account (who signed in)
    void tokenChanged(const QString& refreshToken); // to keep

private:
    struct Error {
        Problem problem;
        QString message;
    };
    using Failed = std::function<void(const Error&)>;
    void send(const QByteArray& method, const QString& url, Failed failed, std::function<void(QByteArray)> done, bool retried = false);
    void getAll(const QString& url, std::function<void(QJsonArray)> done, Failed failed, QJsonArray before = {});
    void renew(std::function<void()> then, Failed failed);
    void redeem(const QByteArray& form, Failed failed, std::function<void()> then);
    void answer(QTcpSocket* socket, const QByteArray& request);
    void endSignIn();
    void fail(const Error& error);

    azure::Account account_;
    QString refresh_, access_;
    QDateTime expires_;
    std::vector<std::pair<std::function<void()>, Failed>> waiting_; // for a new access token
    QNetworkAccessManager network_;
    std::vector<azure::Subscription> subscriptions_;
    std::vector<azure::Vm> vms_;
    Problem problem_ = Problem::None;
    QString lastError_;
    bool loaded_ = false, refreshing_ = false;
    // the sign-in waiting for the browser
    std::vector<QTcpServer*> listeners_;
    QByteArray verifier_;
    QString state_, redirect_;
    QUrl page_;
    QTimer signInTimeout_;
};
