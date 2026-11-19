#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <vector>

class QComboBox;
class QFormLayout;
class QLineEdit;
class QListWidget;
class QSettings;

// Settings → SSH Servers: saved in portal-mux.conf ([sshServers]) as edited; passwords go to the keychain on apply()
class SshServersPage : public QWidget {
    Q_OBJECT
public:
    explicit SshServersPage(QSettings* settings, QWidget* parent = nullptr);

    bool pending() const { return !passwords_.isEmpty() || !forgotten_.isEmpty(); } // keychain changes apply() makes
    void apply();

signals:
    void changed();

private:
    struct Server {
        QString id, name, host, user, auth = QStringLiteral("password"), keyFile;
    };

    void load();
    void save() const;
    void show(int row);
    void showAuth();
    void add();
    void remove();
    Server* current();

    QSettings* settings_;
    std::vector<Server> servers_;
    QString shownId_;                 // whose password the field holds
    QMap<QString, QString> passwords_; // typed, by server id
    QStringList forgotten_;           // removed servers' ids
    QListWidget* list_;
    QWidget* editor_;
    QFormLayout* form_;
    QLineEdit* host_;
    QLineEdit* user_;
    QComboBox* auth_;
    QLineEdit* password_;
    QLineEdit* keyFile_;
    QWidget* keyRow_;
};
