#include "app/SftpView.h"

#include <QDateTime>
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

SftpView::SftpView(rdc::SshConnection::Params params, QWidget* parent) : QWidget(parent), client_(*this, std::move(params))
{
    pathEdit_ = new QLineEdit;
    auto* up = new QPushButton(QIcon::fromTheme(QStringLiteral("go-up")), tr("&Up"));
    auto* refresh = new QPushButton(QIcon::fromTheme(QStringLiteral("view-refresh")), tr("&Refresh"));
    auto* uploadButton = new QPushButton(QIcon::fromTheme(QStringLiteral("document-send")), tr("U&pload…"));
    auto* downloadButton = new QPushButton(QIcon::fromTheme(QStringLiteral("document-save")), tr("&Download…"));
    list_ = new QTreeWidget;
    list_->setHeaderLabels({tr("Name"), tr("Size"), tr("Modified")});
    list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    list_->setRootIsDecorated(false);
    list_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    status_ = new QLabel;

    auto* bar = new QHBoxLayout;
    bar->addWidget(up);
    bar->addWidget(pathEdit_, 1);
    bar->addWidget(refresh);
    bar->addWidget(uploadButton);
    bar->addWidget(downloadButton);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(bar);
    layout->addWidget(list_, 1);
    layout->addWidget(status_);

    connect(up, &QPushButton::clicked, this, [this] { open(QFileInfo(path_).path()); });
    connect(refresh, &QPushButton::clicked, this, [this] { open(path_); });
    connect(pathEdit_, &QLineEdit::returnPressed, this, [this] { open(pathEdit_->text()); });
    connect(uploadButton, &QPushButton::clicked, this, &SftpView::upload);
    connect(downloadButton, &QPushButton::clicked, this, &SftpView::download);
    connect(list_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        if (item->data(0, DirRole).toBool()) open(path_ + QLatin1Char('/') + item->text(0));
    });

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
            path_ = clean;
            pathEdit_->setText(clean);
            list_->clear();
            const QLocale locale;
            for (const auto& e : entries) {
                auto* item = new QTreeWidgetItem(list_);
                item->setText(0, QString::fromStdString(e.name));
                item->setIcon(0, QIcon::fromTheme(e.directory ? QStringLiteral("folder") : QStringLiteral("text-x-generic")));
                item->setText(1, e.directory ? QString() : locale.formattedDataSize(qint64(e.size)));
                item->setText(2, QDateTime::fromSecsSinceEpoch(e.mtime).toString(Qt::ISODate));
                item->setData(0, DirRole, e.directory);
            }
            setBusy(tr("%n item(s)", nullptr, int(entries.size())));
        });
    });
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

void SftpView::upload()
{
    const QStringList files = QFileDialog::getOpenFileNames(this, tr("Upload files"), QDir::homePath());
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
