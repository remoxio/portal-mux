#include "app/SshKeys.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QSysInfo>

#include <libssh/libssh.h>

namespace sshkeys {

static QString sshDir()
{
    return QDir::homePath() + QStringLiteral("/.ssh");
}

bool triedByDefault(const QString& publicKeyPath)
{
    const QString name = QFileInfo(publicKeyPath).fileName();
    return name == QLatin1String("id_ed25519.pub") || name == QLatin1String("id_ecdsa.pub") || name == QLatin1String("id_rsa.pub");
}

// as ssh-keygen -t ed25519 makes it: the private key 0600 in OpenSSH's format, the .pub beside it
static QString generate(QWidget* parent)
{
    const QString title = QObject::tr("New Key");
    bool ok = false;
    const QString passphrase = QInputDialog::getText(parent, title, QObject::tr("A passphrase for the new key, asked when it is used (empty: none):"),
                                                     QLineEdit::Password, {}, &ok);
    if (!ok) return {};
    if (!passphrase.isEmpty() &&
        QInputDialog::getText(parent, title, QObject::tr("The passphrase again:"), QLineEdit::Password, {}, &ok) != passphrase) {
        if (ok) QMessageBox::warning(parent, title, QObject::tr("The passphrases differ; no key was made."));
        return {};
    }
    ssh_key key = nullptr;
    char* privateText = nullptr;
    char* publicText = nullptr;
    const QByteArray secret = passphrase.toUtf8();
    const bool made = ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) == SSH_OK &&
                      ssh_pki_export_privkey_base64(key, secret.isEmpty() ? nullptr : secret.constData(), nullptr, nullptr, &privateText) == SSH_OK &&
                      ssh_pki_export_pubkey_base64(key, &publicText) == SSH_OK;
    const QByteArray privateKey = made ? QByteArray(privateText) : QByteArray();
    const QByteArray publicKey = made ? "ssh-ed25519 " + QByteArray(publicText) + " " + qgetenv(qEnvironmentVariableIsSet("USER") ? "USER" : "USERNAME") +
                                            "@" + QSysInfo::machineHostName().toUtf8() + "\n"
                                      : QByteArray();
    ssh_string_free_char(privateText);
    ssh_string_free_char(publicText);
    ssh_key_free(key);

    const QString dir = sshDir();
    if (!QFileInfo::exists(dir) && QDir().mkpath(dir)) QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    QFile privateFile(dir + QStringLiteral("/id_ed25519")), publicFile(privateFile.fileName() + QStringLiteral(".pub"));
    const bool written = made && privateFile.open(QIODevice::WriteOnly | QIODevice::NewOnly, QFile::ReadOwner | QFile::WriteOwner) &&
                         privateFile.write(privateKey) == privateKey.size() && privateFile.flush() &&
                         publicFile.open(QIODevice::WriteOnly | QIODevice::NewOnly) && publicFile.write(publicKey) == publicKey.size() &&
                         publicFile.flush();
    if (!written) {
        const QString error = made ? (privateFile.isOpen() ? publicFile : privateFile).errorString() : QObject::tr("libssh could not make the key");
        if (privateFile.isOpen()) privateFile.remove(); // a key without its .pub, or half written
        if (publicFile.isOpen()) publicFile.remove();
        QMessageBox::warning(parent, title, QObject::tr("No key was made: %1").arg(error));
        return {};
    }
    return publicFile.fileName();
}

QString pick(QWidget* parent, const QString& host)
{
    const QString dir = sshDir();
    QStringList keys = QDir(dir).entryList({QStringLiteral("*.pub")}, QDir::Files, QDir::Name);
    keys.removeIf([](const QString& name) { return name.endsWith(QLatin1String("-cert.pub")); });
    const QString make = QObject::tr("A new key (id_ed25519)");
    if (!QFileInfo::exists(dir + QStringLiteral("/id_ed25519"))) keys << make;
    bool ok = false;
    const QString chosen = QInputDialog::getItem(parent, QObject::tr("Copy My Key to Server"),
                                                 QObject::tr("Which of your SSH keys should %1 let you in with?").arg(host), keys,
                                                 std::max(0, int(keys.indexOf(QStringLiteral("id_ed25519.pub")))), false, &ok);
    if (!ok || chosen.isEmpty()) return {};
    return chosen == make ? generate(parent) : dir + QLatin1Char('/') + chosen;
}

} // namespace sshkeys
