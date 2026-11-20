#include "app/SshServers.h"

#include "app/ConnectionStore.h"
#include "app/Icons.h"
#include "app/Look.h"
#include "app/Prompts.h"
#include "ssh/SshConnection.h"
#include "ssh/SshTunnel.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QStyledItemDelegate>
#include <QToolTip>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <thread>

std::string sshServerSecret(const QString& id) { return "ssh-server/" + id.toStdString(); }

namespace {

QSettings* editorSettings = nullptr;
std::function<void(QWidget*)> editor;
constexpr int NewServerRole = Qt::UserRole + 100; // the "New SSH Server…" item; its data stays empty

} // namespace

void setSshServersEditor(QSettings* settings, std::function<void(QWidget*)> open)
{
    editorSettings = settings;
    editor = std::move(open);
}

void offerNewSshServer(QComboBox* box, SshServersRefill refill)
{
    if (!editor) return;
    if (box->count() > 0) box->insertSeparator(box->count());
    box->addItem(icons::tabler("plus"), SshServersPage::tr("New SSH Server…"));
    box->setItemData(box->count() - 1, true, NewServerRole);
    if (box->property("offersNewSshServer").toBool()) return; // connected when first offered
    box->setProperty("offersNewSshServer", true);
    QObject::connect(box, &QComboBox::currentIndexChanged, box, [box](int index) {
        if (!box->itemData(index, NewServerRole).toBool()) box->setProperty("chosenSshServer", box->itemData(index));
    });
    QObject::connect(box, &QComboBox::activated, box, [box, refill](int index) {
        if (!box->itemData(index, NewServerRole).toBool()) return;
        QStringList before;
        for (const SshServer& s : loadSshServers(editorSettings)) before << s.id;
        const QString chosen = box->property("chosenSshServer").toString();
        editor(box->window());
        const std::vector<SshServer> servers = loadSshServers(editorSettings);
        QString select = chosen;
        for (const SshServer& s : servers)
            if (!before.contains(s.id)) select = s.id;
        refill(select, servers);
    });
}

namespace {

constexpr int iconSize = 20, gap = 10, maxRows = 8, editSize = 16, editBox = 28;

// One login on a worker, its prompts over `dialogs`; done(error) on the UI thread, "" = logged in; stops with its parent
class SshTest : public QObject, private rdc::SessionListener {
public:
    SshTest(rdc::SshConnection::Params params, QWidget* dialogs, std::function<void(QString)> done)
        : QObject(dialogs), dialogs_(dialogs), prompts_(this, [] {}), ssh_(*this, std::move(params))
    {
        worker_ = std::jthread([this, done = std::move(done)](std::stop_token stop) {
            const bool ok = ssh_.open([&] { return stop.stop_requested(); });
            const QString error = ok ? QString() : ssh_.error().empty() ? SshServersPage::tr("Cancelled.") : QString::fromStdString(ssh_.error());
            QMetaObject::invokeMethod(this, [this, done, error] {
                done(error);
                deleteLater();
            }, Qt::QueuedConnection);
        });
    }
    ~SshTest() override
    {
        prompts_.close();
        worker_.request_stop();
        ssh_.abort();
        worker_.join();
    }

private:
    void onStateChanged(rdc::SessionState) override {}
    void onError(std::string) override {}
    bool askTrust(const std::string& title, const std::string& details, bool* always) override
    {
        return prompts_.ask([this, t = QString::fromStdString(title), d = QString::fromStdString(details), always] {
            return prompts::trust(dialogs_, t, d, always);
        });
    }
    bool askCredentials(const std::string& prompt, rdc::Credentials& creds) override
    {
        return prompts_.ask([this, p = QString::fromStdString(prompt), &creds] { return prompts::credentials(dialogs_, p, creds); });
    }

    QWidget* dialogs_;
    PromptBridge prompts_;
    rdc::SshConnection ssh_;
    std::jthread worker_;
};

// name over user@host, a line between rows, icon buttons on the right
class ServerRow : public QStyledItemDelegate {
public:
    struct Button {
        const char* icon; // Tabler
        const char* tip;  // QT_TRANSLATE_NOOP("SshServersPage", …)
        std::function<void(int row)> run;
    };
    ServerRow(std::vector<Button> buttons, QObject* parent) : QStyledItemDelegate(parent), buttons_(std::move(buttons)) {}

    static QRectF buttonRect(const QRectF& r, size_t i) // from the right
    {
        return {r.right() - 6 - editBox * qreal(i + 1), r.center().y() - editBox / 2.0, qreal(editBox), qreal(editBox)};
    }
    int buttonAt(const QRectF& r, QPointF pos) const
    {
        for (size_t i = 0; i < buttons_.size(); ++i)
            if (buttonRect(r, i).contains(pos)) return int(i);
        return -1;
    }

