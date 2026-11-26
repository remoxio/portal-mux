#include "app/Logins.h"

#include "app/CardRow.h"
#include "app/Icons.h"
#include "app/Look.h"
#include "app/Prompts.h"
#include "app/Secrets.h"
#include "core/Properties.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

std::string loginSecret(const QString& id) { return "login/" + id.toStdString(); }

QString Login::summary() const { return domain.isEmpty() ? user : domain + QLatin1Char('\\') + user; }

std::vector<Login> loadLogins(QSettings* settings)
{
    std::vector<Login> logins;
    const int n = settings->beginReadArray(QStringLiteral("logins"));
    for (int i = 0; i < n; ++i) {
        settings->setArrayIndex(i);
        Login l{settings->value(QStringLiteral("id")).toString(), settings->value(QStringLiteral("name")).toString(),
                settings->value(QStringLiteral("user")).toString(), settings->value(QStringLiteral("domain")).toString()};
        if (!l.id.isEmpty()) logins.push_back(l);
    }
    settings->endArray();
    return logins;
}

std::optional<Login> findLogin(QSettings* settings, const QString& id)
{
    for (const Login& l : loadLogins(settings))
        if (l.id == id) return l;
    return std::nullopt;
}

std::vector<std::pair<QString, GroupDefaults>> allGroupDefaults(QSettings* settings)
{
    std::vector<std::pair<QString, GroupDefaults>> all;
    const int n = settings->beginReadArray(QStringLiteral("groupDefaults"));
    for (int i = 0; i < n; ++i) {
        settings->setArrayIndex(i);
        all.emplace_back(settings->value(QStringLiteral("group")).toString(),
                         GroupDefaults{settings->value(QStringLiteral("login")).toString(), settings->value(QStringLiteral("sshServer")).toString()});
    }
    settings->endArray();
    return all;
}

GroupDefaults groupDefaults(QSettings* settings, const QString& group)
{
    for (const auto& [name, defaults] : allGroupDefaults(settings))
        if (name == group) return defaults;
    return {};
}

void setGroupDefaults(QSettings* settings, const QString& group, const GroupDefaults& defaults)
{
    auto all = allGroupDefaults(settings);
    std::erase_if(all, [&](const auto& entry) { return entry.first == group; });
    if (!defaults.login.isEmpty() || !defaults.sshServer.isEmpty()) all.emplace_back(group, defaults);
    settings->remove(QStringLiteral("groupDefaults"));
    settings->beginWriteArray(QStringLiteral("groupDefaults"), int(all.size()));
    for (int i = 0; i < int(all.size()); ++i) {
        settings->setArrayIndex(i);
        settings->setValue(QStringLiteral("group"), all[size_t(i)].first);
        settings->setValue(QStringLiteral("login"), all[size_t(i)].second.login);
        settings->setValue(QStringLiteral("sshServer"), all[size_t(i)].second.sshServer);
    }
    settings->endArray();
}

rdc::Connection withDefaults(QSettings* settings, rdc::Connection c, std::string* passwordSecret)
{
    const GroupDefaults group = groupDefaults(settings, QString::fromStdString(c.group));
    for (const char* key : {"tunnel", "jumpHost"})
        if (c.option(key) == fromGroup) c.options[key] = group.sshServer.toStdString();
    const std::string login = c.option("login") == fromGroup ? group.login.toStdString() : c.option("login");
    passwordSecret->clear();
    if (const auto l = findLogin(settings, QString::fromStdString(login))) { // a removed one: the connection's own
        c.username = l->user.toStdString();
        if (!l->domain.isEmpty() && rdc::findProperty(c.protocol, "domain")) c.options["domain"] = l->domain.toStdString();
        *passwordSecret = loginSecret(l->id);
    }
    return c;
}

namespace {

QSettings* editorSettings = nullptr;
std::function<void(QWidget*)> editor;
constexpr int maxRows = 8;

} // namespace

void setLoginsEditor(QSettings* settings, std::function<void(QWidget*)> open)
{
    editorSettings = settings;
    editor = std::move(open);
}

void offerNewLogin(QComboBox* box, std::function<void(const QString& select)> refill)
{
    if (!editor) return;
    offerNew(box, LoginsPage::tr("New Login…"), [] {
        QStringList ids;
        for (const Login& l : loadLogins(editorSettings)) ids << l.id;
        return ids;
    }, editor, std::move(refill));
}

LoginsPage::LoginsPage(QSettings* settings, QWidget* parent) : QWidget(parent), settings_(settings)
{
    list_ = new QListWidget;
    list_->setItemDelegate(new CardRow({{"trash", tr("Remove"), [this](int row) { remove(row); }},
                                        {"pencil", tr("Edit"), [this](int row) { edit(row); }}}, // from the right
                                       list_));
    new QShortcut(QKeySequence::Delete, list_, [this] { remove(list_->currentRow()); }, Qt::WidgetShortcut);
    list_->setFrameShape(QFrame::NoFrame);
    list_->setAttribute(Qt::WA_MacShowFocusRect, false);
    list_->viewport()->setAutoFillBackground(false);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    empty_ = new QLabel(tr("No logins yet"));
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setEnabled(false); // dimmed
    empty_->setMinimumHeight(56);
    auto* hint = new QLabel(tr("A user name and password that many connections use: choose it under Login in a connection, "
                               "or for a whole group in its Group Settings. A password change is then made once, here."));
    hint->setWordWrap(true);
    auto* add = new QPushButton(QStringLiteral("+"));
    add->setToolTip(tr("Add a login"));
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(add);
    auto* card = new QGroupBox;
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->addWidget(hint);
    cardLayout->addWidget(list_);
    cardLayout->addWidget(empty_);
    cardLayout->addLayout(buttons);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(card);

    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { edit(list_->row(item)); });
    connect(add, &QPushButton::clicked, this, [this] { edit(-1); });

    logins_ = loadLogins(settings_);
    showList();
    list_->setCurrentRow(logins_.empty() ? -1 : 0);
}

