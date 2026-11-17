#include "app/ConnectionDialog.h"

#include "core/Properties.h"

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
#include <QVBoxLayout>

using rdc::PropertyDef;
using rdc::PropertyKind;
using rdc::Protocol;

static constexpr const char* secretPlaceholder = "Unchanged";

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

ConnectionDialog::ConnectionDialog(const rdc::Connection& c, const QStringList& groups, QWidget* parent)
    : QDialog(parent), original_(c)
{
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
        protocol_->addItem(QString::fromLatin1(label), int(p));
    protocol_->setCurrentIndex(protocol_->findData(int(c.protocol)));
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
    password_->setEchoMode(QLineEdit::Password);
    password_->setPlaceholderText(c.id.empty() ? tr("Ask when connecting") : tr(secretPlaceholder));

    generalForm_->addRow(tr("&Name:"), name_);
    generalForm_->addRow(tr("&Group:"), group_);
    generalForm_->addRow(tr("&Protocol:"), protocol_);
    generalForm_->addRow(tr("&Host:"), host_);
    generalForm_->addRow(tr("P&ort:"), port_);
    generalForm_->addRow(tr("&User name:"), username_);
    generalForm_->addRow(tr("Pass&word:"), password_);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tabs_);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    // adb serial (empty = the only device); no port, user or password
    auto validate = [this, buttons] {
        const bool android = protocol() == Protocol::Android, serial = protocol() == Protocol::Serial;
        if (auto* label = qobject_cast<QLabel*>(generalForm_->labelForField(host_)))
            label->setText(android || serial ? tr("&Device:") : tr("&Host:"));
        host_->setPlaceholderText(android  ? tr("Serial from \"adb devices\" or host:port; empty = the only connected device")
                                  : serial ? tr("/dev/ttyUSB0, /dev/ttyACM0…")
                                           : QString());
        host_->setCompleter(serial ? serialCompleter_ : nullptr);
        const bool loginInside = protocol() == Protocol::Moonlight || protocol() == Protocol::Telnet; // paired, or logs in in the terminal
        const bool local = protocol() == Protocol::Local;
        generalForm_->setRowVisible(host_, !local);
        generalForm_->setRowVisible(port_, !android && !local && !serial);
        for (QWidget* w : std::initializer_list<QWidget*>{username_, password_})
            generalForm_->setRowVisible(w, !android && !loginInside && !local && !serial);
        buttons->button(QDialogButtonBox::Ok)->setEnabled(android || local || !host_->text().trimmed().isEmpty());
    };
    connect(host_, &QLineEdit::textChanged, this, validate);
    connect(protocol_, &QComboBox::currentIndexChanged, this, [this, validate] {
        buildPropertyTabs(currentValues());
        validate();
    });
    validate();
    buildPropertyTabs(c.options);
    resize(760, sizeHint().height());
}

Protocol ConnectionDialog::protocol() const { return Protocol(protocol_->currentData().toInt()); }

void ConnectionDialog::buildPropertyTabs(const std::map<std::string, std::string>& values)
{
    while (tabs_->count() > 1) delete tabs_->widget(1);
    for (QWidget* row : generalRows_) generalForm_->removeRow(row);
    generalRows_.clear();
    editors_.clear();

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
            auto* e = new QLineEdit(d.kind == PropertyKind::Secret ? QString() : value);
            if (d.kind == PropertyKind::Secret) {
                e->setEchoMode(QLineEdit::Password);
                e->setPlaceholderText(original_.id.empty() ? QString() : tr(secretPlaceholder));
            }
            editor = field = e;
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
            for (const rdc::Choice& ch : d.choices) e->addItem(tr(ch.label.c_str()), QString::fromStdString(ch.value));
            e->setCurrentIndex(std::max(0, e->findData(value)));
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
        if (d.kind == PropertyKind::Bool)
            form->addRow(field);
        else
            form->addRow(label + QLatin1Char(':'), field);
        if (form == generalForm_) generalRows_.push_back(field);
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
