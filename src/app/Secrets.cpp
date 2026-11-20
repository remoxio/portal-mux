#include "app/Secrets.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <qt6keychain/keychain.h>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace secrets {
namespace {

const QString service = QStringLiteral("portal-mux");
const QString keyEntry = QStringLiteral("passwords-key");
constexpr int rounds = 600000; // PBKDF2-HMAC-SHA256, as OWASP advises
const QByteArray checkName = "check";

QString tr(const char* text, int n = -1) { return QCoreApplication::translate("secrets", text, nullptr, n); }

const unsigned char* data(const QByteArray& b) { return reinterpret_cast<const unsigned char*>(b.constData()); }
unsigned char* data(QByteArray& b) { return reinterpret_cast<unsigned char*>(b.data()); }

QByteArray randomBytes(int n)
{
    QByteArray bytes(n, '\0');
    RAND_bytes(data(bytes), n);
    return bytes;
}

QByteArray derive(const QString& master, const QByteArray& salt, int iterations)
{
    const QByteArray password = master.toUtf8();
    QByteArray key(32, '\0');
    PKCS5_PBKDF2_HMAC(password.constData(), int(password.size()), data(salt), int(salt.size()), iterations, EVP_sha256(), 32, data(key));
    return key;
}

// nonce, ciphertext, tag; `name` as associated data: a value can't move to another entry
QByteArray seal(const QByteArray& key, const QByteArray& name, const QByteArray& plain)
{
    const QByteArray nonce = randomBytes(12);
    QByteArray text(plain.size(), '\0'), tag(16, '\0');
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int n = 0;
    EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, data(key), data(nonce));
    EVP_EncryptUpdate(ctx, nullptr, &n, data(name), int(name.size()));
    EVP_EncryptUpdate(ctx, data(text), &n, data(plain), int(plain.size()));
    EVP_EncryptFinal_ex(ctx, data(text) + n, &n);
    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data());
    EVP_CIPHER_CTX_free(ctx);
    return nonce + text + tag;
}

std::optional<QByteArray> open(const QByteArray& key, const QByteArray& name, const QByteArray& sealed)
{
    if (key.size() != 32 || sealed.size() < 28) return std::nullopt;
    const QByteArray nonce = sealed.left(12), text = sealed.mid(12, sealed.size() - 28);
    QByteArray plain(text.size(), '\0'), tag = sealed.right(16);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int n = 0;
    const bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, data(key), data(nonce)) == 1 &&
                    EVP_DecryptUpdate(ctx, nullptr, &n, data(name), int(name.size())) == 1 &&
                    EVP_DecryptUpdate(ctx, data(plain), &n, data(text), int(text.size())) == 1 &&
                    EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag.data()) == 1 &&
                    EVP_DecryptFinal_ex(ctx, data(plain) + n, &n) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return std::nullopt;
    return plain;
}

struct Vault {
    QByteArray salt, check; // no salt: no master password yet
    int rounds = 0;
    std::map<QString, QByteArray> passwords; // sealed
    std::optional<QByteArray> key;
    bool keychainRead = false;
    std::vector<std::function<void()>> waiting, afterPrompt;
    bool prompting = false, declined = false;
};

