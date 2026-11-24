#include "app/SftpView.h"
#include "app/Addresses.h"
#include "app/Icons.h"

#include <QAbstractItemDelegate>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileSystemWatcher>
#include <QGridLayout>
#include <QGuiApplication>
#include <QInputDialog>
#include <QMenu>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

static constexpr int DirRole = Qt::UserRole;
static constexpr int ModeRole = Qt::UserRole + 1;

SftpView::SftpView(rdc::SshConnection::Params params, bool panel, QWidget* parent) : QWidget(parent), client_(*this, std::move(params))
{
    pathEdit_ = new QLineEdit;
    QAbstractButton *up, *refresh, *uploadButton, *downloadButton;
    if (panel) { // icons, the path under them
        auto button = [](const char* icon, const QString& tip) {
            auto* b = new QToolButton;
            b->setIcon(icons::lucide(icon));
            b->setToolTip(tip);
            b->setAutoRaise(true);
            return b;
        };
        up = button("arrow-up", tr("Up"));
        refresh = button("refresh-cw", tr("Refresh"));
        uploadButton = button("upload", tr("Upload… (or drop files on the list)"));
        downloadButton = button("download", tr("Download the selected files…"));
        follow_ = button("terminal", tr("Follow the terminal's folder"));
        follow_->setCheckable(true);
        follow_->setChecked(true);
        connect(follow_, &QToolButton::toggled, this, [this](bool on) {
            if (on) setBusy(tr("Follows the terminal: its prompt must report the folder (OSC 7, or the window title)."));
        });
    } else {
        up = new QPushButton(icons::lucide("arrow-up"), tr("&Up"));
        refresh = new QPushButton(icons::lucide("refresh-cw"), tr("&Refresh"));
        uploadButton = new QPushButton(icons::lucide("upload"), tr("U&pload…"));
        downloadButton = new QPushButton(icons::lucide("download"), tr("&Download…"));
    }
    list_ = new QTreeWidget;
    list_->setHeaderLabels({tr("Name"), tr("Size"), tr("Modified")});
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setEditTriggers(QAbstractItemView::NoEditTriggers); // F2 renames
    list_->setRootIsDecorated(false);
    list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    list_->header()->setStretchLastSection(false);
    list_->setColumnHidden(2, panel);
    list_->viewport()->setAcceptDrops(true);
    list_->viewport()->installEventFilter(this);
    status_ = new QLabel;
    status_->setWordWrap(panel);

    auto* bar = new QHBoxLayout;
    auto* layout = new QVBoxLayout(this);
    if (panel) {
        layout->setContentsMargins(4, 2, 0, 2);
        layout->setSpacing(2);
        bar->setSpacing(0);
        for (QAbstractButton* b : {up, refresh, uploadButton, downloadButton}) bar->addWidget(b);
        bar->addStretch();
        bar->addWidget(follow_);
        layout->addLayout(bar);
        layout->addWidget(pathEdit_);
        list_->setFrameShape(QFrame::NoFrame);
    } else {
        bar->addWidget(up);
        bar->addWidget(pathEdit_, 1);
        bar->addWidget(refresh);
        bar->addWidget(uploadButton);
        bar->addWidget(downloadButton);
        layout->addLayout(bar);
    }
    layout->addWidget(list_, 1);
    layout->addWidget(status_);

    // the list's menu; the shortcuts while it has the focus
    auto action = [this](const char* icon, const QString& text, auto slot, QKeySequence key = {}) {
        auto* a = new QAction(icon ? icons::lucide(icon) : QIcon(), text, list_);
        a->setShortcut(key);
        a->setShortcutContext(Qt::WidgetShortcut);
        list_->addAction(a);
        connect(a, &QAction::triggered, this, slot);
        return a;
    };
    open_ = action("external-link", tr("&Open"), [this] { activate(list_->currentItem()); });
    download_ = action("download", tr("&Download…"), &SftpView::download);
    rename_ = action("text-cursor-input", tr("&Rename"), &SftpView::rename, Qt::Key_F2);
    delete_ = action("trash", tr("De&lete…"), &SftpView::remove, QKeySequence::Delete);
    permissions_ = action("lock", tr("&Permissions…"), &SftpView::editPermissions);
    copyPath_ = action("copy", tr("&Copy Path"), [this] {
        QStringList paths;
        for (QTreeWidgetItem* item : chosen()) paths << remote(item->text(0));
        QGuiApplication::clipboard()->setText(paths.isEmpty() ? path_ : paths.join(QLatin1Char('\n')));
    });
    newFolder_ = action("folder-plus", tr("New &Folder…"), [this] { create(true); });
    newFile_ = action("file-plus", tr("New F&ile…"), [this] { create(false); });
    upload_ = action("upload", tr("&Upload Here…"), qOverload<>(&SftpView::upload));
    refresh_ = action("refresh-cw", tr("R&efresh"), [this] { open(path_); }, QKeySequence::Refresh);
    hidden_ = action(nullptr, tr("Show &Hidden Files"), [this] { open(path_); }, QKeySequence(Qt::CTRL | Qt::Key_H));
    hidden_->setCheckable(true);
    hidden_->setChecked(true);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        updateActions();
        QMenu menu(this);
        if (list_->itemAt(pos)) {
            menu.addActions({open_, download_});
            menu.addSeparator();
            menu.addActions({rename_, delete_, permissions_});
            menu.addSeparator();
        }
        menu.addActions({copyPath_, newFolder_, newFile_, upload_});
        menu.addSeparator();
        menu.addActions({refresh_, hidden_});
        menu.exec(list_->viewport()->mapToGlobal(pos));
    });
    connect(list_, &QTreeWidget::itemSelectionChanged, this, &SftpView::updateActions);
    connect(list_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item, int column) {
        if (renaming_.isEmpty() || column != 0 || !(item->flags() & Qt::ItemIsEditable)) return;
        const QString from = std::exchange(renaming_, {});
        const QString to = item->text(0).trimmed();
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        if (to.isEmpty() || to == from || to == QLatin1String("..") || to.contains(QLatin1Char('/'))) {
            item->setText(0, from);
            return;
        }
        setBusy(tr("Renaming %1…").arg(from));
        client_.rename(remote(from).toStdString(), remote(to).toStdString(), finished(1, to));
    });
    connect(list_->itemDelegate(), &QAbstractItemDelegate::closeEditor, this, [this] { // after a commit, or cancelled
        renaming_.clear();
        if (QTreeWidgetItem* item = list_->currentItem()) item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    });
    updateActions();

    connect(up, &QAbstractButton::clicked, this, [this] { open(QFileInfo(path_).path()); });
    connect(refresh, &QAbstractButton::clicked, refresh_, &QAction::trigger);
    connect(pathEdit_, &QLineEdit::returnPressed, this, [this] { open(pathEdit_->text()); });
    connect(uploadButton, &QAbstractButton::clicked, this, qOverload<>(&SftpView::upload));
    connect(downloadButton, &QAbstractButton::clicked, this, &SftpView::download);
    connect(list_, &QTreeWidget::itemActivated, this, &SftpView::activate);
    setBusy(panel ? tr("Connects once the terminal is logged in.") : QString());
}