void saveLogins(QSettings* settings, const std::vector<Login>& logins)
{
    settings->remove(QStringLiteral("logins"));
    settings->beginWriteArray(QStringLiteral("logins"), int(logins.size()));
    for (int i = 0; i < int(logins.size()); ++i) {
        const Login& l = logins[size_t(i)];
        settings->setArrayIndex(i);
        settings->setValue(QStringLiteral("id"), l.id);
        settings->setValue(QStringLiteral("name"), l.name);
        settings->setValue(QStringLiteral("user"), l.user);
        settings->setValue(QStringLiteral("domain"), l.domain);
    }
    settings->endArray();
}

void LoginsPage::save() const { saveLogins(settings_, logins_); }

void LoginsPage::showList()
{
    const int row = list_->currentRow();
    list_->clear();
    for (const Login& l : logins_) (new QListWidgetItem(icons::lucide("key-round"), l.name, list_))->setData(Qt::UserRole, l.summary());
    const int rows = std::min(int(logins_.size()), maxRows); // more scroll
    list_->setFixedHeight(rows ? rows * list_->sizeHintForRow(0) : 0);
    list_->setVisible(rows);
    empty_->setVisible(!rows);
    list_->setCurrentRow(std::min(row, int(logins_.size()) - 1));
}

void LoginsPage::edit(int row)
{
    const bool adding = row < 0 || row >= int(logins_.size());
    Login l;
    if (adding) l.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    else l = logins_[size_t(row)];

    auto* sheet = new QDialog(this);
    sheet->setAttribute(Qt::WA_DeleteOnClose);
    sheet->setWindowTitle(adding ? tr("New Login") : tr("Login"));
#ifdef Q_OS_MACOS
    sheet->setWindowFlag(Qt::Sheet);
#endif
    auto* name = new QLineEdit(l.name);
    auto* user = new QLineEdit(l.user);
    user->setMinimumWidth(user->fontMetrics().averageCharWidth() * 40);
    auto* domain = new QLineEdit(l.domain);
    auto* password = new QLineEdit(passwords_.value(l.id));
    password->setEchoMode(QLineEdit::Password);
    auto* form = new QFormLayout;
    look::hintedRow(form, tr("&Name:"), name, tr("Leave empty to show the user name."));
    form->addRow(tr("&User name:"), user);
    look::hintedRow(form, tr("&Domain:"), domain, tr("Windows (RDP) only. Leave empty for the connection's."));
    QLabel* passwordHint = look::hintedRow(form, tr("&Password:"), password, secrets::savedHint(!passwords_.contains(l.id) && secrets::saved(loginSecret(l.id)))).hint;
    secrets::attach(password, loginSecret(l.id), [this, passwordHint, id = l.id] {
        passwords_.remove(id);
        look::setHint(passwordHint, secrets::savedHint(false));
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
    ok->setEnabled(!l.user.isEmpty());
    connect(user, &QLineEdit::textChanged, ok, [ok](const QString& text) { ok->setEnabled(!text.trimmed().isEmpty()); });
    connect(buttons, &QDialogButtonBox::accepted, sheet, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, sheet, &QDialog::reject);
    auto* layout = new QVBoxLayout(sheet);
#ifdef Q_OS_MACOS
    auto* heading = new QLabel(sheet->windowTitle()); // a sheet has no title bar
    QFont bold = heading->font();
    bold.setWeight(QFont::DemiBold);
    heading->setFont(bold);
    layout->addWidget(heading);
#endif
    layout->addLayout(form);
    layout->addWidget(buttons);

    connect(sheet, &QDialog::accepted, this, [=, this]() mutable {
        l.user = user->text().trimmed();
        l.domain = domain->text().trimmed();
        l.name = name->text().trimmed().isEmpty() ? l.summary() : name->text().trimmed();
        if (!password->text().isEmpty()) passwords_[l.id] = password->text();
        if (adding) {
            logins_.push_back(l);
            row = int(logins_.size()) - 1;
        } else {
            logins_[size_t(row)] = l;
        }
        save();
        showList();
        list_->setCurrentRow(row);
        emit changed();
    });
    sheet->open();
}

void LoginsPage::remove(int row)
{
    if (row < 0 || row >= int(logins_.size())) return;
    const Login& l = logins_[size_t(row)];
    if (!prompts::confirm(this, tr("Remove Login"),
                          tr("Remove \"%1\"? Its saved password goes too; connections that use it ask for their own.").arg(l.name),
                          tr("&Remove")))
        return;
    passwords_.remove(l.id);
    forgotten_ << l.id;
    logins_.erase(logins_.begin() + row);
    save();
    showList();
    emit changed();
}

void LoginsPage::apply()
{
    for (const QString& id : std::as_const(forgotten_)) secrets::remove(loginSecret(id));
    for (auto it = passwords_.cbegin(); it != passwords_.cend(); ++it)
        // qApp: OK closes the dialog
        secrets::write(loginSecret(it.key()), it.value(), qApp, [page = QPointer<LoginsPage>(this)](const QString& error) {
            if (!error.isEmpty()) QMessageBox::warning(page, tr("Passwords"), tr("The password was not saved: %1").arg(error));
        });
    passwords_.clear();
    forgotten_.clear();
}
