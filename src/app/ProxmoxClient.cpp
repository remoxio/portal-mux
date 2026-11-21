#include "app/ProxmoxClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>

using proxmox::Guest;

ProxmoxClient::ProxmoxClient(proxmox::Server server, QObject* parent) : QObject(parent), server_(std::move(server)) {}

void ProxmoxClient::setServer(proxmox::Server server)
{
    server_ = std::move(server);
    ticket_.clear();
    csrf_.clear();
    asked_ = false;
    rejected_ = false;
}

void ProxmoxClient::setSecret(const QString& secret)
{
    secret_ = secret;
    ticket_.clear();
    rejected_ = false;
}

void ProxmoxClient::fail(Problem problem, const QString& message, const Fail& onFail)
{
    lastError_ = message;
    problem_ = problem;
    cpu_.clear();
    loaded_ = false;
    emit failed(message);
    if (onFail) onFail(message);
}

void ProxmoxClient::send(const QByteArray& method, const QString& path, const QByteArray& form, Done done, Fail onFail, bool retried)
{
    const bool token = proxmox::isApiToken(server_.user);
    if (secret_.isEmpty()) return fail(Problem::NoSecret, tr("No password stored for %1.").arg(QString::fromStdString(server_.name)), onFail);
    if (!token && ticket_.isEmpty() && path != QLatin1String("/access/ticket"))
        return login([=, this] { send(method, path, form, done, onFail, true); }, onFail);

    QNetworkRequest request = this->request(QStringLiteral("https"), path, {}, method != "GET");
    if (!form.isEmpty()) request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    QNetworkReply* reply = network_.sendCustomRequest(request, method, form);

    auto unknown = std::make_shared<QString>();
    connect(reply, &QNetworkReply::sslErrors, this, [this, reply, unknown](const QList<QSslError>&) {
        const QSslCertificate cert = reply->sslConfiguration().peerCertificate();
        if (trusts(cert)) reply->ignoreSslErrors();
        else *unknown = QString::fromLatin1(cert.digest(QCryptographicHash::Sha256).toHex()) + QLatin1Char('\n') + cert.subjectDisplayName();
    });
    connect(reply, &QNetworkReply::finished, this, [=, this] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (!unknown->isEmpty()) {
            fail(Problem::Certificate, tr("Its certificate is not trusted."), onFail);
            if (!asked_) {
                asked_ = true;
                emit certificateUnknown(unknown->section(QLatin1Char('\n'), 0, 0), unknown->section(QLatin1Char('\n'), 1));
            }
            return;
        }
        if (status == 401 && !token && !retried && path != QLatin1String("/access/ticket")) { // ticket expired
            ticket_.clear();
            return send(method, path, form, done, onFail, true);
        }
        if (reply->error() != QNetworkReply::NoError && status == 0) return fail(Problem::Unreachable, reply->errorString(), onFail);
        const QByteArray body = reply->readAll(); // not before: a timed-out reply is closed
        if (status < 200 || status >= 300) {
            const QString text = proxmox::errorText(body);
            const QString reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
            const QString message = text.isEmpty() ? QStringLiteral("%1 %2").arg(status).arg(reason) : text;
            rejected_ = rejected_ || status == 401;
            if (status == 401 || !onFail || path == QLatin1String("/access/ticket"))
                return fail(status == 401 ? Problem::Refused : Problem::Other, message, onFail);
            return onFail(message);
        }
        done(body);
    });
}

QNetworkRequest ProxmoxClient::request(const QString& scheme, const QString& path, const QUrlQuery& query, bool write) const
{
    QUrl url;
    url.setScheme(scheme);
    url.setHost(QString::fromStdString(server_.host));
    url.setPort(server_.port);
    url.setPath(QStringLiteral("/api2/json") + path);
    url.setQuery(query);
    QNetworkRequest request(url);
    request.setTransferTimeout(15000);
    if (proxmox::isApiToken(server_.user)) {
        request.setRawHeader("Authorization", "PVEAPIToken=" + QByteArray::fromStdString(server_.user) + '=' + secret_.toUtf8());
    } else if (!ticket_.isEmpty()) {
        request.setRawHeader("Cookie", "PVEAuthCookie=" + QUrl::toPercentEncoding(ticket_));
        if (write) request.setRawHeader("CSRFPreventionToken", csrf_.toUtf8());
    }
    return request;
}

bool ProxmoxClient::trusts(const QSslCertificate& certificate) const
{
    return !server_.fingerprint.empty() && !certificate.isNull() &&
           certificate.digest(QCryptographicHash::Sha256).toHex() == QByteArray::fromStdString(server_.fingerprint);
}

