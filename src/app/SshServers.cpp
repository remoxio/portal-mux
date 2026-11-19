#include "app/SshServers.h"

#include "app/ConnectionStore.h"
#include "app/Icons.h"
#include "app/PageRail.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

static std::string keychainKey(const QString& id) { return "ssh-server/" + id.toStdString(); }

SshServersPage::SshServersPage(QSettings* settings, QWidget* parent) : QWidget(parent), settings_(settings)
{
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    list_ = new QListWidget;
    auto* add = new QToolButton;
    add->setText(QStringLiteral("+"));
    add->setToolTip(tr("Add an SSH server"));
    auto* remove = new QToolButton;
    remove->setText(QStringLiteral("−"));
    for (QToolButton* b : {add, remove}) b->setAutoRaise(true);
    remove->setToolTip(tr("Remove the selected SSH server"));
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(0);
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addStretch();
    auto* left = new QVBoxLayout;
    left->addWidget(pageRail(list_), 1);
    left->addLayout(buttons);

    host_ = new QLineEdit;
    host_->setPlaceholderText(tr("hostname or hostname:port"));
    user_ = new QLineEdit;
    user_->setPlaceholderText(tr("Empty = your user name here"));
    auth_ = new QComboBox;
    auth_->addItem(tr("Password"), QStringLiteral("password"));
    auth_->addItem(tr("Public key"), QStringLiteral("publicKey"));
    password_ = new QLineEdit;
    password_->setEchoMode(QLineEdit::Password);
    keyFile_ = new QLineEdit;
    keyFile_->setPlaceholderText(tr("Empty = SSH agent and ~/.ssh default keys"));
    auto* browse = new QPushButton(tr("Browse…"));
    keyRow_ = new QWidget;
    auto* keyLayout = new QHBoxLayout(keyRow_);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->addWidget(keyFile_, 1);
    keyLayout->addWidget(browse);
    editor_ = new QWidget;
    form_ = new QFormLayout(editor_);
    form_->setContentsMargins(0, 0, 0, 0);
    form_->addRow(tr("&Host:"), host_);
    form_->addRow(tr("&User name:"), user_);
    form_->addRow(tr("&Authentication:"), auth_);
    form_->addRow(tr("&Password:"), password_);
    form_->addRow(tr("Private &key:"), keyRow_);

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    layout->addLayout(left);
    auto* right = new QVBoxLayout;
    right->addWidget(editor_);
    right->addStretch();
    layout->addLayout(right, 1);

    connect(list_, &QListWidget::currentRowChanged, this, &SshServersPage::show);
    connect(list_, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) { // renamed in the list
        const int row = list_->row(item);
        if (row < 0 || row >= int(servers_.size())) return;
        const QString name = item->text().trimmed();
        if (name.isEmpty()) return item->setText(servers_[row].name);
        servers_[row].name = name;
        save();
        emit changed();
    });
    auto edited = [this](QString Server::*field, QLineEdit* edit) {
        connect(edit, &QLineEdit::textChanged, this, [this, field](const QString& text) {
            if (Server* s = current()) {
                s->*field = text.trimmed();
                save();
            }
        });
    };
    edited(&Server::host, host_);
    edited(&Server::user, user_);
    edited(&Server::keyFile, keyFile_);
    connect(auth_, &QComboBox::currentIndexChanged, this, [this] {
        if (Server* s = current()) {
            s->auth = auth_->currentData().toString();
            save();
            emit changed();
        }
        showAuth();
    });
    connect(password_, &QLineEdit::textEdited, this, [this](const QString& text) {
        if (shownId_.isEmpty()) return;
        if (text.isEmpty()) {
            passwords_.remove(shownId_);
        } else {
            passwords_[shownId_] = text;
        }
        emit changed();
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, tr("Private Key"), keyFile_->text().isEmpty() ? QDir::homePath() + QStringLiteral("/.ssh") : keyFile_->text());
        if (!path.isEmpty()) keyFile_->setText(path);
    });
    connect(add, &QToolButton::clicked, this, &SshServersPage::add);
    connect(remove, &QToolButton::clicked, this, &SshServersPage::remove);

    load();
    list_->setCurrentRow(servers_.empty() ? -1 : 0);
    show(list_->currentRow());
}

SshServersPage::Server* SshServersPage::current()
{
    const int row = list_->currentRow();
    return row >= 0 && row < int(servers_.size()) ? &servers_[row] : nullptr;
}

