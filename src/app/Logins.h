#pragma once

#include "core/Connection.h"

#include <QMap>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QListWidget;
class QSettings;

// A login of Settings → Logins: one user name and password for many connections
struct Login {
    QString id, name, user, domain;
    QString summary() const; // domain\user
};
std::vector<Login> loadLogins(QSettings* settings);
std::optional<Login> findLogin(QSettings* settings, const QString& id);
std::string loginSecret(const QString& id); // its password's name in secrets

inline constexpr char fromGroup[] = "group"; // a connection's login, tunnel or jump host: the group's

// what a group's connections get where they choose "As the group"; ids, "" = none
struct GroupDefaults {
    QString login, sshServer;
};
GroupDefaults groupDefaults(QSettings* settings, const QString& group);
void setGroupDefaults(QSettings* settings, const QString& group, const GroupDefaults& defaults);
// "As the group" replaced by the group's choice; the login's user name, domain and the name of its password's secret
// (empty: the connection's own)
rdc::Connection withDefaults(QSettings* settings, rdc::Connection c, std::string* passwordSecret);

// Settings → Logins as a dialog above `parent`; set by the main window
void setLoginsEditor(QSettings* settings, std::function<void(QWidget* parent)> open);
// a login list's last items: "New Login…", which opens the editor, then refill(the login added there, else the one chosen before)
void offerNewLogin(QComboBox* box, std::function<void(const QString& select)> refill);

// Settings → Logins; passwords are saved on apply()
class LoginsPage : public QWidget {
    Q_OBJECT
public:
    explicit LoginsPage(QSettings* settings, QWidget* parent = nullptr);

    bool pending() const { return !passwords_.isEmpty() || !forgotten_.isEmpty(); }
    void apply();

signals:
    void changed();

private:
    void save() const;
    void showList();
    void edit(int row); // -1: a new one
    void remove(int row);

    QSettings* settings_;
    std::vector<Login> logins_;
    QMap<QString, QString> passwords_; // typed, by login id
    QStringList forgotten_;           // removed logins' ids
    QListWidget* list_;
    QLabel* empty_;
};