void SftpView::start(const std::string& password)
{
    if (started_) return;
    started_ = true;
    if (!password.empty()) client_.setPassword(password);
    setBusy(tr("Connecting…"));
    client_.start([this](std::string error) {
        const QString home = QString::fromStdString(client_.home());
        later([this, error = QString::fromStdString(error), home] {
            if (error.isEmpty())
                open(home.isEmpty() ? QStringLiteral(".") : home);
            else
                setBusy(error == QLatin1String("cancelled") ? tr("Not connected.") : error);
        });
    });
}

SftpView::~SftpView()
{
    prompts_.close();
    client_.stop();
}

bool SftpView::askTrust(const std::string& title, const std::string& details, bool*) // nothing here offers "Always trust"
{
    return prompts::trust(prompts_, this, QString::fromStdString(title), QString::fromStdString(details));
}

bool SftpView::askCredentials(const std::string& prompt, rdc::Credentials& creds)
{
    return prompts::credentials(prompts_, this, QString::fromStdString(prompt), creds);
}

void SftpView::setBusy(const QString& message) { status_->setText(message); }

QString SftpView::remote(const QString& name) const
{
    return path_ == QLatin1String("/") ? path_ + name : path_ + QLatin1Char('/') + name;
}

QList<QTreeWidgetItem*> SftpView::chosen() const
{
    QList<QTreeWidgetItem*> items = list_->selectedItems();
    items.removeIf([](const QTreeWidgetItem* item) { return item->text(0) == QLatin1String(".."); });
    return items;
}

