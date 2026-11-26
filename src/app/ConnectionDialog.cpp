#include "app/ConnectionDialog.h"

#include "app/ConnectionStore.h"
#include "app/Icons.h"
#include "app/Logins.h"
#include "app/Look.h"
#include "app/Secrets.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileInfo>
#include <QDir>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

using rdc::PropertyDef;
using rdc::PropertyKind;
using rdc::Protocol;

std::string connectionSecret(const std::string& id, const std::string& property) { return property.empty() ? id : id + "/" + property; }

// its hint says whether one is saved; an eye shows it, a trash can deletes it
static void secretField(QLineEdit* field, QLabel* hint, const std::string& id, const QString& none)
{
    field->setEchoMode(QLineEdit::Password);
    look::setHint(hint, secrets::saved(id) ? secrets::savedHint(true) : none);
    secrets::attach(field, id, [hint, none] { look::setHint(hint, none); });
}

static QWidget* withBrowse(QLineEdit* edit, bool directory)
{
    auto* box = new QWidget;
    auto* layout = new QHBoxLayout(box);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* button = new QPushButton(QObject::tr("Browse…"));
    layout->addWidget(edit, 1);
    layout->addWidget(button);
    QObject::connect(button, &QPushButton::clicked, edit, [edit, directory] {
        const QString path = directory ? QFileDialog::getExistingDirectory(edit, QString(), edit->text())
                                       : QFileDialog::getOpenFileName(edit, QString(), edit->text());
        if (!path.isEmpty()) edit->setText(path);
    });
    return box;
}

