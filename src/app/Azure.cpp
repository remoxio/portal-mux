#include "app/Azure.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QUrlQuery>

#include <algorithm>
#include <map>

namespace azure {

static QString tr(const char* text) { return QCoreApplication::translate("Azure", text); }

// Resource Manager by its old name, as the Azure CLI asks for it: the serial console trusts no other ("Connection not trusted")
static QString scopeOf(const Account& account)
{
    return QStringLiteral("https://management.core.windows.net//%1 offline_access openid profile")
        .arg(account.client() == cliClientId ? QStringLiteral(".default") : QStringLiteral("user_impersonation")); // one's own app: no setup
}

std::string Vm::key() const { return QString::fromStdString(id).toLower().toStdString(); }

QString loginUrl() { return qEnvironmentVariable("PORTAL_MUX_AZURE_LOGIN", QStringLiteral("https://login.microsoftonline.com")); }

QString armUrl() { return qEnvironmentVariable("PORTAL_MUX_AZURE_ARM", QStringLiteral("https://management.azure.com")); }

static QString lower(const QString& id) { return id.toLower(); }

std::optional<Page> parsePage(const QByteArray& json, QString* error)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    if (!o.value(QLatin1String("value")).isArray()) {
        if (error) *error = tr("Azure sent no list.");
        return std::nullopt;
    }
    return Page{o.value(QLatin1String("value")).toArray(), o.value(QLatin1String("nextLink")).toString()};
}

std::vector<Subscription> subscriptions(const QJsonArray& value)
{
    std::vector<Subscription> r;
    for (const QJsonValue& v : value) {
        const QJsonObject o = v.toObject();
        r.push_back({o.value(QLatin1String("subscriptionId")).toString().toStdString(), o.value(QLatin1String("displayName")).toString().toStdString(),
                     o.value(QLatin1String("state")).toString().toStdString()});
    }
    std::sort(r.begin(), r.end(), [](const Subscription& a, const Subscription& b) { return a.name < b.name; });
    return r;
}

// "/subscriptions/<id>/resourceGroups/<name>/…": the part after `after`
static QString idPart(const QString& id, const QString& after)
{
    const QStringList parts = id.split(QLatin1Char('/'));
    for (qsizetype i = 0; i + 1 < parts.size(); ++i)
        if (parts[i].compare(after, Qt::CaseInsensitive) == 0) return parts[i + 1];
    return {};
}

std::vector<Vm> vms(const QJsonArray& value)
{
    std::vector<Vm> r;
    for (const QJsonValue& v : value) {
        const QJsonObject o = v.toObject();
        const QJsonObject p = o.value(QLatin1String("properties")).toObject();
        Vm vm;
        const QString id = o.value(QLatin1String("id")).toString();
        vm.id = id.toStdString();
        vm.name = o.value(QLatin1String("name")).toString().toStdString();
        vm.subscription = idPart(id, QStringLiteral("subscriptions")).toStdString();
        vm.resourceGroup = idPart(id, QStringLiteral("resourceGroups")).toStdString();
        vm.location = o.value(QLatin1String("location")).toString().toStdString();
        vm.size = p.value(QLatin1String("hardwareProfile")).toObject().value(QLatin1String("vmSize")).toString().toStdString();
        vm.os = p.value(QLatin1String("storageProfile")).toObject().value(QLatin1String("osDisk")).toObject().value(QLatin1String("osType")).toString().toStdString();
        vm.admin = p.value(QLatin1String("osProfile")).toObject().value(QLatin1String("adminUsername")).toString().toStdString();
        vm.bootDiagnostics =
            p.value(QLatin1String("diagnosticsProfile")).toObject().value(QLatin1String("bootDiagnostics")).toObject().value(QLatin1String("enabled")).toBool();
        for (const QJsonValue& nic : p.value(QLatin1String("networkProfile")).toObject().value(QLatin1String("networkInterfaces")).toArray()) {
            const QJsonObject n = nic.toObject();
            const std::string nicId = n.value(QLatin1String("id")).toString().toStdString();
            if (n.value(QLatin1String("properties")).toObject().value(QLatin1String("primary")).toBool()) vm.nics.insert(vm.nics.begin(), nicId);
            else vm.nics.push_back(nicId);
        }
        if (!vm.id.empty()) r.push_back(std::move(vm));
    }
    std::sort(r.begin(), r.end(), [](const Vm& a, const Vm& b) {
        return QString::fromStdString(a.name).compare(QString::fromStdString(b.name), Qt::CaseInsensitive) < 0;
    });
    return r;
}

