#include "app/Prompts.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>

namespace prompts {

bool trust(QWidget* parent, const QString& title, const QString& details, bool* always)
{
    // Not QMessageBox: platform themes may activate the focused button on Enter.
    // No is default and focused, Yes never takes focus (Alt+Y works).
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Security"));
    auto* icon = new QLabel;
    icon->setPixmap(dialog.style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(48, 48));
    auto* text = new QLabel(QStringLiteral("<b>%1</b>").arg(title.toHtmlEscaped()));
    text->setWordWrap(true);
    auto* info = new QLabel(details);
    info->setWordWrap(true);
    info->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto* yes = new QPushButton(QObject::tr("&Yes"));
    auto* no = new QPushButton(QObject::tr("&No"));
    yes->setAutoDefault(false);
    yes->setFocusPolicy(Qt::NoFocus);
    no->setDefault(true);
    QObject::connect(yes, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(no, &QPushButton::clicked, &dialog, &QDialog::reject);

    auto* grid = new QGridLayout(&dialog);
    grid->addWidget(icon, 0, 0, 2, 1, Qt::AlignTop);
    grid->addWidget(text, 0, 1);
    grid->addWidget(info, 1, 1);
    auto* buttons = new QHBoxLayout;
    auto* remember = always ? new QCheckBox(QObject::tr("&Always trust")) : nullptr;
    if (remember) {
        remember->setToolTip(QObject::tr("Don't ask again for this connection (Edit → Security to undo)"));
        buttons->addWidget(remember);
    }
    buttons->addStretch();
    buttons->addWidget(yes);
    buttons->addWidget(no);
    grid->addLayout(buttons, 2, 0, 1, 2);
    grid->setColumnStretch(1, 1);
    dialog.setMinimumWidth(480);
    no->setFocus();
    if (dialog.exec() != QDialog::Accepted) return false;
    if (remember) *always = remember->isChecked();
    return true;
}

bool credentials(QWidget* parent, const QString& prompt, rdc::Credentials& creds)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QObject::tr("Credentials"));
    auto* form = new QFormLayout(&dialog);
    form->addRow(new QLabel(prompt));
    auto* user = new QLineEdit(QString::fromStdString(creds.username));
    auto* domain = new QLineEdit(QString::fromStdString(creds.domain));
    auto* password = new QLineEdit(QString::fromStdString(creds.password));
    password->setEchoMode(QLineEdit::Password);
    form->addRow(QObject::tr("&User name:"), user);
    form->addRow(QObject::tr("&Domain:"), domain);
    form->addRow(QObject::tr("&Password:"), password);
    form->setRowVisible(user, creds.needsUser);
    form->setRowVisible(domain, creds.needsDomain);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    (creds.needsUser && user->text().isEmpty() ? user : password)->setFocus();

    if (dialog.exec() != QDialog::Accepted) return false;
    creds.username = user->text().toStdString();
    creds.domain = domain->text().toStdString();
    creds.password = password->text().toStdString();
    return true;
}

} // namespace prompts