ConnectionDialog::ConnectionDialog(const rdc::Connection& c, const QStringList& groups, QSettings* settings, QWidget* parent)
    : QDialog(parent), original_(c), settings_(settings), sshServers_(loadSshServers(settings))
{
    if (c.id.empty()) { // a new one in a group starts with the group's login and SSH server
        const GroupDefaults defaults = groupDefaults(settings, QString::fromStdString(c.group));
        if (!defaults.login.isEmpty()) original_.options.emplace("login", fromGroup);
        if (!defaults.sshServer.isEmpty())
            for (const char* key : {"tunnel", "jumpHost"}) original_.options.emplace(key, fromGroup);
    }
    setWindowTitle(c.id.empty() ? tr("New Connection") : tr("Edit Connection"));
    tabs_ = new QTabWidget;
    auto* general = new QWidget;
    generalForm_ = new QFormLayout(general);
    tabs_->addTab(general, tr("General"));

    auto text = [](const std::string& s) { return new QLineEdit(QString::fromStdString(s)); };
    name_ = text(c.name);
    group_ = new QComboBox;
    group_->setEditable(true);
    group_->addItems(groups);
    group_->setCurrentText(QString::fromStdString(c.group));
    protocol_ = new QComboBox;
    for (auto [p, label] : {std::pair{Protocol::RDP, "RDP"}, {Protocol::VNC, "VNC"}, {Protocol::SPICE, "SPICE"}, {Protocol::SSH, "SSH"},
                               {Protocol::Android, "Android (scrcpy)"}, {Protocol::Moonlight, "Moonlight (Sunshine, Apollo)"},
                               {Protocol::Telnet, "Telnet"}, {Protocol::Serial, "Serial port"},
                               {Protocol::Local, "Local terminal (this computer)"}})
        if (p != Protocol::Local || c.protocol == Protocol::Local) protocol_->addItem(QString::fromLatin1(label), int(p)); // one, built in
    protocol_->setCurrentIndex(protocol_->findData(int(c.protocol)));
    protocol_->setEnabled(c.id != localTerminalId);
    host_ = text(c.host);
    QStringList ports; // serial devices present now
    for (const QString& pattern : {QStringLiteral("/dev/ttyUSB*"), QStringLiteral("/dev/ttyACM*"), QStringLiteral("/dev/serial/by-id/*")})
        for (const QFileInfo& f : QDir(QFileInfo(pattern).path(), QFileInfo(pattern).fileName(), QDir::Name, QDir::System).entryInfoList())
            ports << f.filePath();
    serialCompleter_ = new QCompleter(ports, this);
    serialCompleter_->setFilterMode(Qt::MatchContains);
    port_ = new QSpinBox;
    port_->setRange(0, 65535);
    port_->setSpecialValueText(tr("Default"));
    port_->setValue(c.port);
    username_ = text(c.username);
    password_ = new QLineEdit;

    generalForm_->addRow(tr("&Name:"), name_);
    generalForm_->addRow(tr("&Group:"), group_);
    generalForm_->addRow(tr("&Protocol:"), protocol_);
    const look::HintedRow host = look::hintedRow(generalForm_, tr("&Host:"), host_);
    hostLabel_ = host.label;
    hostHint_ = host.hint;
    generalForm_->addRow(tr("P&ort:"), port_);
    generalForm_->addRow(tr("&User name:"), username_);
    const look::HintedRow password = look::hintedRow(generalForm_, tr("Pass&word:"), password_);
    passwordLabel_ = password.label;
    secretField(password_, password.hint, connectionSecret(c.id, ""), secrets::savedHint(false));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs_);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // adb serial (empty = the only device); no port, user or password
    validate_ = [this, buttons] {
        const bool android = protocol() == Protocol::Android, serial = protocol() == Protocol::Serial;
        hostLabel_->setText(android || serial ? tr("&Device:") : tr("&Host:"));
        look::setHint(hostHint_, android  ? tr("The serial from \"adb devices\", or host:port. Leave empty for the only connected device.")
                                 : serial ? tr("Such as /dev/ttyUSB0 or /dev/ttyACM0.")
                                          : QString());
        host_->setCompleter(serial ? serialCompleter_ : nullptr);
        const bool loginInside = protocol() == Protocol::Moonlight || protocol() == Protocol::Telnet; // paired, or logs in in the terminal
        const bool local = protocol() == Protocol::Local;
        generalForm_->setRowVisible(hostLabel_, !local);
        generalForm_->setRowVisible(port_, !android && !local && !serial);
        for (QWidget* w : std::initializer_list<QWidget*>{username_, passwordLabel_})
            generalForm_->setRowVisible(w, !android && !loginInside && !local && !serial && !usesLogin());
        buttons->button(QDialogButtonBox::Ok)->setEnabled(android || local || !host_->text().trimmed().isEmpty());
    };
    connect(host_, &QLineEdit::textChanged, this, validate_);
    connect(protocol_, &QComboBox::currentIndexChanged, this, [this] {
        buildPropertyTabs(currentValues());
        validate_();
    });
    connect(group_, &QComboBox::currentTextChanged, this, [this] { // "As the group" names another group's choice
        if (auto it = editors_.find("login"); it != editors_.end()) {
            auto* box = static_cast<QComboBox*>(it->second);
            fillLogin(box, box->currentData().toString());
        }
        for (const auto& [box, first] : serverBoxes_) fillServers(box, first, box->currentData().toString());
        validate_();
    });
    buildPropertyTabs(original_.options);
    validate_();
    resize(760, sizeHint().height());
}

Protocol ConnectionDialog::protocol() const { return Protocol(protocol_->currentData().toInt()); }