void applyPower(std::vector<Vm>& vms, const QJsonArray& statusOnly)
{
    std::map<QString, std::string> power;
    for (const QJsonValue& v : statusOnly) {
        const QJsonObject o = v.toObject();
        const QJsonObject p = o.value(QLatin1String("properties")).toObject();
        const QJsonObject view = (p.contains(QLatin1String("instanceView")) ? p : o).value(QLatin1String("instanceView")).toObject();
        for (const QJsonValue& s : view.value(QLatin1String("statuses")).toArray()) {
            const QString code = s.toObject().value(QLatin1String("code")).toString();
            if (code.startsWith(QLatin1String("PowerState/"))) power[lower(o.value(QLatin1String("id")).toString())] = code.mid(11).toStdString();
        }
    }
    for (Vm& vm : vms)
        if (const auto it = power.find(QString::fromStdString(vm.key())); it != power.end()) vm.power = it->second;
}

void applyAddresses(std::vector<Vm>& vms, const QJsonArray& nics, const QJsonArray& publicIps)
{
    std::map<QString, QString> ips; // public IP resource → address
    for (const QJsonValue& v : publicIps) {
        const QJsonObject o = v.toObject();
        ips[lower(o.value(QLatin1String("id")).toString())] = o.value(QLatin1String("properties")).toObject().value(QLatin1String("ipAddress")).toString();
    }
    std::map<QString, std::pair<QString, QString>> addresses; // NIC → private, public
    for (const QJsonValue& v : nics) {
        const QJsonObject o = v.toObject();
        std::pair<QString, QString> found;
        for (const QJsonValue& c : o.value(QLatin1String("properties")).toObject().value(QLatin1String("ipConfigurations")).toArray()) {
            const QJsonObject p = c.toObject().value(QLatin1String("properties")).toObject();
            const std::pair<QString, QString> here{p.value(QLatin1String("privateIPAddress")).toString(),
                                                   ips[lower(p.value(QLatin1String("publicIPAddress")).toObject().value(QLatin1String("id")).toString())]};
            if (found.first.isEmpty() || p.value(QLatin1String("primary")).toBool()) found = here;
        }
        addresses[lower(o.value(QLatin1String("id")).toString())] = found;
    }
    for (Vm& vm : vms) {
        vm.privateIp.clear();
        vm.publicIp.clear();
        for (const std::string& nic : vm.nics)
            if (const auto it = addresses.find(lower(QString::fromStdString(nic))); it != addresses.end()) {
                vm.privateIp = it->second.first.toStdString();
                vm.publicIp = it->second.second.toStdString();
                break;
            }
    }
}

QString errorText(const QByteArray& json)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    const QJsonValue error = o.value(QLatin1String("error"));
    QString text = error.isObject() ? error.toObject().value(QLatin1String("message")).toString()
                                    : o.value(QLatin1String("error_description")).toString();
    if (text.isEmpty()) text = error.toString();
    return text.section(QLatin1String("\r\n"), 0, 0).section(QLatin1Char('\n'), 0, 0).trimmed(); // sign-in errors: trace IDs after it
}

static QByteArray base64url(const QByteArray& bytes) { return bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals); }

QByteArray newVerifier()
{
    QByteArray bytes(32, Qt::Uninitialized);
    QRandomGenerator::system()->generate(bytes.begin(), bytes.end());
    return base64url(bytes);
}

QByteArray challenge(const QByteArray& verifier) { return base64url(QCryptographicHash::hash(verifier, QCryptographicHash::Sha256)); }

static QString endpoint(const Account& account, const char* what)
{
    return loginUrl() + QLatin1Char('/') + QString::fromLatin1(QUrl::toPercentEncoding(QString::fromStdString(account.tenant))) +
           QStringLiteral("/oauth2/v2.0/") + QLatin1String(what);
}

