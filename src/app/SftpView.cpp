#include "app/SftpView.h"
#include "app/Addresses.h"
#include "app/Icons.h"

#include <QDateTime>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
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
#include <QVBoxLayout>

static constexpr int DirRole = Qt::UserRole;

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

    connect(up, &QAbstractButton::clicked, this, [this] { open(QFileInfo(path_).path()); });
    connect(refresh, &QAbstractButton::clicked, this, [this] { open(path_); });
    connect(pathEdit_, &QLineEdit::returnPressed, this, [this] { open(pathEdit_->text()); });
    connect(uploadButton, &QAbstractButton::clicked, this, qOverload<>(&SftpView::upload));
    connect(downloadButton, &QAbstractButton::clicked, this, &SftpView::download);
    connect(list_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        if (item->data(0, DirRole).toBool()) open(path_ + QLatin1Char('/') + item->text(0));
    });
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

void SftpView::open(const QString& path)
{
    const QString clean = QDir::cleanPath(path);
    setBusy(tr("Listing %1…").arg(clean));
    client_.list(clean.toStdString(), [this, clean](std::vector<rdc::SftpClient::Entry> entries, std::string error) {
        later([this, clean, entries = std::move(entries), error = QString::fromStdString(error)] {
            if (!error.isEmpty()) {
                setBusy(error);
                return;
            }
            // the cursor: on the folder just left, or where it was on a refresh
            QString keep = QFileInfo(path_).path() == clean ? QFileInfo(path_).fileName() : QString();
            if (clean == path_ && list_->currentItem()) keep = list_->currentItem()->text(0);
            path_ = clean;
            pathEdit_->setText(clean);
            list_->clear();
            if (clean != QLatin1String("/")) {
                auto* parent = new QTreeWidgetItem(list_, {QStringLiteral("..")});
                parent->setIcon(0, icons::material("folder"));
                parent->setData(0, DirRole, true);
                list_->setCurrentItem(parent);
            }
            const QLocale locale;
            for (const auto& e : entries) {
                auto* item = new QTreeWidgetItem(list_);
                item->setText(0, QString::fromStdString(e.name));
                item->setIcon(0, e.directory ? icons::material("folder") : icons::lucide("file"));
                item->setText(1, e.directory ? QString() : locale.formattedDataSize(qint64(e.size)));
                item->setText(2, QDateTime::fromSecsSinceEpoch(e.mtime).toString(Qt::ISODate));
                item->setData(0, DirRole, e.directory);
                if (item->text(0) == keep) list_->setCurrentItem(item);
            }
            setBusy(tr("%n item(s)", nullptr, int(entries.size())));
        });
    });
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
    for (const QString& name : names) {
        setBusy(tr("Downloading %1…").arg(name));
        client_.download((path_ + QLatin1Char('/') + name).toStdString(), (target + QLatin1Char('/') + name).toStdString(),
                         [this, name](std::string error) {
                             later([this, name, error = QString::fromStdString(error)] {
                                 setBusy(error.isEmpty() ? tr("Downloaded %1").arg(name) : error);
                             });
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
    for (const QString& file : files) {
        const QString name = QFileInfo(file).fileName();
        setBusy(tr("Uploading %1…").arg(name));
        client_.upload(file.toStdString(), (path_ + QLatin1Char('/') + name).toStdString(), [this, name](std::string error) {
            later([this, name, error = QString::fromStdString(error)] {
                setBusy(error.isEmpty() ? tr("Uploaded %1").arg(name) : error);
                if (error.isEmpty()) open(path_);
            });
        });
    }
}