void ConnectionDialog::buildPropertyTabs(const std::map<std::string, std::string>& values)
{
    while (tabs_->count() > 1) delete tabs_->widget(1);
    for (QWidget* row : generalRows_) generalForm_->removeRow(row);
    generalRows_.clear();
    editors_.clear();
    serverBoxes_.clear();

    std::map<std::string, QFormLayout*> forms;
    for (const PropertyDef& d : rdc::properties(protocol())) {
        QFormLayout*& form = forms[d.tab];
        if (!form && d.tab == "General") form = generalForm_;
        if (!form) {
            auto* page = new QWidget;
            form = new QFormLayout(page);
            tabs_->addTab(page, tr(d.tab.c_str()));
        }
        const auto it = values.find(d.key);
        const QString value = QString::fromStdString(it != values.end() ? it->second : d.defaultValue);
        const QString label = tr(d.label.c_str());

        QWidget* editor = nullptr;
        QWidget* field = nullptr;
        switch (d.kind) {
        case PropertyKind::Text:
        case PropertyKind::Secret: {
            editor = field = new QLineEdit(d.kind == PropertyKind::Secret ? QString() : value);
            break;
        }
        case PropertyKind::Int: {
            auto* e = new QSpinBox;
            e->setRange(d.min, d.max);
            e->setValue(value.toInt());
            editor = field = e;
            break;
        }
        case PropertyKind::Bool: {
            auto* e = new QCheckBox(label);
            e->setChecked(value == QLatin1String("true"));
            editor = field = e;
            break;
        }
        case PropertyKind::Choice: {
            auto* e = new QComboBox;
            if (d.key == "tunnel" || d.key == "jumpHost") {
                e->setMinimumContentsLength(24);
                serverBoxes_.emplace_back(e, d.choices);
                fillServers(e, d.choices, value);
            } else if (d.key == "login") {
                e->setMinimumContentsLength(24);
                fillLogin(e, value);
                connect(e, &QComboBox::currentIndexChanged, this, [this] { validate_(); });
            } else {
                for (const rdc::Choice& ch : d.choices) e->addItem(tr(ch.label.c_str()), QString::fromStdString(ch.value));
                e->setCurrentIndex(std::max(0, e->findData(value)));
            }
            editor = field = e;
            break;
        }
        case PropertyKind::File:
        case PropertyKind::Directory: {
            auto* e = new QLineEdit(value);
            editor = e;
            field = withBrowse(e, d.kind == PropertyKind::Directory);
            break;
        }
        }
        if (!d.tip.empty()) field->setToolTip(tr(d.tip.c_str()));
        QWidget* rowKey = field; // what removeRow takes
        if (d.kind == PropertyKind::Bool)
#ifdef Q_OS_MACOS
            form->addRow(static_cast<QWidget*>(nullptr), field); // in the fields' column, as AppKit's forms
#else
            form->addRow(field);
#endif
        else if (d.key == "login") { // above the user name it replaces
            int row = 0;
            QFormLayout::ItemRole role;
            generalForm_->getWidgetPosition(username_, &row, &role);
            generalForm_->insertRow(row, label + QLatin1Char(':'), field);
        } else if (d.kind == PropertyKind::Secret) {
            const look::HintedRow hinted = look::hintedRow(form, label + QLatin1Char(':'), field);
            secretField(static_cast<QLineEdit*>(field), hinted.hint, connectionSecret(original_.id, d.key), {});
            rowKey = hinted.label;
        } else if (d.key == "tunnel" || d.key == "jumpHost") {
            rowKey = look::hintedRow(form, label + QLatin1Char(':'), field, tr("New SSH Server… in the list adds one to Settings → SSH Servers.")).label;
        } else {
            form->addRow(label + QLatin1Char(':'), field);
        }
        if (form == generalForm_) generalRows_.push_back(rowKey);
        editors_[d.key] = editor;
    }
}

std::map<std::string, std::string> ConnectionDialog::currentValues() const
{
    std::map<std::string, std::string> values = original_.options;
    for (const auto& [key, editor] : editors_) {
        if (auto* e = qobject_cast<QLineEdit*>(editor)) {
            if (e->echoMode() != QLineEdit::Password) values[key] = e->text().trimmed().toStdString();
        } else if (auto* s = qobject_cast<QSpinBox*>(editor)) {
            values[key] = std::to_string(s->value());
        } else if (auto* b = qobject_cast<QCheckBox*>(editor)) {
            values[key] = b->isChecked() ? "true" : "false";
        } else if (auto* cb = qobject_cast<QComboBox*>(editor)) {
            values[key] = cb->currentData().toString().toStdString();
        }
    }
    return values;
}