rdc::SftpClient::Done SftpView::finished(int operations, const QString& select)
{
    auto left = std::make_shared<int>(operations);
    auto errors = std::make_shared<QStringList>();
    return [this, left, errors, select](std::string error) {
        later([this, left, errors, select, error = QString::fromStdString(error)] {
            if (!error.isEmpty()) *errors << error;
            if (--*left == 0) open(path_, select, errors->join(QLatin1Char('\n')));
        });
    };
}

void SftpView::updateActions()
{
    const bool connected = !path_.isEmpty();
    const QList<QTreeWidgetItem*> items = chosen();
    const QTreeWidgetItem* current = list_->currentItem();
    const bool folder = current && current->data(0, DirRole).toBool();
    open_->setEnabled(connected && current);
    open_->setIcon(icons::lucide(folder ? "folder-open" : "external-link"));
    download_->setEnabled(std::any_of(items.begin(), items.end(), [](const QTreeWidgetItem* i) { return !i->data(0, DirRole).toBool(); }));
    rename_->setEnabled(items.size() == 1);
    delete_->setEnabled(!items.isEmpty());
    permissions_->setEnabled(!items.isEmpty());
    for (QAction* a : {copyPath_, newFolder_, newFile_, upload_, refresh_, hidden_}) a->setEnabled(connected);
}

