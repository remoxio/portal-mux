#pragma once

#include "app/SshServers.h"
#include "core/Connection.h"

#include <QDialog>

#include <map>

class QComboBox;
class QFormLayout;
class QLineEdit;
class QSpinBox;
class QTabWidget;

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

    rdc::Connection original_;
    std::vector<SshServer> sshServers_; // the SSH tunnel's choices
    QTabWidget* tabs_;
    QFormLayout* generalForm_;
    QLineEdit *name_, *host_, *username_, *password_;
    class QCompleter* serialCompleter_;
    QComboBox *group_, *protocol_;
    QSpinBox* port_;
    std::map<std::string, QWidget*> editors_;
    std::vector<QWidget*> generalRows_;
};