void ConnectionDialog::fillServers(QComboBox* box, const std::vector<rdc::Choice>& first, const QString& select)
{
    box->clear();
    const QString groupServer = groupDefaults(settings_, group_->currentText().trimmed()).sshServer;
    const auto server = std::find_if(sshServers_.begin(), sshServers_.end(), [&](const SshServer& s) { return s.id == groupServer; });
    for (const rdc::Choice& ch : first)
        box->addItem(ch.value != fromGroup        ? tr(ch.label.c_str())
                     : server != sshServers_.end() ? tr("As the group: %1").arg(server->name)
                                                   : tr("As the group (it has none)"),
                     QString::fromStdString(ch.value));
    for (const SshServer& s : sshServers_) {
        box->addItem(icons::lucide("server"), s.name, s.id);
        box->setItemData(box->count() - 1, s.summary(), Qt::ToolTipRole);
    }
    if (!select.isEmpty() && box->findData(select) < 0) // imported, or removed in Settings
        box->addItem(QUuid::fromString(select).isNull() ? select : tr("A removed SSH server"), select);
    offerNewSshServer(box, [this, box, first](const QString& chosen, const std::vector<SshServer>& servers) {
        sshServers_ = servers;
        for (const auto& [other, items] : serverBoxes_)
            if (other != box) fillServers(other, items, other->currentData().toString());
        fillServers(box, first, chosen);
    });
    box->setCurrentIndex(std::max(0, box->findData(select)));
}

void ConnectionDialog::fillLogin(QComboBox* box, const QString& select)
{
    box->clear();
    const auto group = findLogin(settings_, groupDefaults(settings_, group_->currentText().trimmed()).login);
    box->addItem(tr("Typed here"), QString());
    box->addItem(group ? tr("As the group: %1").arg(group->name) : tr("As the group (it has none)"), QString::fromLatin1(fromGroup));
    for (const Login& l : loadLogins(settings_)) {
        box->addItem(icons::lucide("key-round"), l.name, l.id);
        box->setItemData(box->count() - 1, l.summary(), Qt::ToolTipRole);
    }
    if (!select.isEmpty() && box->findData(select) < 0) box->addItem(tr("A removed login"), select);
    offerNewLogin(box, [this, box](const QString& chosen) {
        fillLogin(box, chosen);
        validate_();
    });
    box->setCurrentIndex(std::max(0, box->findData(select)));
}

bool ConnectionDialog::usesLogin() const
{
    const auto it = editors_.find("login");
    if (it == editors_.end()) return false;
    QString id = static_cast<QComboBox*>(it->second)->currentData().toString();
    if (id == QLatin1String(fromGroup)) id = groupDefaults(settings_, group_->currentText().trimmed()).login;
    return !id.isEmpty() && findLogin(settings_, id);
}

rdc::Connection ConnectionDialog::connection() const
{
    rdc::Connection c = original_;
    c.protocol = protocol();
    c.host = host_->text().trimmed().toStdString();
    c.name = !name_->text().trimmed().isEmpty() ? name_->text().trimmed().toStdString()
           : !c.host.empty()                    ? c.host
                                                : tr("Android device").toStdString();
    c.group = group_->currentText().trimmed().toStdString();
    c.port = port_->value();
    c.username = username_->text().trimmed().toStdString();
    c.password.clear();
    c.options.clear();
    for (const auto& [key, value] : currentValues())
        if (editors_.count(key)) c.options[key] = value; // only keys of the chosen protocol
    return c;
}

std::map<std::string, QString> ConnectionDialog::secrets() const
{
    std::map<std::string, QString> result;
    if (!password_->text().isEmpty()) result[""] = password_->text();
    for (const auto& [key, editor] : editors_)
        if (auto* e = qobject_cast<QLineEdit*>(editor); e && e->echoMode() == QLineEdit::Password && !e->text().isEmpty())
            result[key] = e->text();
    return result;
}
