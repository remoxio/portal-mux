#include "app/AzureClient.h"

#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>

#include <memory>

static const QString computeApi = QStringLiteral("2024-07-01"), networkApi = QStringLiteral("2024-05-01"),
                     subscriptionsApi = QStringLiteral("2022-12-01"), serialApi = QStringLiteral("2024-07-01");

AzureClient::AzureClient(azure::Account account, QObject* parent) : QObject(parent), account_(std::move(account))
{
    signInTimeout_.setSingleShot(true);
    signInTimeout_.setInterval(10 * 60 * 1000);
    connect(&signInTimeout_, &QTimer::timeout, this, [this] {
        endSignIn();
        fail({Problem::SignIn, tr("The sign-in timed out.")});
    });
}

void AzureClient::setAccount(azure::Account account)
{
    const bool same = account.tenant == account_.tenant && account.client() == account_.client();
    account_ = std::move(account);
    if (same) return;
    refresh_.clear();
    access_.clear();
    emit tokenChanged({});
    fail({Problem::SignIn, tr("Not signed in.")});
}

void AzureClient::setRefreshToken(const QString& token)
{
    refresh_ = token;
    access_.clear();
}

const azure::Vm* AzureClient::vm(const std::string& key) const
{
    for (const azure::Vm& v : vms_)
        if (v.key() == key) return &v;
    return nullptr;
}

void AzureClient::fail(const Error& error)
{
    problem_ = error.problem;
    lastError_ = error.message;
    loaded_ = false;
    emit changed();
}

void AzureClient::signIn()
{
    if (signingIn()) {
        QDesktopServices::openUrl(page_);
        return;
    }
    auto* v4 = new QTcpServer(this);
    if (!v4->listen(QHostAddress::LocalHost)) {
        delete v4;
        return fail({Problem::SignIn, tr("No local port for the sign-in to come back to.")});
    }
    listeners_.push_back(v4);
    auto* v6 = new QTcpServer(this); // "localhost" may be ::1 to the browser
    if (v6->listen(QHostAddress::LocalHostIPv6, v4->serverPort())) listeners_.push_back(v6);
    else delete v6;
    for (QTcpServer* server : listeners_)
        connect(server, &QTcpServer::newConnection, this, [this, server] {
            while (QTcpSocket* socket = server->nextPendingConnection()) {
                socket->setParent(this); // outlives the listener, to send its answer
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                auto request = std::make_shared<QByteArray>();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket, request] {
                    *request += socket->readAll();
                    if (request->contains("\r\n\r\n")) answer(socket, *request);
                    else if (request->size() > 16384) socket->abort();
                });
            }
        });
    verifier_ = azure::newVerifier();
    state_ = QString::fromLatin1(azure::newVerifier());
    redirect_ = QStringLiteral("http://localhost:%1").arg(v4->serverPort());
    page_ = azure::authorizeUrl(account_, redirect_, azure::challenge(verifier_), state_);
    signInTimeout_.start();
    if (!QDesktopServices::openUrl(page_)) {
        endSignIn();
        return fail({Problem::SignIn, tr("No web browser to sign in with.")});
    }
    emit changed();
}

void AzureClient::endSignIn()
{
    for (QTcpServer* server : listeners_) server->deleteLater();
    listeners_.clear();
    signInTimeout_.stop();
}