    bool editorEvent(QEvent* event, QAbstractItemModel* model, const QStyleOptionViewItem& option, const QModelIndex& index) override
    {
        const auto type = event->type();
        if (type == QEvent::MouseButtonRelease || type == QEvent::MouseButtonDblClick) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (const int b = buttonAt(option.rect, mouse->position()); mouse->button() == Qt::LeftButton && b >= 0) {
                if (type == QEvent::MouseButtonRelease) buttons_[size_t(b)].run(index.row());
                return true; // not activated as well
            }
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }

    bool helpEvent(QHelpEvent* event, QAbstractItemView* view, const QStyleOptionViewItem& option, const QModelIndex& index) override
    {
        const int b = event->type() == QEvent::ToolTip ? buttonAt(option.rect, event->pos()) : -1;
        if (b < 0) return QStyledItemDelegate::helpEvent(event, view, option, index);
        QToolTip::showText(event->globalPos(), SshServersPage::tr(buttons_[size_t(b)].tip), view, buttonRect(option.rect, size_t(b)).toRect());
        return true;
    }

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
        if (o.state & QStyle::State_Selected) {
            look::paintSelection(p, r.adjusted(0, 1, 0, -1), o.palette, 6);
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
        const qreal top = r.center().y() - (nameHeight + lineHeight) / 2.0, width = buttonRect(r, buttons_.size() - 1).left() - gap - textLeft;
        p->setPen(text);
        p->setFont(o.font);
        p->drawText(QRectF(textLeft, top, width, nameHeight), Qt::AlignLeft | Qt::AlignVCenter,
                    o.fontMetrics.elidedText(o.text, Qt::ElideRight, int(width)));
        p->setPen(dim);
        p->setFont(smallFont);
        p->drawText(QRectF(textLeft, top + nameHeight, width, lineHeight), Qt::AlignLeft | Qt::AlignVCenter,
                    QFontMetrics(smallFont).elidedText(index.data(Qt::UserRole).toString(), Qt::ElideRight, int(width)));
        for (size_t i = 0; i < buttons_.size(); ++i) {
            QPixmap glyph = icons::tabler(buttons_[i].icon).pixmap(QSize(editSize, editSize), p->device()->devicePixelRatioF());
            QPainter tint(&glyph);
            tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tint.fillRect(glyph.rect(), dim);
            tint.end();
            p->drawPixmap(buttonRect(r, i).center() - QPointF(editSize, editSize) / 2, glyph);
        }
        p->restore();
    }

private:
    std::vector<Button> buttons_;
};

} // namespace

QString SshServer::address() const { return user.isEmpty() ? host : user + QLatin1Char('@') + host; }

QString SshServer::summary() const
{
    return address() + QStringLiteral(" · ") + (auth == QLatin1String("publicKey") ? SshServersPage::tr("Public key") : SshServersPage::tr("Password"));
}

std::vector<SshServer> loadSshServers(QSettings* settings)
{
    std::vector<SshServer> servers;
    const int n = settings->beginReadArray(QStringLiteral("sshServers"));
    for (int i = 0; i < n; ++i) {
        settings->setArrayIndex(i);
        SshServer s;
        s.id = settings->value(QStringLiteral("id")).toString();
        s.name = settings->value(QStringLiteral("name")).toString();
        s.host = settings->value(QStringLiteral("host")).toString();
        s.user = settings->value(QStringLiteral("user")).toString();
        s.auth = settings->value(QStringLiteral("auth"), s.auth).toString();
        s.keyFile = settings->value(QStringLiteral("keyFile")).toString();
        if (!s.id.isEmpty()) servers.push_back(s);
    }
    settings->endArray();
    return servers;
}

std::optional<SshServer> findSshServer(QSettings* settings, const QString& id)
{
    for (const SshServer& s : loadSshServers(settings))
        if (s.id == id) return s;
    return std::nullopt;
}

