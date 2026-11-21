#pragma once

#include "app/SshServers.h"
#include "core/Connection.h"

#include <QDialog>

#include <map>
#include <vector>

class QComboBox;
class QFormLayout;
class QLineEdit;
class QSpinBox;
class QTabWidget;

// a connection's saved secret's name; property "": its password
std::string connectionSecret(const std::string& id, const std::string& property);

// One tab per property group of the protocol (Properties.h)
class ConnectionDialog : public QDialog {
    Q_OBJECT

public:
    ConnectionDialog(const rdc::Connection& connection, const QStringList& groups, std::vector<SshServer> sshServers,
                     QWidget* parent = nullptr);

    rdc::Connection connection() const;
    // Typed secrets by property key; "" = connection password
    std::map<std::string, QString> secrets() const;

private:
    void buildPropertyTabs(const std::map<std::string, std::string>& values);
    std::map<std::string, std::string> currentValues() const;
    rdc::Protocol protocol() const;
    void fillServers(QComboBox* box, const QStringList& noneLabels, const QString& select); // the SSH tunnel's or jump host's

    rdc::Connection original_;
    std::vector<SshServer> sshServers_; // the SSH tunnel's choices
    std::vector<std::pair<QComboBox*, QStringList>> serverBoxes_; // with their "none" labels
    QTabWidget* tabs_;
    QFormLayout* generalForm_;
    QLineEdit *name_, *host_, *username_, *password_;
    class QCompleter* serialCompleter_;
    QComboBox *group_, *protocol_;
    QSpinBox* port_;
    std::map<std::string, QWidget*> editors_;
    std::vector<QWidget*> generalRows_;
};
