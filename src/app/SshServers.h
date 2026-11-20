#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <optional>
#include <string>
#include <vector>

class QLabel;
class QListWidget;
class QPushButton;
class QSettings;

// A server of Settings → SSH Servers
struct SshServer {
    QString id, name, host, user, auth = QStringLiteral("password"), keyFile;
    QString address() const; // [user@]host[:port]
    QString summary() const; // and the authentication
};
std::vector<SshServer> loadSshServers(QSettings* settings);
std::optional<SshServer> findSshServer(QSettings* settings, const QString& id);
std::string sshServerSecret(const QString& id); // its password's name in secrets

// Settings → SSH Servers; passwords are saved on apply()
class SshServersPage : public QWidget {
    Q_OBJECT
public:
    explicit SshServersPage(QSettings* settings, QWidget* parent = nullptr);

    bool pending() const { return !passwords_.isEmpty() || !forgotten_.isEmpty(); } // password changes apply() makes
    void apply();

signals:
    void changed();

private:
    void save() const;
    void showList();
    void edit(int row); // -1: a new one
    void remove();

    QSettings* settings_;
    std::vector<SshServer> servers_;
    QMap<QString, QString> passwords_; // typed, by server id
    QStringList forgotten_;           // removed servers' ids
    QListWidget* list_;
    QLabel* empty_;
    QPushButton* remove_;
};
