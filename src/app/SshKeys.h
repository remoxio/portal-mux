#pragma once

#include <QString>

class QWidget;

namespace sshkeys {

// a public key from ~/.ssh, or a new id_ed25519 made there; the .pub's path, empty if cancelled
QString pick(QWidget* parent, const QString& host);
// one libssh and OpenSSH try without being told
bool triedByDefault(const QString& publicKeyPath);

} // namespace sshkeys