void SshServersPage::load()
{
    const int n = settings_->beginReadArray(QStringLiteral("sshServers"));
    for (int i = 0; i < n; ++i) {
        settings_->setArrayIndex(i);
        Server s;
        s.id = settings_->value(QStringLiteral("id")).toString();
        s.name = settings_->value(QStringLiteral("name")).toString();
        s.host = settings_->value(QStringLiteral("host")).toString();
        s.user = settings_->value(QStringLiteral("user")).toString();
        s.auth = settings_->value(QStringLiteral("auth"), s.auth).toString();
        s.keyFile = settings_->value(QStringLiteral("keyFile")).toString();
        if (!s.id.isEmpty()) servers_.push_back(s);
    }
    settings_->endArray();
    const QSignalBlocker block(list_);
    for (const Server& s : servers_) {
        auto* item = new QListWidgetItem(icons::lucide("server"), s.name, list_);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
    }
}

void SshServersPage::save() const
{
    settings_->remove(QStringLiteral("sshServers"));
    settings_->beginWriteArray(QStringLiteral("sshServers"), int(servers_.size()));
    for (int i = 0; i < int(servers_.size()); ++i) {
        const Server& s = servers_[i];
        settings_->setArrayIndex(i);
        settings_->setValue(QStringLiteral("id"), s.id);
        settings_->setValue(QStringLiteral("name"), s.name);
        settings_->setValue(QStringLiteral("host"), s.host);
        settings_->setValue(QStringLiteral("user"), s.user);
        settings_->setValue(QStringLiteral("auth"), s.auth);
        settings_->setValue(QStringLiteral("keyFile"), s.keyFile);
    }
    settings_->endArray();
}

void SshServersPage::show(int row)
{
    const Server* s = row >= 0 && row < int(servers_.size()) ? &servers_[row] : nullptr;
    const QSignalBlocker blockHost(host_), blockUser(user_), blockAuth(auth_), blockKey(keyFile_);
    host_->setText(s ? s->host : QString());
    user_->setText(s ? s->user : QString());
    auth_->setCurrentIndex(std::max(0, auth_->findData(s ? s->auth : QStringLiteral("password"))));
    keyFile_->setText(s ? s->keyFile : QString());
    password_->setText(s ? passwords_.value(s->id) : QString());
    password_->setPlaceholderText({});
    editor_->setEnabled(s);
    shownId_ = s ? s->id : QString();
    showAuth();
    if (!s) return;
    secrets::read(keychainKey(s->id), this, [this, id = s->id](const QString& password) {
        if (const Server* now = current(); now && now->id == id)
            password_->setPlaceholderText(password.isEmpty() ? tr("Ask when connecting") : tr("Stored in the keychain"));
    });
}

void SshServersPage::showAuth()
{
    const bool key = auth_->currentData().toString() == QLatin1String("publicKey");
    form_->setRowVisible(password_, !key);
    form_->setRowVisible(keyRow_, key);
}

void SshServersPage::add()
{
    Server s;
    s.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    s.name = tr("New SSH Server");
    servers_.push_back(s);
    save();
    emit changed();
    auto* item = new QListWidgetItem(icons::lucide("server"), s.name);
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    {
        const QSignalBlocker block(list_);
        list_->addItem(item);
    }
    list_->setCurrentItem(item);
    list_->editItem(item);
}

void SshServersPage::remove()
{
    const int row = list_->currentRow();
    const Server* s = current();
    if (!s) return;
    QMessageBox box(QMessageBox::Question, tr("Remove SSH Server"), tr("Remove \"%1\"? Its saved password goes too.").arg(s->name),
                    QMessageBox::Yes | QMessageBox::No, this);
    box.setDefaultButton(QMessageBox::No);
    box.setOption(QMessageBox::Option::DontUseNativeDialog); // KDE's dialog: Enter presses the first button
    if (box.exec() != QMessageBox::Yes) return;
    passwords_.remove(s->id);
    forgotten_ << s->id;
    shownId_.clear();
    servers_.erase(servers_.begin() + row);
    save();
    emit changed();
    delete list_->takeItem(row);
    show(list_->currentRow());
}

void SshServersPage::apply()
{
    for (const QString& id : std::as_const(forgotten_)) secrets::remove(keychainKey(id));
    for (auto it = passwords_.cbegin(); it != passwords_.cend(); ++it)
        // qApp: OK closes the dialog
        secrets::write(keychainKey(it.key()), it.value(), qApp, [page = QPointer<SshServersPage>(this)](const QString& error) {
            if (!error.isEmpty())
                QMessageBox::warning(page, tr("Keychain"), tr("The password could not be stored in the system keychain: %1").arg(error));
        });
    if (!passwords_.isEmpty() && passwords_.contains(shownId_)) {
        password_->clear();
        password_->setPlaceholderText(tr("Stored in the keychain"));
    }
    passwords_.clear();
    forgotten_.clear();
}