void SftpView::open(const QString& path, const QString& select, const QString& message)
{
    const QString clean = QDir::cleanPath(path);
    setBusy(tr("Listing %1…").arg(clean));
    client_.list(clean.toStdString(), [this, clean, select, message](std::vector<rdc::SftpClient::Entry> entries, std::string error) {
        later([this, clean, select, message, entries = std::move(entries), error = QString::fromStdString(error)] {
            if (!error.isEmpty()) {
                setBusy(message.isEmpty() ? error : message + QLatin1Char('\n') + error);
                return;
            }
            // the cursor: on the folder just left, or where it was on a refresh
            QString keep = QFileInfo(path_).path() == clean ? QFileInfo(path_).fileName() : QString();
            if (clean == path_ && list_->currentItem()) keep = list_->currentItem()->text(0);
            if (!select.isEmpty()) keep = select;
            path_ = clean;
            pathEdit_->setText(clean);
            renaming_.clear();
            list_->clear();
            names_.clear();
            if (clean != QLatin1String("/")) {
                auto* parent = new QTreeWidgetItem(list_, {QStringLiteral("..")});
                parent->setIcon(0, icons::material("folder"));
                parent->setData(0, DirRole, true);
                list_->setCurrentItem(parent, 0, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows); // not by the held keys: Ctrl+H
            }
            const QLocale locale;
            for (const auto& e : entries) {
                const QString name = QString::fromStdString(e.name);
                names_.insert(name);
                if (!hidden_->isChecked() && name.startsWith(QLatin1Char('.'))) continue;
                auto* item = new QTreeWidgetItem(list_);
                item->setText(0, name);
                item->setIcon(0, e.directory ? icons::material("folder") : icons::lucide("file"));
                item->setText(1, e.directory ? QString() : locale.formattedDataSize(qint64(e.size)));
                item->setText(2, QDateTime::fromSecsSinceEpoch(e.mtime).toString(Qt::ISODate));
                item->setData(0, DirRole, e.directory);
                item->setData(0, ModeRole, e.permissions);
                if (name == keep) list_->setCurrentItem(item, 0, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            }
            updateActions();
            setBusy(message.isEmpty() ? tr("%n item(s)", nullptr, int(entries.size())) : message);
        });
    });
}

void SftpView::activate(QTreeWidgetItem* item)
{
    if (!item) return;
    if (item->data(0, DirRole).toBool()) open(remote(item->text(0)));
    else openFile(item->text(0));
}

void SftpView::openFile(const QString& name)
{
    if (!edits_) {
        edits_ = std::make_unique<QTemporaryDir>();
        watcher_ = new QFileSystemWatcher(this);
        connect(watcher_, &QFileSystemWatcher::fileChanged, this, &SftpView::saveBack);
    }
    const QString source = remote(name);
    QString local = edited_.key(source);
    if (local.isEmpty()) {
        local = edits_->filePath(QString::number(edited_.size()) + QLatin1Char('/') + name); // a folder each: the same names elsewhere
        QDir().mkpath(QFileInfo(local).path());
        edited_.insert(local, source);
    }
    watcher_->removePath(local);
    setBusy(tr("Downloading %1…").arg(name));
    client_.download(source.toStdString(), local.toStdString(), [this, name, local](std::string error) {
        later([this, name, local, error = QString::fromStdString(error)] {
            if (!error.isEmpty()) return setBusy(error);
            watcher_->addPath(local);
            setBusy(tr("Opened %1: saving it sends it back.").arg(name));
            QDesktopServices::openUrl(QUrl::fromLocalFile(local));
        });
    });
}

void SftpView::saveBack(const QString& local)
{
    if (saving_.contains(local)) return;
    saving_.insert(local);
    QTimer::singleShot(500, this, [this, local] { // editors write in steps, or replace the file
        saving_.remove(local);
        if (!QFileInfo::exists(local)) return;
        if (!watcher_->files().contains(local)) watcher_->addPath(local);
        const QString target = edited_.value(local), name = QFileInfo(local).fileName();
        setBusy(tr("Saving %1…").arg(name));
        client_.upload(local.toStdString(), target.toStdString(), [this, target, name](std::string error) {
            later([this, target, name, error = QString::fromStdString(error)] {
                const QString message = error.isEmpty() ? tr("Saved %1 on the server.").arg(name) : error;
                if (QFileInfo(target).path() == path_) open(path_, {}, message);
                else setBusy(message);
            });
        });
    });
}

void SftpView::rename()
{
    QTreeWidgetItem* item = list_->currentItem();
    if (!item || !item->isSelected() || item->text(0) == QLatin1String("..")) return;
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    renaming_ = item->text(0);
    list_->editItem(item, 0);
}

void SftpView::remove()
{
    const QList<QTreeWidgetItem*> items = chosen();
    if (items.isEmpty()) return;
    const bool folders = std::any_of(items.begin(), items.end(), [](const QTreeWidgetItem* i) { return i->data(0, DirRole).toBool(); });
    QString question = items.size() == 1 ? tr("Delete \"%1\" on the server?").arg(items.first()->text(0))
                                         : tr("Delete these %n items on the server?", nullptr, int(items.size()));
    if (folders) question += QLatin1Char(' ') + tr("Folders go with everything in them.");
    if (!prompts::confirm(this, tr("Delete"), question, tr("&Delete"))) return;
    setBusy(tr("Deleting…"));
    const auto done = finished(int(items.size()));
    for (const QTreeWidgetItem* item : items) client_.remove(remote(item->text(0)).toStdString(), done);
}

void SftpView::create(bool folder)
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, folder ? tr("New Folder") : tr("New File"), tr("Name:"), QLineEdit::Normal, {}, &ok).trimmed();
    if (!ok || name.isEmpty()) return;
    if (name == QLatin1String(".") || name == QLatin1String("..") || name.contains(QLatin1Char('/')))
        return setBusy(tr("\"%1\" can't be a name.").arg(name));
    if (folder) client_.makeFolder(remote(name).toStdString(), finished(1, name));
    else client_.makeFile(remote(name).toStdString(), finished(1, name));
}