SshServersPage::SshServersPage(QSettings* settings, QWidget* parent) : QWidget(parent), settings_(settings)
{
    list_ = new QListWidget;
    list_->setItemDelegate(new ServerRow({{"pencil", QT_TRANSLATE_NOOP("SshServersPage", "Edit"), [this](int row) { edit(row); }},
                                          {"trash", QT_TRANSLATE_NOOP("SshServersPage", "Remove"), [this](int row) { remove(row); }}},
                                         list_));
    new QShortcut(QKeySequence::Delete, list_, [this] { remove(list_->currentRow()); }, Qt::WidgetShortcut);
    list_->setFrameShape(QFrame::NoFrame);
    list_->setAttribute(Qt::WA_MacShowFocusRect, false);
    list_->viewport()->setAutoFillBackground(false);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed); // as its rows: the card doesn't grow
    empty_ = new QLabel(tr("No SSH servers yet"));
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setEnabled(false); // dimmed
    empty_->setMinimumHeight(56);
    auto* add = new QPushButton(QStringLiteral("+"));
    add->setToolTip(tr("Add an SSH server"));
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(add);
    auto* card = new QGroupBox;
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->addWidget(list_);
    cardLayout->addWidget(empty_);
    cardLayout->addLayout(buttons);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(card);

    connect(list_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) { edit(list_->row(item)); });
    connect(add, &QPushButton::clicked, this, [this] { edit(-1); });

    servers_ = loadSshServers(settings_);
    showList();
    list_->setCurrentRow(servers_.empty() ? -1 : 0);
}

void SshServersPage::save() const
{
    settings_->remove(QStringLiteral("sshServers"));
    settings_->beginWriteArray(QStringLiteral("sshServers"), int(servers_.size()));
    for (int i = 0; i < int(servers_.size()); ++i) {
        const SshServer& s = servers_[i];
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
    for (const SshServer& s : servers_) (new QListWidgetItem(icons::lucide("server"), s.name, list_))->setData(Qt::UserRole, s.summary());
    const int rows = std::min(int(servers_.size()), maxRows); // more scroll
    list_->setFixedHeight(rows ? rows * list_->sizeHintForRow(0) : 0);
    list_->setVisible(rows);
    empty_->setVisible(!rows);
    list_->setCurrentRow(std::min(row, int(servers_.size()) - 1));
}

void SshServersPage::edit(int row)
{
    const bool adding = row < 0 || row >= int(servers_.size());
    SshServer s;
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
        secrets::read(sshServerSecret(s.id), password, [password](const QString& stored) {
            if (!stored.isEmpty()) password->setPlaceholderText(tr("Saved"));
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
    auto* test = new QPushButton(tr("&Test Connection"));
    test->setToolTip(tr("Logs in once with these values; nothing is saved"));
    test->setEnabled(!s.host.isEmpty());
    connect(host, &QLineEdit::textChanged, ok, [ok, test](const QString& text) {
        ok->setEnabled(!text.trimmed().isEmpty());
        test->setEnabled(!text.trimmed().isEmpty());
    });
    connect(test, &QPushButton::clicked, sheet, [=, this] {
        const QString who = user->text().trimmed();
        rdc::SshConnection::Params p = rdc::SshTunnel::parse(((who.isEmpty() ? QString() : who + QLatin1Char('@')) + host->text().trimmed()).toStdString());
        p.knownHostsFile = (QFileInfo(settings_->fileName()).absolutePath() + QStringLiteral("/known_hosts")).toStdString(); // the app's
        const bool key = auth->currentData().toString() == QLatin1String("publicKey");
        if (key) p.keyFile = keyFile->text().trimmed().toStdString();
        const QString where = QString::fromStdString(p.user.empty() ? p.host : p.user + "@" + p.host);
        auto run = [=](const QString& saved) mutable {
            if (!key) p.password = (password->text().isEmpty() ? saved : password->text()).toStdString();
            test->setEnabled(false);
            test->setText(tr("Testing…"));
            new SshTest(p, sheet, [=](const QString& error) {
                test->setEnabled(true);
                test->setText(tr("&Test Connection"));
                if (error.isEmpty()) QMessageBox::information(sheet, tr("Test Connection"), tr("Logged in to %1.").arg(where));
                else QMessageBox::warning(sheet, tr("Test Connection"), error);
            });
        };
        if (!key && password->text().isEmpty() && !adding) secrets::read(sshServerSecret(s.id), sheet, run);
        else run({});
    });
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
    auto* bottom = new QHBoxLayout; // the test on the left, as a dialog's extra button
    bottom->addWidget(test);
    bottom->addStretch();
    bottom->addWidget(buttons);
    layout->addLayout(bottom);

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

void SshServersPage::remove(int row)
{
    if (row < 0 || row >= int(servers_.size())) return;
    const SshServer& s = servers_[row];
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
    for (const QString& id : std::as_const(forgotten_)) secrets::remove(sshServerSecret(id));
    for (auto it = passwords_.cbegin(); it != passwords_.cend(); ++it)
        // qApp: OK closes the dialog
        secrets::write(sshServerSecret(it.key()), it.value(), qApp, [page = QPointer<SshServersPage>(this)](const QString& error) {
            if (!error.isEmpty())
                QMessageBox::warning(page, tr("Passwords"), tr("The password was not saved: %1").arg(error));
        });
    passwords_.clear();
    forgotten_.clear();
}