QString path() { return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/passwords.json"); }

Vault& vault()
{
    static Vault v = [] {
        Vault v;
        QFile file(path());
        if (!file.open(QIODevice::ReadOnly)) return v;
        QJsonParseError error;
        const QJsonObject o = QJsonDocument::fromJson(file.readAll(), &error).object();
        if (error.error != QJsonParseError::NoError) {
            QFile::copy(path(), path() + QStringLiteral(".broken")); // save() would destroy it
            return v;
        }
        const QJsonObject master = o.value(QStringLiteral("master")).toObject();
        v.salt = QByteArray::fromHex(master.value(QStringLiteral("salt")).toString().toLatin1());
        v.rounds = master.value(QStringLiteral("rounds")).toInt();
        v.check = QByteArray::fromHex(o.value(QStringLiteral("check")).toString().toLatin1());
        const QJsonObject passwords = o.value(QStringLiteral("passwords")).toObject();
        for (auto it = passwords.begin(); it != passwords.end(); ++it)
            v.passwords[it.key()] = QByteArray::fromHex(it.value().toString().toLatin1());
        return v;
    }();
    return v;
}

bool save(QString* error)
{
    const Vault& v = vault();
    QJsonObject passwords;
    for (const auto& [name, sealed] : v.passwords) passwords[name] = QString::fromLatin1(sealed.toHex());
    const QJsonObject o{{QStringLiteral("master"), QJsonObject{{QStringLiteral("salt"), QString::fromLatin1(v.salt.toHex())},
                                                              {QStringLiteral("rounds"), v.rounds}}},
                        {QStringLiteral("check"), QString::fromLatin1(v.check.toHex())},
                        {QStringLiteral("passwords"), passwords}};
    QDir().mkpath(QFileInfo(path()).path());
    QSaveFile file(path());
    if (!file.open(QIODevice::WriteOnly) || file.write(QJsonDocument(o).toJson()) < 0 || !file.commit()) {
        *error = file.errorString();
        return false;
    }
    QFile::setPermissions(path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

void keep(const QByteArray& key) // unless the keychain refuses: then asked again next time
{
    auto* job = new QKeychain::WritePasswordJob(service);
    job->setAutoDelete(true);
    job->setKey(keyEntry);
    job->setTextData(QString::fromLatin1(key.toHex()));
    job->start();
}

void removeOld(const std::string& id) // a keychain entry from before passwords.json
{
    auto* job = new QKeychain::DeletePasswordJob(service);
    job->setAutoDelete(true);
    job->setKey(QString::fromStdString(id));
    job->start();
}

// the keychain's key, if it opens the vault; `done` runs later
void loadKey(std::function<void()> done)
{
    Vault& v = vault();
    if (v.key || v.keychainRead) return QTimer::singleShot(0, qApp, std::move(done));
    v.waiting.push_back(std::move(done));
    if (v.waiting.size() > 1) return;
    auto finish = std::make_shared<std::function<void(QString)>>([](const QString& hex) {
        Vault& v = vault();
        v.keychainRead = true;
        if (const QByteArray key = QByteArray::fromHex(hex.toLatin1()); !v.check.isEmpty() && open(key, checkName, v.check)) v.key = key;
        for (auto& f : std::exchange(v.waiting, {})) f();
    });
    // a keychain that never answers (crashed, stuck) must not block connecting
    QTimer::singleShot(20000, qApp, [finish] {
        if (auto f = std::exchange(*finish, nullptr)) f({});
    });
    auto* job = new QKeychain::ReadPasswordJob(service);
    job->setAutoDelete(true);
    job->setKey(keyEntry);
    QObject::connect(job, &QKeychain::Job::finished, qApp, [job, finish] {
        if (auto f = std::exchange(*finish, nullptr)) f(job->error() == QKeychain::NoError ? job->textData() : QString());
    });
    job->start();
}

// asks to unlock when `prompt`; false if still locked or there is no master password
// the asker's window, else the active one, else the main one: a prompt without a parent can open behind the window it blocks
QWidget* promptParent(QObject* context)
{
    if (auto* widget = qobject_cast<QWidget*>(context); widget && widget->window()->isVisible()) return widget->window();
    if (QWidget* active = QApplication::activeWindow()) return active;
    for (QWidget* top : QApplication::topLevelWidgets())
        if (top->inherits("QMainWindow") && top->isVisible()) return top;
    return nullptr;
}

void withKey(QObject* context, bool prompt, std::function<void(bool)> done)
{
    loadKey([ctx = QPointer<QObject>(context), prompt, done] {
        if (!ctx) return;
        Vault& v = vault();
        if (!v.key && prompt && !v.salt.isEmpty() && !v.declined) {
            if (v.prompting) return v.afterPrompt.push_back([ctx, done] {
                if (ctx) done(bool(vault().key));
            });
            unlock(promptParent(ctx));
        }
        done(bool(v.key));
    });
}

void applyMaster(const QString& master)
{
    Vault& v = vault();
    const QByteArray salt = randomBytes(16), key = derive(master, salt, rounds);
    for (auto& [name, sealed] : v.passwords)
        if (const auto plain = v.key ? open(*v.key, name.toUtf8(), sealed) : std::nullopt) sealed = seal(key, name.toUtf8(), *plain);
    v.salt = salt;
    v.rounds = rounds;
    v.check = seal(key, checkName, checkName);
    v.key = key;
    keep(key);
}

bool tryMaster(const QString& master)
{
    Vault& v = vault();
    const QByteArray key = derive(master, v.salt, v.rounds);
    if (!open(key, checkName, v.check)) return false;
    v.key = key;
    keep(key);
    return true;
}

// choose: a new one, typed twice; else the one that unlocks
bool ask(QWidget* parent, bool choose)
{
    Vault& v = vault();
    QDialog dialog(parent);
    dialog.setWindowTitle(!choose ? tr("Unlock Saved Passwords") : v.salt.isEmpty() ? tr("Master Password") : tr("Change Master Password"));
    auto* text = new QLabel(choose ? tr("Saved passwords are encrypted with the master password. This computer's keychain keeps its key, "
                                        "so it is asked for only where that is missing. If it is forgotten, only the saved passwords are lost.")
                                   : tr("This computer's keychain has no key for the saved passwords. Enter the master password."));
    text->setWordWrap(true);
    auto* first = new QLineEdit;
    auto* again = new QLineEdit;
    for (QLineEdit* e : {first, again}) e->setEchoMode(QLineEdit::Password);
    auto* form = new QFormLayout;
    form->addRow(choose ? tr("New master password:") : tr("Master password:"), first);
    if (choose) form->addRow(tr("Again:"), again);
    else delete again;
    auto* wrong = new QLabel(tr("The master password is wrong."));
    QPalette red = wrong->palette();
    red.setColor(QPalette::WindowText, QColor(0xe0, 0x40, 0x40));
    wrong->setPalette(red);
    wrong->hide();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
    auto valid = [=] { ok->setEnabled(!first->text().isEmpty() && (!choose || again->text() == first->text())); };
    valid();
    QObject::connect(first, &QLineEdit::textChanged, ok, valid);
    if (choose) QObject::connect(again, &QLineEdit::textChanged, ok, valid);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        const bool right = choose || tryMaster(first->text());
        if (choose) applyMaster(first->text());
        QApplication::restoreOverrideCursor();
        if (right) return dialog.accept();
        wrong->show();
        first->selectAll();
        first->setFocus();
    });
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(text);
    layout->addLayout(form);
    layout->addWidget(wrong);
    layout->addWidget(buttons);
    dialog.setFixedWidth(460);
    const bool done = dialog.exec() == QDialog::Accepted;
    if (!done) { // once per batch of requests
        v.declined = true;
        QTimer::singleShot(0, qApp, [] { vault().declined = false; });
    }
    return done;
}

} // namespace

