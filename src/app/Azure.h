#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QString>
#include <QUrl>

#include <optional>
#include <string>
#include <vector>

// Azure Resource Manager answers and the sign-in's pieces as plain data; no network (see AzureClient).
namespace azure {

inline constexpr char cliClientId[] = "04b07795-8ddb-461a-bbee-02f9e1bf7b46"; // Azure CLI's public client, accepted by every tenant

struct Account {
    std::string id, name;
    std::string tenant = "organizations"; // or a directory's ID or domain
    std::string clientId;                 // empty: cliClientId
    std::string user, tenantId;           // who signed in, and where: from the ID token
    std::string client() const { return clientId.empty() ? cliClientId : clientId; }
};

struct Subscription {
    std::string id, name, state;
};

struct Vm {
    std::string id; // the resource ID
    std::string name, subscription, resourceGroup, location, size;
    std::string os;    // "Windows" or "Linux"
    std::string admin; // its administrator account
    std::string power; // running, deallocated, stopped, starting, stopping, deallocating; empty: not known
    std::vector<std::string> nics;
    std::string privateIp, publicIp;
    std::string key() const; // the ID in lower case: Azure doesn't keep its case
};

QString loginUrl(); // https://login.microsoftonline.com; tests: PORTAL_MUX_AZURE_LOGIN
QString armUrl();   // https://management.azure.com; tests: PORTAL_MUX_AZURE_ARM

struct Page {
    QJsonArray value;
    QString next;
};
std::optional<Page> parsePage(const QByteArray& json, QString* error);
std::vector<Subscription> subscriptions(const QJsonArray& value);
std::vector<Vm> vms(const QJsonArray& value);                 // sorted by name
void applyPower(std::vector<Vm>& vms, const QJsonArray& statusOnly); // the list with statusOnly=true
void applyAddresses(std::vector<Vm>& vms, const QJsonArray& nics, const QJsonArray& publicIps);
QString errorText(const QByteArray& json); // Resource Manager's or the sign-in's

// Sign-in: OAuth 2.0 authorization code with PKCE, in the system browser, back to a local port
QByteArray newVerifier();
QByteArray challenge(const QByteArray& verifier);
QUrl tokenUrl(const Account& account);
QUrl authorizeUrl(const Account& account, const QString& redirect, const QByteArray& challenge, const QString& state);
QByteArray codeForm(const Account& account, const QString& code, const QString& redirect, const QByteArray& verifier);
QByteArray refreshForm(const Account& account, const QString& refreshToken);
struct Redirect {
    QString code, state, error; // error: the sign-in's description
};
std::optional<Redirect> parseRedirect(const QByteArray& request); // the browser's "GET /?code=… HTTP/1.1"; nullopt: not the answer
struct Tokens {
    QString access, refresh;
    qint64 expiresIn = 0;
    std::string user, tenantId; // from the ID token, when sent
};
std::optional<Tokens> parseTokens(const QByteArray& json, QString* error);

} // namespace azure
