#pragma once

#include "app/SshServers.h"
#include "core/Properties.h"

#include <QDialog>

#include <functional>
#include <map>
#include <vector>

class QComboBox;
class QFormLayout;
class QLineEdit;
class QSettings;
class QSpinBox;
class QTabWidget;

// a connection's saved secret's name; property "": its password
std::string connectionSecret(const std::string& id, const std::string& property);

// One tab per property group of the protocol (Properties.h)
class ConnectionDialog : public QDialog {
    Q_OBJECT

public:
    // settings: SSH servers, logins and group defaults
    ConnectionDialog(const rdc::Connection& connection, const QStringList& groups, QSettings* settings, QWidget* parent = nullptr);

    rdc::Connection connection() const;
    // Typed secrets by property key; "" = connection password
    std::map<std::string, QString> secrets() const;

private:
    void buildPropertyTabs(const std::map<std::string, std::string>& values);
    std::map<std::string, std::string> currentValues() const;
    rdc::Protocol protocol() const;
    // the SSH tunnel's or jump host's: `first` (none, as the group), then the servers
    void fillServers(QComboBox* box, const std::vector<rdc::Choice>& first, const QString& select);
    void fillLogin(QComboBox* box, const QString& select);
    bool usesLogin() const; // a saved login's user name and password, not the fields'

    rdc::Connection original_;
    QSettings* settings_;
    std::vector<SshServer> sshServers_; // the SSH tunnel's choices
    std::vector<std::pair<QComboBox*, std::vector<rdc::Choice>>> serverBoxes_; // with their first items
    std::function<void()> validate_;
    QTabWidget* tabs_;
    QFormLayout* generalForm_;
    QLineEdit *name_, *host_, *username_, *password_;
    class QCompleter* serialCompleter_;
    QComboBox *group_, *protocol_;
    QSpinBox* port_;
    std::map<std::string, QWidget*> editors_;
    std::vector<QWidget*> generalRows_;
};