void read(const std::string& id, QObject* context, std::function<void(QString)> done)
{
    const QString name = QString::fromStdString(id);
    if (vault().passwords.count(name)) {
        withKey(context, true, [name, done](bool ready) {
            const auto plain = ready ? open(*vault().key, name.toUtf8(), vault().passwords[name]) : std::nullopt;
            done(plain ? QString::fromUtf8(*plain) : QString());
        });
        return;
    }
    // from before passwords.json: moved in once the master password is there
    auto once = std::make_shared<std::function<void(QString)>>([id, name, ctx = QPointer<QObject>(context), done](const QString& value) {
        if (!ctx) return;
        if (value.isEmpty()) return done({});
        withKey(ctx, false, [id, name, value, done](bool ready) {
            if (ready) {
                vault().passwords[name] = seal(*vault().key, name.toUtf8(), value.toUtf8());
                if (QString error; save(&error)) removeOld(id);
            }
            done(value);
        });
    });
    auto finish = [once](const QString& value) {
        if (auto f = std::exchange(*once, nullptr)) f(value);
    };
    QTimer::singleShot(20000, qApp, [finish] { finish({}); });
    auto* job = new QKeychain::ReadPasswordJob(service);
    job->setAutoDelete(true);
    job->setKey(name);
    QObject::connect(job, &QKeychain::Job::finished, qApp, [job, finish] {
        finish(job->error() == QKeychain::NoError ? job->textData() : QString());
    });
    job->start();
}

