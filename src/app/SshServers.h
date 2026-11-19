#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <vector>

class QLabel;
class QListWidget;
class QPushButton;
class QSettings;

// Settings → SSH Servers; passwords reach the keychain on apply()
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
        QString summary() const; // user@host · authentication
    };

    void load();
    void save() const;
    void showList();
    void edit(int row); // -1: a new one
    void remove();

    QSettings* settings_;
    std::vector<Server> servers_;
    QMap<QString, QString> passwords_; // typed, by server id
    QStringList forgotten_;           // removed servers' ids
    QListWidget* list_;
    QLabel* empty_;
    QPushButton *edit_, *remove_;
};
