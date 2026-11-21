#pragma once

#include <QString>

#include <functional>
#include <string>

class QLineEdit;
class QObject;
class QWidget;

// Saved passwords: AES-256-GCM in passwords.json, keyed by the master password; the keychain keeps
// the derived key, so the master password is asked for only where it is missing
namespace secrets {

// `done` runs on the UI thread; empty = no password
void read(const std::string& id, QObject* context, std::function<void(QString password)> done);
// the first one asks to choose a master password
void write(const std::string& id, const QString& password, QObject* context, std::function<void(QString error)> done);
void remove(const std::string& id);
bool saved(const std::string& id); // without unlocking anything
// A password field's saved password: an eye shows it once the master password is typed again, a trash can deletes it
// after asking. The eye shows typed text at once. deleted: after a deletion.
void attach(QLineEdit* field, const std::string& id, std::function<void()> deleted = {});

struct Status {
    bool master = false, locked = false;
    int count = 0;
};
void status(QObject* context, std::function<void(Status)> done);
bool unlock(QWidget* parent);
bool setMaster(QWidget* parent); // also changes it
QWidget* page();                 // Settings → Passwords

} // namespace secrets