QUrl tokenUrl(const Account& account) { return QUrl(endpoint(account, "token")); }

QUrl authorizeUrl(const Account& account, const QString& redirect, const QByteArray& challenge, const QString& state)
{
    QUrlQuery query;
    for (const auto& [key, value] : std::initializer_list<std::pair<const char*, QString>>{
             {"client_id", QString::fromStdString(account.client())}, {"response_type", QStringLiteral("code")}, {"redirect_uri", redirect},
             {"response_mode", QStringLiteral("query")}, {"scope", scopeOf(account)}, {"state", state}, {"code_challenge", QString::fromLatin1(challenge)},
             {"code_challenge_method", QStringLiteral("S256")}, {"prompt", QStringLiteral("select_account")}})
        query.addQueryItem(QLatin1String(key), QString::fromLatin1(QUrl::toPercentEncoding(value)));
    QUrl url(endpoint(account, "authorize"));
    url.setQuery(query.query(QUrl::FullyEncoded), QUrl::StrictMode);
    return url;
}

static QByteArray form(std::initializer_list<std::pair<const char*, QString>> fields)
{
    QByteArray body;
    for (const auto& [key, value] : fields) body += (body.isEmpty() ? "" : "&") + QByteArray(key) + '=' + QUrl::toPercentEncoding(value);
    return body;
}

QByteArray codeForm(const Account& account, const QString& code, const QString& redirect, const QByteArray& verifier)
{
    return form({{"client_id", QString::fromStdString(account.client())}, {"grant_type", QStringLiteral("authorization_code")}, {"code", code},
                 {"redirect_uri", redirect}, {"code_verifier", QString::fromLatin1(verifier)}, {"scope", scopeOf(account)}});
}

QByteArray refreshForm(const Account& account, const QString& refreshToken)
{
    return form({{"client_id", QString::fromStdString(account.client())}, {"grant_type", QStringLiteral("refresh_token")},
                 {"refresh_token", refreshToken}, {"scope", scopeOf(account)}});
}

std::optional<Redirect> parseRedirect(const QByteArray& request)
{
    const QList<QByteArray> line = request.left(request.indexOf("\r\n")).split(' ');
    if (line.size() < 2 || line[0] != "GET") return std::nullopt;
    const qsizetype q = line[1].indexOf('?');
    if (q < 0) return std::nullopt;
    const QUrlQuery query(QString::fromLatin1(line[1].mid(q + 1).replace('+', "%20")));
    Redirect r{query.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded), query.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded),
               query.queryItemValue(QStringLiteral("error_description"), QUrl::FullyDecoded)};
    if (r.error.isEmpty()) r.error = query.queryItemValue(QStringLiteral("error"), QUrl::FullyDecoded);
    if (r.code.isEmpty() && r.error.isEmpty()) return std::nullopt;
    return r;
}

std::optional<Tokens> parseTokens(const QByteArray& json, QString* error)
{
    const QJsonObject o = QJsonDocument::fromJson(json).object();
    Tokens t;
    t.access = o.value(QLatin1String("access_token")).toString();
    t.refresh = o.value(QLatin1String("refresh_token")).toString();
    const QJsonValue expires = o.value(QLatin1String("expires_in")); // number, or a string
    t.expiresIn = expires.isString() ? expires.toString().toLongLong() : expires.toInteger();
    if (t.access.isEmpty()) {
        if (error) *error = errorText(json).isEmpty() ? tr("The sign-in sent no token.") : errorText(json);
        return std::nullopt;
    }
    // only shown, so not verified: it came straight from the sign-in over HTTPS
    const QByteArray payload = o.value(QLatin1String("id_token")).toString().toLatin1().split('.').value(1);
    const QJsonObject claims =
        QJsonDocument::fromJson(QByteArray::fromBase64(payload, QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)).object();
    for (const char* claim : {"preferred_username", "upn", "email", "name"})
        if (t.user.empty()) t.user = claims.value(QLatin1String(claim)).toString().toStdString();
    t.tenantId = claims.value(QLatin1String("tid")).toString().toStdString();
    return t;
}

} // namespace azure
