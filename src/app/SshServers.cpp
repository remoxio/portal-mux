#include "app/SshServers.h"

#include "app/ConnectionStore.h"
#include "app/Icons.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QStyledItemDelegate>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>

static std::string keychainKey(const QString& id) { return "ssh-server/" + id.toStdString(); }

namespace {

constexpr int iconSize = 20, gap = 10, maxRows = 8;

// the name over user@host, as System Settings' rows; a line between rows
class ServerRow : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    static QFont small(QFont font)
    {
        font.setPointSizeF(font.pointSizeF() * 0.85);
        return font;
    }

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override
    {
        return {0, option.fontMetrics.height() + QFontMetrics(small(option.font)).height() + 14};
    }

    void paint(QPainter* p, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        QStyleOptionViewItem o = option;
        initStyleOption(&o, index);
        const QRectF r = o.rect;
        QColor text = o.palette.color(QPalette::WindowText), dim = text;
        dim.setAlphaF(0.55);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const qreal textLeft = r.left() + 8 + iconSize + gap;
        if (o.state & QStyle::State_Selected) { // the accent also when the window isn't active, as AppKit's lists
            p->setPen(Qt::NoPen);
            p->setBrush(o.palette.color(QPalette::Active, QPalette::Accent));
            p->drawRoundedRect(r.adjusted(0, 1, 0, -1), 6, 6);
            text = dim = o.palette.color(QPalette::Active, QPalette::HighlightedText);
            dim.setAlphaF(0.8);
        } else if (index.row() > 0) {
            QColor line = text;
            line.setAlphaF(0.1);
            p->fillRect(QRectF(textLeft, r.top(), r.right() - textLeft - 8, 1), line);
        }
        QPixmap icon = o.icon.pixmap(QSize(iconSize, iconSize), p->device()->devicePixelRatioF());
        if (!icon.isNull()) { // in the text's colour
            QPainter tint(&icon);
            tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tint.fillRect(icon.rect(), text);
        }
        p->drawPixmap(QPointF(r.left() + 8, r.center().y() - iconSize / 2.0), icon);
        const QFont smallFont = small(o.font);
        const int nameHeight = o.fontMetrics.height(), lineHeight = QFontMetrics(smallFont).height();
        const qreal top = r.center().y() - (nameHeight + lineHeight) / 2.0, width = r.right() - textLeft - 8;
        p->setPen(text);
        p->setFont(o.font);
        p->drawText(QRectF(textLeft, top, width, nameHeight), Qt::AlignLeft | Qt::AlignVCenter,
                    o.fontMetrics.elidedText(o.text, Qt::ElideRight, int(width)));
        p->setPen(dim);
        p->setFont(smallFont);
        p->drawText(QRectF(textLeft, top + nameHeight, width, lineHeight), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(smallFont).elidedText(index.data(Qt::UserRole).toString(), Qt::ElideRight, int(width)));
        p->restore();
    }
};

} // namespace

QString SshServersPage::Server::summary() const
{
    const QString where = user.isEmpty() ? host : user + QLatin1Char('@') + host;
    return where + QStringLiteral(" · ") + (auth == QLatin1String("publicKey") ? tr("Public key") : tr("Password"));
}

SshServersPage::SshServersPage(QSettings* settings, QWidget* parent) : QWidget(parent), settings_(settings)
{
    list_ = new QListWidget;
    list_->setItemDelegate(new ServerRow(list_));
    list_->setFrameShape(QFrame::NoFrame);
    list_->setAttribute(Qt::WA_MacShowFocusRect, false);
    list_->viewport()->setAutoFillBackground(false);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed); // as its rows: the card doesn't grow
    empty_ = new QLabel(tr("No SSH servers yet"));
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setEnabled(false); // dimmed
    empty_->setMinimumHeight(56);
    edit_ = new QPushButton(tr("Edit…"));
    auto* add = new QPushButton(QStringLiteral("+"));
    add->setToolTip(tr("Add an SSH server"));
    remove_ = new QPushButton(QStringLiteral("−"));
    remove_->setToolTip(tr("Remove the selected SSH server"));
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(edit_);
    buttons->addWidget(add);
    buttons->addWidget(remove_);
    auto* card = new QGroupBox;
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->addWidget(list_);
    cardLayout->addWidget(empty_);
    cardLayout->addLayout(buttons);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(card);

    connect(list_, &QListWidget::currentRowChanged, this, [this](int row) {
        edit_->setEnabled(row >= 0);
        remove_->setEnabled(row >= 0);
    });
    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { edit(list_->row(item)); });
    connect(edit_, &QPushButton::clicked, this, [this] { edit(list_->currentRow()); });
    connect(add, &QPushButton::clicked, this, [this] { edit(-1); });
    connect(remove_, &QPushButton::clicked, this, &SshServersPage::remove);

    load();
    showList();
    list_->setCurrentRow(servers_.empty() ? -1 : 0);
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