void write(const std::string& id, const QString& password, QObject* context, std::function<void(QString)> done)
{
    withKey(context, true, [id, password, done, ctx = QPointer<QObject>(context)](bool ready) {
        Vault& v = vault();
        if (!ready && v.salt.isEmpty() && !v.declined) ask(promptParent(ctx), true);
        if (!v.key) return done(v.salt.isEmpty() ? tr("No master password was chosen.") : tr("The saved passwords are locked."));
        const QString name = QString::fromStdString(id);
        v.passwords[name] = seal(*v.key, name.toUtf8(), password.toUtf8());
        QString error;
        if (!save(&error)) return done(error);
        removeOld(id);
        done({});
    });
}

void remove(const std::string& id)
{
    if (QString error; vault().passwords.erase(QString::fromStdString(id))) save(&error);
    removeOld(id);
}

void status(QObject* context, std::function<void(Status)> done)
{
    withKey(context, false, [done](bool ready) {
        const Vault& v = vault();
        done({!v.salt.isEmpty(), !v.salt.isEmpty() && !ready, int(v.passwords.size())});
    });
}

bool unlock(QWidget* parent)
{
    Vault& v = vault();
    if (v.key || v.salt.isEmpty()) return bool(v.key);
    v.prompting = true;
    const bool done = ask(parent, false);
    v.prompting = false;
    for (auto& f : std::exchange(v.afterPrompt, {})) f();
    return done;
}

bool setMaster(QWidget* parent)
{
    Vault& v = vault();
    if (!v.salt.isEmpty() && !v.key && !unlock(parent)) return false; // resealed with the new key: the old one first
    if (!ask(parent, true)) return false;
    if (QString error; !save(&error)) {
        QMessageBox::warning(parent, tr("Master Password"), tr("The saved passwords could not be written: %1").arg(error));
        return false;
    }
    return true;
}

QWidget* page()
{
    auto* card = new QGroupBox(tr("Master password"));
    auto* state = new QLabel;
    state->setWordWrap(true);
    auto* set = new QPushButton;
    auto* unlockButton = new QPushButton(tr("Unlock…"));
    auto* buttons = new QHBoxLayout;
    buttons->addWidget(set);
    buttons->addWidget(unlockButton);
    buttons->addStretch();
    auto* layout = new QVBoxLayout(card);
    layout->addWidget(state);
    layout->addLayout(buttons);
    auto show = [=] {
        status(card, [=](Status s) {
            state->setText(!s.master  ? tr("No master password yet: you choose one when a password is first saved.")
                           : s.locked ? tr("%n saved password(s), locked: this computer's keychain has no key for them.", s.count)
                                      : tr("%n saved password(s), encrypted with the master password.", s.count));
            set->setText(s.master ? tr("Change Master Password…") : tr("Set Master Password…"));
            unlockButton->setVisible(s.locked);
        });
    };
    QObject::connect(set, &QPushButton::clicked, card, [=] {
        setMaster(card->window());
        show();
    });
    QObject::connect(unlockButton, &QPushButton::clicked, card, [=] {
        unlock(card->window());
        show();
    });
    show();
    return card;
}

} // namespace secrets