void AzureClient::answer(QTcpSocket* socket, const QByteArray& request)
{
    auto reply = [socket](const char* status, const QString& text) {
        const QByteArray body = QStringLiteral("<!doctype html><meta charset=utf-8><title>Portal Mux</title>"
                                               "<p style=\"font:16px sans-serif;margin:3em\">%1</p>").arg(text.toHtmlEscaped()).toUtf8();
        socket->write(QByteArray("HTTP/1.1 ") + status + "\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
                      QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    };
    const auto r = azure::parseRedirect(request);
    if (!r) return reply("404 Not Found", {}); // the browser's favicon, say
    if (r->state != state_ || !signingIn()) return reply("400 Bad Request", tr("This is not the sign-in Portal Mux is waiting for."));
    reply("200 OK", r->error.isEmpty() ? tr("Signed in. You can close this page and go back to Portal Mux.") : tr("Not signed in: %1").arg(r->error));
    endSignIn();
    if (!r->error.isEmpty()) return fail({Problem::SignIn, r->error});
    redeem(azure::codeForm(account_, r->code, redirect_, verifier_), [this](const Error& e) { fail(e); }, [this] { refresh(); });
}

// the tokens for the sign-in's code, or for the refresh token
void AzureClient::redeem(const QByteArray& form, Failed failed, std::function<void()> then)
{
    QNetworkRequest request(azure::tokenUrl(account_));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    request.setTransferTimeout(20000);
    QNetworkReply* reply = network_.post(request, form);
    connect(reply, &QNetworkReply::finished, this, [this, reply, failed, then] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 0) return failed({Problem::Unreachable, reply->errorString()});
        QString error;
        const auto tokens = azure::parseTokens(reply->readAll(), &error);
        if (!tokens) return failed({Problem::SignIn, error});
        access_ = tokens->access;
        expires_ = QDateTime::currentDateTimeUtc().addSecs(tokens->expiresIn);
        if (!tokens->user.empty()) account_.user = tokens->user;
        if (!tokens->tenantId.empty()) account_.tenantId = tokens->tenantId;
        if (!tokens->refresh.isEmpty() && tokens->refresh != refresh_) {
            refresh_ = tokens->refresh;
            emit tokenChanged(refresh_);
        }
        then();
    });
}

void AzureClient::renew(std::function<void()> then, Failed failed)
{
    waiting_.emplace_back(std::move(then), std::move(failed));
    if (waiting_.size() > 1) return; // one renewal for all
    redeem(azure::refreshForm(account_, refresh_), [this](const Error& e) {
        for (auto& w : std::exchange(waiting_, {})) w.second(e);
    }, [this] {
        for (auto& w : std::exchange(waiting_, {})) w.first();
    });
}

void AzureClient::send(const QByteArray& method, const QString& url, Failed failed, std::function<void(QByteArray)> done, bool retried)
{
    if (refresh_.isEmpty()) return failed({Problem::SignIn, tr("Not signed in.")});
    if (access_.isEmpty() || QDateTime::currentDateTimeUtc().secsTo(expires_) < 120)
        return renew([=, this] { send(method, url, failed, done, true); }, failed);
    QNetworkRequest request{QUrl(url)};
    request.setRawHeader("Authorization", "Bearer " + access_.toLatin1());
    request.setTransferTimeout(20000);
    request.setRawHeader("Accept", "application/json");
    if (method == "POST") { // as Microsoft's SDKs: without them, Resource Manager says 411, the serial console 415
        request.setHeader(QNetworkRequest::ContentLengthHeader, 0);
        request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    }
    QNetworkReply* reply = network_.sendCustomRequest(request, method, QByteArray());
    connect(reply, &QNetworkReply::finished, this, [=, this] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401 && !retried) { // its token was revoked or is too old
            access_.clear();
            return send(method, url, failed, done, true);
        }
        if (status == 0) return failed({Problem::Unreachable, reply->errorString()});
        const QByteArray body = reply->readAll(); // not before: a timed-out reply is closed
        if (status < 200 || status >= 300) {
            const QString text = azure::errorText(body);
            return failed({status == 401 ? Problem::SignIn : Problem::Other,
                           text.isEmpty() ? QStringLiteral("HTTP %1 %2") // HTTP/2 has no reason phrase
                                                .arg(status).arg(reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString()).trimmed()
                                          : text});
        }
        done(body);
    });
}

void AzureClient::getAll(const QString& url, std::function<void(QJsonArray)> done, Failed failed, QJsonArray before)
{
    send("GET", url, failed, [=, this](const QByteArray& body) mutable {
        QString error;
        const auto page = azure::parsePage(body, &error);
        if (!page) return failed({Problem::Other, error});
        for (const QJsonValue& v : page->value) before.append(v);
        if (page->next.isEmpty()) return done(before);
        if (!page->next.startsWith(azure::armUrl() + QLatin1Char('/'))) return failed({Problem::Other, tr("Azure sent a next page elsewhere.")}); // not our token
        getAll(page->next, done, failed, before);
    });
}