void SftpView::editPermissions()
{
    const QList<QTreeWidgetItem*> items = chosen();
    if (items.isEmpty()) return;
    const unsigned mode = items.first()->data(0, ModeRole).toUInt();
    QDialog dialog(this);
    dialog.setWindowTitle(items.size() == 1 ? tr("Permissions of %1").arg(items.first()->text(0)) : tr("Permissions of %n items", nullptr, int(items.size())));
    auto* grid = new QGridLayout(&dialog);
    const QString columns[] = {tr("Read"), tr("Write"), tr("Execute")};
    const QString rows[] = {tr("Owner"), tr("Group"), tr("Others")};
    QCheckBox* boxes[9];
    for (int c = 0; c < 3; ++c) grid->addWidget(new QLabel(columns[c]), 0, c + 1, Qt::AlignCenter);
    for (int r = 0; r < 3; ++r) {
        grid->addWidget(new QLabel(rows[r]), r + 1, 0);
        for (int c = 0; c < 3; ++c) {
            QCheckBox*& box = boxes[r * 3 + c];
            box = new QCheckBox;
            box->setChecked(mode & (0400u >> (r * 3 + c)));
            grid->addWidget(box, r + 1, c + 1, Qt::AlignCenter);
        }
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    grid->addWidget(buttons, 4, 0, 1, 4);
    if (dialog.exec() != QDialog::Accepted) return;
    unsigned bits = 0;
    for (int i = 0; i < 9; ++i)
        if (boxes[i]->isChecked()) bits |= 0400u >> i;
    const auto done = finished(int(items.size()), items.first()->text(0));
    for (const QTreeWidgetItem* item : items) // setuid, setgid and sticky stay
        client_.setPermissions(remote(item->text(0)).toStdString(), (item->data(0, ModeRole).toUInt() & ~0777u) | bits, done);
}

void SftpView::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Backspace && event->modifiers() == Qt::NoModifier && path_ != QLatin1String("/"))
        open(QFileInfo(path_).path());
    event->accept();
}

void SftpView::download()
{
    QStringList names;
    for (QTreeWidgetItem* item : list_->selectedItems())
        if (!item->data(0, DirRole).toBool()) names << item->text(0);
    if (names.isEmpty()) return;
    const QString target = QFileDialog::getExistingDirectory(this, tr("Download to"), QDir::homePath());
    if (target.isEmpty()) return;
    QStringList existing;
    for (const QString& name : names)
        if (QFileInfo::exists(target + QLatin1Char('/') + name)) existing << name;
    if (!existing.isEmpty() &&
        !prompts::confirm(this, tr("Download"), tr("Replace %1 in %2?").arg(existing.join(QStringLiteral(", ")), target), tr("&Replace")))
        return;
    for (const QString& name : names) {
        setBusy(tr("Downloading %1…").arg(name));
        client_.download(remote(name).toStdString(), (target + QLatin1Char('/') + name).toStdString(), [this, name](std::string error) {
            later([this, name, error = QString::fromStdString(error)] { setBusy(error.isEmpty() ? tr("Downloaded %1").arg(name) : error); });
        });
    }
}

void SftpView::follow(const QString& path)
{
    precise_ = true;
    if (follow_ && follow_->isChecked() && started_ && QDir::cleanPath(path) != path_) open(path);
}

void SftpView::followTitle(const QString& title)
{
    if (precise_ || !follow_ || !follow_->isChecked() || !started_) return;
    const QString path = titleFolder(title, QString::fromStdString(client_.home()));
    if (!path.isEmpty() && QDir::cleanPath(path) != path_) open(path);
}

bool SftpView::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == list_->viewport() && (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove)) {
        auto* drag = static_cast<QDropEvent*>(event);
        if (drag->mimeData()->hasUrls() && started_) drag->acceptProposedAction();
        return true;
    }
    if (watched == list_->viewport() && event->type() == QEvent::Drop) {
        auto* drop = static_cast<QDropEvent*>(event);
        QStringList files;
        for (const QUrl& url : drop->mimeData()->urls())
            if (url.isLocalFile() && QFileInfo(url.toLocalFile()).isFile()) files << url.toLocalFile();
        upload(files);
        drop->acceptProposedAction();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

void SftpView::upload() { upload(QFileDialog::getOpenFileNames(this, tr("Upload files"), QDir::homePath())); }

void SftpView::upload(const QStringList& files)
{
    if (files.isEmpty() || path_.isEmpty()) return;
    QStringList existing;
    for (const QString& file : files)
        if (names_.contains(QFileInfo(file).fileName())) existing << QFileInfo(file).fileName();
    if (!existing.isEmpty() &&
        !prompts::confirm(this, tr("Upload"), tr("Replace %1 on the server?").arg(existing.join(QStringLiteral(", "))), tr("&Replace")))
        return;
    setBusy(tr("Uploading…"));
    const auto done = finished(int(files.size()), QFileInfo(files.last()).fileName());
    for (const QString& file : files) client_.upload(file.toStdString(), remote(QFileInfo(file).fileName()).toStdString(), done);
}