void ProxmoxClient::login(std::function<void()> then, Fail onFail)
{
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("username"), QString::fromStdString(server_.user));
    form.addQueryItem(QStringLiteral("password"), secret_);
    send("POST", QStringLiteral("/access/ticket"), form.query(QUrl::FullyEncoded).toUtf8(), [this, then, onFail](const QByteArray& body) {
        const QJsonObject d = QJsonDocument::fromJson(body).object().value(QLatin1String("data")).toObject();
        if (d.contains(QLatin1String("NeedTFA"))) {
            rejected_ = true;
            return fail(Problem::Refused, tr("This account logs in with a second factor, which Portal Mux can't do yet: use an API token."), onFail);
        }
        ticket_ = d.value(QLatin1String("ticket")).toString();
        csrf_ = d.value(QLatin1String("CSRFPreventionToken")).toString();
        if (ticket_.isEmpty()) return fail(Problem::Other, tr("Proxmox accepted the login but sent no ticket."), onFail);
        then();
    }, onFail);
}

void ProxmoxClient::refresh()
{
    send("GET", QStringLiteral("/cluster/resources"), {}, [this](const QByteArray& body) {
        QString error;
        auto r = proxmox::parseResources(body, &error);
        if (!r) return fail(Problem::Other, error);
        resources_ = std::move(*r);
        lastError_.clear();
        problem_ = Problem::None;
        loaded_ = true;
        rejected_ = false;
        const auto now = std::chrono::steady_clock::now();
        if (now - sampled_ > std::chrono::seconds(15)) cpu_.clear(); // not polled meanwhile: the graph would bridge the gap
        sampled_ = now;
        std::map<std::string, std::deque<float>> cpu;
        auto sample = [&](const std::string& key, double value) {
            std::deque<float>& history = cpu[key] = std::move(cpu_[key]);
            history.push_back(float(std::clamp(value * 100, 0.0, 100.0)));
            if (history.size() > 40) history.pop_front();
        };
        for (const proxmox::Node& n : resources_.nodes)
            if (n.online) sample("node/" + n.name, n.cpu);
        for (const Guest& g : resources_.guests)
            if (g.status == "running") sample(g.key(), g.cpu);
        cpu_ = std::move(cpu);
        emit resourcesChanged();
    });
}

void ProxmoxClient::retry()
{
    asked_ = false;
    refresh();
}

const std::deque<float>* ProxmoxClient::cpu(const std::string& key) const
{
    const auto it = cpu_.find(key);
    return it == cpu_.end() ? nullptr : &it->second;
}

void ProxmoxClient::control(const Guest& guest, const QString& action, Fail onFail)
{
    const QString path = QStringLiteral("/nodes/%1/%2/%3/status/%4")
                             .arg(QString::fromStdString(guest.node), QString::fromStdString(guest.type))
                             .arg(guest.vmid)
                             .arg(action);
    send("POST", path, {}, [this](const QByteArray&) {
        QTimer::singleShot(1500, this, &ProxmoxClient::refresh);
    }, std::move(onFail));
}

void ProxmoxClient::requestSpice(const Guest& guest, std::function<void(QByteArray json)> done, Fail onFail)
{
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("proxy"), QString::fromStdString(server_.host));
    const QString path = QStringLiteral("/nodes/%1/qemu/%2/spiceproxy").arg(QString::fromStdString(guest.node)).arg(guest.vmid);
    send("POST", path, form.query(QUrl::FullyEncoded).toUtf8(), std::move(done), std::move(onFail));
}

static QString guestPath(const Guest& guest, const char* rest)
{
    return QStringLiteral("/nodes/%1/%2/%3/%4").arg(QString::fromStdString(guest.node), QString::fromStdString(guest.type)).arg(guest.vmid)
        .arg(QLatin1String(rest));
}

void ProxmoxClient::checkSpice(const Guest& guest, std::function<void(bool spice)> done, Fail onFail)
{
    send("GET", guestPath(guest, "status/current"), {}, [done](const QByteArray& body) { done(proxmox::usesSpice(body)); }, std::move(onFail));
}

void ProxmoxClient::requestVnc(const Guest& guest, std::function<void(QByteArray json)> done, Fail onFail)
{
    send("POST", guestPath(guest, "vncproxy"), "websocket=1", std::move(done), std::move(onFail));
}

void ProxmoxClient::requestTerminal(const Guest& guest, std::function<void(QByteArray json)> done, Fail onFail)
{
    send("POST", guestPath(guest, "termproxy"), {}, std::move(done), std::move(onFail));
}

QNetworkRequest ProxmoxClient::vncWebSocket(const Guest& guest, int port, const QString& ticket) const
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("port"), QString::number(port));
    query.addQueryItem(QStringLiteral("vncticket"), QString::fromLatin1(QUrl::toPercentEncoding(ticket)));
    return request(QStringLiteral("wss"), guestPath(guest, "vncwebsocket"), query, false);
}