void AzureClient::refresh()
{
    if (refreshing_ || signingIn()) return;
    refreshing_ = true;
    struct Gather {
        std::vector<azure::Subscription> subscriptions;
        QJsonArray vms, power, nics, ips;
        int pending = 0;
        std::optional<Error> error;
    };
    auto done = [this](const std::shared_ptr<Gather>& g) {
        if (--g->pending > 0) return;
        refreshing_ = false;
        if (g->error) return fail(*g->error);
        std::vector<azure::Vm> vms = azure::vms(g->vms);
        azure::applyPower(vms, g->power);
        azure::applyAddresses(vms, g->nics, g->ips);
        subscriptions_ = std::move(g->subscriptions);
        vms_ = std::move(vms);
        problem_ = Problem::None;
        lastError_.clear();
        loaded_ = true;
        emit changed();
    };
    getAll(azure::armUrl() + QStringLiteral("/subscriptions?api-version=") + subscriptionsApi, [this, done](const QJsonArray& list) {
        auto g = std::make_shared<Gather>();
        g->subscriptions = azure::subscriptions(list);
        g->pending = 1;
        for (const azure::Subscription& s : g->subscriptions) {
            if (s.state == "Disabled" || s.state == "Deleted") continue;
            const QString base = azure::armUrl() + QStringLiteral("/subscriptions/") + QString::fromStdString(s.id) + QStringLiteral("/providers/");
            for (auto [path, into] : {std::pair{QStringLiteral("Microsoft.Compute/virtualMachines?api-version=") + computeApi, &Gather::vms},
                                      std::pair{QStringLiteral("Microsoft.Compute/virtualMachines?statusOnly=true&api-version=") + computeApi, &Gather::power},
                                      std::pair{QStringLiteral("Microsoft.Network/networkInterfaces?api-version=") + networkApi, &Gather::nics},
                                      std::pair{QStringLiteral("Microsoft.Network/publicIPAddresses?api-version=") + networkApi, &Gather::ips}}) {
                ++g->pending;
                getAll(base + path, [g, into, done](const QJsonArray& items) {
                    for (const QJsonValue& v : items) ((*g).*into).append(v);
                    done(g);
                }, [g, done](const Error& e) {
                    if (!g->error || g->error->problem == Problem::Other) g->error = e; // the reason that needs the user first
                    done(g);
                });
            }
        }
        done(g);
    }, [this](const Error& e) {
        refreshing_ = false;
        fail(e);
    });
}

void AzureClient::control(const azure::Vm& vm, const QString& action, Fail onFail)
{
    const QString url = azure::armUrl() + QString::fromStdString(vm.id) + QLatin1Char('/') + action + QStringLiteral("?api-version=") + computeApi;
    send("POST", url, [this, onFail](const Error& e) {
        if (e.problem != Problem::Other) fail(e);
        if (onFail) onFail(e.message);
    }, [this, key = vm.key(), action](const QByteArray&) {
        const char* now = action == QLatin1String("start") ? "starting" : action == QLatin1String("deallocate") ? "deallocating"
                          : action == QLatin1String("powerOff")                                   ? "stopping" : nullptr;
        for (azure::Vm& v : vms_)
            if (v.key() == key && now) v.power = now; // at once; the next lists confirm it
        emit changed();
        QTimer::singleShot(3000, this, &AzureClient::refresh);
    });
}

// as `az serial-console connect`
void AzureClient::serialConsole(const azure::Vm& vm, std::function<void(QUrl, QString)> done, Fail onFail)
{
    auto failed = [this, onFail](const Error& e) {
        if (e.problem != Problem::Other) fail(e);
        onFail(e.message);
    };
    const QString status = azure::armUrl() + QStringLiteral("/subscriptions/") + QString::fromStdString(vm.subscription) +
                           QStringLiteral("/providers/Microsoft.SerialConsole/consoleServices/default?api-version=") + serialApi;
    // shortcut: the CLI asks https://<region>.management.azure.com when boot diagnostics use one's own storage account; do so if Azure refuses
    const QString connect = azure::armUrl() + QString::fromStdString(vm.id) +
                            QStringLiteral("/providers/Microsoft.SerialConsole/serialPorts/0/connect?api-version=") + serialApi;
    send("GET", status, failed, [=, this](const QByteArray& body) {
        if (QJsonDocument::fromJson(body).object().value(QLatin1String("properties")).toObject().value(QLatin1String("disabled")).toBool(true))
            return onFail(tr("The serial console is switched off for this subscription (az serial-console enable)."));
        send("POST", connect, failed, [=, this](const QByteArray& body) {
            const QUrl url(QJsonDocument::fromJson(body).object().value(QLatin1String("connectionString")).toString());
            if (!url.isValid() || (url.scheme() != QLatin1String("wss") && url.scheme() != QLatin1String("ws")))
                return onFail(tr("Azure sent no serial console address."));
            done(url, access_);
        });
    });
}