void SshServersPage::showList()
{
    const int row = list_->currentRow();
    list_->clear();
    for (const Server& s : servers_) (new QListWidgetItem(icons::lucide("server"), s.name, list_))->setData(Qt::UserRole, s.summary());
    const int rows = std::min(int(servers_.size()), maxRows); // more scroll
    list_->setFixedHeight(rows ? rows * list_->sizeHintForRow(0) : 0);
    list_->setVisible(rows);
    empty_->setVisible(!rows);
    list_->setCurrentRow(std::min(row, int(servers_.size()) - 1));
}

void SshServersPage::edit(int row)
{
    const bool adding = row < 0 || row >= int(servers_.size());
    Server s;
    if (adding) s.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    else s = servers_[row];

    auto* sheet = new QDialog(this);
    sheet->setAttribute(Qt::WA_DeleteOnClose);
    sheet->setWindowTitle(adding ? tr("New SSH Server") : tr("SSH Server"));
#ifdef Q_OS_MACOS
    sheet->setWindowFlag(Qt::Sheet);
#endif
    auto* name = new QLineEdit(s.name);
    name->setPlaceholderText(tr("As the host"));
    auto* host = new QLineEdit(s.host);
    host->setPlaceholderText(tr("hostname or hostname:port"));
    host->setMinimumWidth(host->fontMetrics().averageCharWidth() * 40);
    auto* user = new QLineEdit(s.user);
    user->setPlaceholderText(tr("Empty = your user name here"));
    auto* auth = new QComboBox;
    auth->addItem(tr("Password"), QStringLiteral("password"));
    auth->addItem(tr("Public key"), QStringLiteral("publicKey"));
    auth->setCurrentIndex(std::max(0, auth->findData(s.auth)));
    auto* password = new QLineEdit(passwords_.value(s.id));
    password->setEchoMode(QLineEdit::Password);
    password->setPlaceholderText(tr("Ask when connecting"));
    if (!adding && !passwords_.contains(s.id))
        secrets::read(keychainKey(s.id), password, [password](const QString& stored) {
            if (!stored.isEmpty()) password->setPlaceholderText(tr("Stored in the keychain"));
        });
    auto* keyFile = new QLineEdit(s.keyFile);
    keyFile->setPlaceholderText(tr("Empty = SSH agent and ~/.ssh default keys"));
    auto* browse = new QPushButton(tr("Browse…"));
    auto* keyRow = new QWidget;
    auto* keyLayout = new QHBoxLayout(keyRow);
    keyLayout->setContentsMargins(0, 0, 0, 0);
    keyLayout->addWidget(keyFile, 1);
    keyLayout->addWidget(browse);
    auto* form = new QFormLayout;
    form->addRow(tr("&Name:"), name);
    form->addRow(tr("&Host:"), host);
    form->addRow(tr("&User name:"), user);
    form->addRow(tr("&Authentication:"), auth);
    form->addRow(tr("&Password:"), password);
    form->addRow(tr("Private &key:"), keyRow);
    auto showAuth = [form, auth, password, keyRow] {
        const bool key = auth->currentData().toString() == QLatin1String("publicKey");
        form->setRowVisible(password, !key);
        form->setRowVisible(keyRow, key);
    };
    showAuth();
    connect(auth, &QComboBox::currentIndexChanged, sheet, showAuth);
    connect(browse, &QPushButton::clicked, sheet, [sheet, keyFile] {
        const QString path = QFileDialog::getOpenFileName(sheet, tr("Private Key"), keyFile->text().isEmpty() ? QDir::homePath() + QStringLiteral("/.ssh") : keyFile->text());
        if (!path.isEmpty()) keyFile->setText(path);
    });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    QPushButton* ok = buttons->button(QDialogButtonBox::Ok);
    ok->setEnabled(!s.host.isEmpty());
    connect(host, &QLineEdit::textChanged, ok, [ok](const QString& text) { ok->setEnabled(!text.trimmed().isEmpty()); });
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
        s.host = host->text().trimmed();
        s.name = name->text().trimmed().isEmpty() ? s.host : name->text().trimmed();
        s.user = user->text().trimmed();
        s.auth = auth->currentData().toString();
        s.keyFile = keyFile->text().trimmed();
        if (!password->text().isEmpty()) passwords_[s.id] = password->text();
        if (adding) {
            servers_.push_back(s);
            row = int(servers_.size()) - 1;
        } else {
            servers_[row] = s;
        }
        save();
        showList();
        list_->setCurrentRow(row);
        emit changed();
    });
    sheet->open();
}

void SshServersPage::remove()
{
    const int row = list_->currentRow();
    if (row < 0 || row >= int(servers_.size())) return;
    const Server& s = servers_[row];
    QMessageBox box(QMessageBox::Question, tr("Remove SSH Server"), tr("Remove \"%1\"? Its saved password goes too.").arg(s.name),
                    QMessageBox::Yes | QMessageBox::No, this);
    box.setDefaultButton(QMessageBox::No);
    box.setOption(QMessageBox::Option::DontUseNativeDialog); // KDE's dialog: Enter presses the first button
    if (box.exec() != QMessageBox::Yes) return;
    passwords_.remove(s.id);
    forgotten_ << s.id;
    servers_.erase(servers_.begin() + row);
    save();
    showList();
    emit changed();
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
    passwords_.clear();
    forgotten_.clear();
}
