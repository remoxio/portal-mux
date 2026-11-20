#include "app/VmSettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <algorithm>

VmSettingsDialog::VmSettingsDialog(const QString& name, VmSettings settings, Apply apply, QWidget* parent)
    : QDialog(parent), settings_(std::move(settings)), apply_(std::move(apply))
{
    const VmSettings& s = settings_;
    setWindowTitle(tr("%1 – Settings").arg(name));

    auto* cpus = new QSpinBox;
    cpus->setRange(1, std::max(s.maxCpus, s.cpus));
    cpus->setValue(s.cpus);
    cpus->setSuffix(tr(" of %1").arg(s.maxCpus));
    cpus->setEnabled(!s.cpusFixed);
    if (s.cpusFixed) cpus->setToolTip(tr("Set per processor in its definition"));

    auto* memory = new QSpinBox;
    memory->setRange(128, std::max(s.maxMemoryMiB, s.memoryMiB));
    memory->setSingleStep(512);
    memory->setValue(s.memoryMiB);
    memory->setSuffix(tr(" MiB of %1 GiB").arg(qRound(s.maxMemoryMiB / 1024.0)));
    memory->setEnabled(!s.memoryFixed);
    if (s.memoryFixed) memory->setToolTip(tr("Set by its NUMA cells or memory devices"));

    auto* iso = new QComboBox;
    iso->setEditable(true);
    iso->addItems(s.isos);
    iso->setCurrentText(s.iso);
    iso->lineEdit()->setPlaceholderText(s.hasCd ? tr("No disc") : tr("No CD/DVD drive"));
    iso->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    iso->setMinimumContentsLength(36);
    iso->setToolTip(tr("A disc image on the host: from its storage pools, or a path typed in"));
    auto* browse = new QPushButton(tr("&Browse…"));
    browse->setVisible(s.localFiles);
    connect(browse, &QPushButton::clicked, this, [this, iso] {
        const QString start = iso->currentText().isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)
                                                           : QFileInfo(iso->currentText()).absolutePath();
        const QString file = QFileDialog::getOpenFileName(this, tr("Disc Image"), start, tr("Disc images (*.iso *.ISO);;All files (*)"));
        if (!file.isEmpty()) iso->setCurrentText(QDir::toNativeSeparators(file));
    });
    auto* eject = new QPushButton(tr("&Eject"));
    connect(eject, &QPushButton::clicked, iso, [iso] { iso->setCurrentText({}); });
    auto* cdRow = new QHBoxLayout;
    cdRow->addWidget(iso, 1);
    cdRow->addWidget(browse);
    cdRow->addWidget(eject);
    auto* cdFirst = new QCheckBox(tr("Boot from the CD/DVD &first"));
    cdFirst->setChecked(s.cdFirst);
    for (QWidget* w : {static_cast<QWidget*>(iso), static_cast<QWidget*>(browse), static_cast<QWidget*>(eject), static_cast<QWidget*>(cdFirst)})
        w->setEnabled(s.hasCd);

    auto* form = new QFormLayout;
    form->addRow(tr("&Processors:"), cpus);
    form->addRow(tr("&Memory:"), memory);
    auto* cdLabel = new QLabel(tr("&CD/DVD:"));
    cdLabel->setBuddy(iso);
    form->addRow(cdLabel, cdRow);
    form->addRow(QString(), cdFirst);
    auto* note = new QLabel(tr("It is running: processors, memory and the boot order change at its next start; the disc changes now."));
    note->setWordWrap(true);
    note->setVisible(s.running);

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(note);
    layout->addStretch();
    layout->addWidget(buttons_);

    read_ = [this, cpus, memory, iso, cdFirst] {
        VmSettings now = settings_;
        now.cpus = cpus->value();
        now.memoryMiB = memory->value();
        now.iso = iso->currentText().trimmed();
        now.cdFirst = cdFirst->isChecked();
        return now;
    };
}

void VmSettingsDialog::addMore(const QString& text, std::function<void()> run)
{
    QPushButton* more = buttons_->addButton(text, QDialogButtonBox::ResetRole); // the left side
    connect(more, &QPushButton::clicked, this, std::move(run));
}

void VmSettingsDialog::accept()
{
    const QString error = apply_(read_());
    if (error.isEmpty()) return QDialog::accept();
    QMessageBox::warning(this, windowTitle(), error);
}

VmSnapshotsDialog::VmSnapshotsDialog(const QString& vmName, std::function<bool()> running, List list, Run run, QWidget* parent)
    : QDialog(parent), vmName_(vmName), running_(running), list_(std::move(list)), run_(std::move(run))
{
    setWindowTitle(tr("%1 – Snapshots").arg(vmName));
    tree_ = new QTreeWidget;
    tree_->setHeaderLabels({tr("Name"), tr("Taken"), tr("State")});
    tree_->setRootIsDecorated(false);
    tree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree_->header()->setStretchLastSection(false);
    connect(tree_, &QTreeWidget::currentItemChanged, this, &VmSnapshotsDialog::updateButtons);
    busy_ = new QLabel;
    busy_->hide();

    take_ = new QPushButton(tr("&Take…"));
    revert_ = new QPushButton(tr("&Revert"));
    delete_ = new QPushButton(tr("&Delete"));
    revert_->setToolTip(tr("Back to this snapshot: what changed since is lost"));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    for (QPushButton* b : {take_, revert_, delete_}) {
        b->setAutoDefault(false);
        buttons->addButton(b, QDialogButtonBox::ActionRole);
    }
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(take_, &QPushButton::clicked, this, [this] {
        QInputDialog ask(this);
        ask.setWindowTitle(tr("Take Snapshot"));
        ask.setLabelText(tr("Name: a letter, then letters, digits, - or _") +
                         (running_() ? QLatin1Char('\n') + tr("It runs: its memory is saved too, and it pauses meanwhile.") : QString()));
        ask.setTextValue(QDateTime::currentDateTime().toString(QStringLiteral("'snapshot-'yyyyMMdd-HHmm")));
        if (auto* edit = ask.findChild<QLineEdit*>()) // libvirt's QEMU job ids and Proxmox take no more
            edit->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("[A-Za-z][A-Za-z0-9_-]{0,39}")), edit));
        if (ask.exec() == QDialog::Accepted && !ask.textValue().isEmpty())
            start(SnapshotOp::Take, ask.textValue(), tr("Taking \"%1\"…").arg(ask.textValue()));
    });
    auto confirm = [this](const QString& title, const QString& text, const QString& yesText) {
        QMessageBox box(QMessageBox::Warning, title, text, QMessageBox::Cancel, this);
        const QAbstractButton* yes = box.addButton(yesText, QMessageBox::DestructiveRole);
        box.setDefaultButton(QMessageBox::Cancel);
        box.setOption(QMessageBox::Option::DontUseNativeDialog); // KDE's box: Enter presses the first button
        box.exec();
        return box.clickedButton() == yes;
    };
    connect(revert_, &QPushButton::clicked, this, [this, confirm] {
        const QString name = selectedName();
        if (confirm(tr("Revert"), tr("Revert \"%1\" to \"%2\"? What changed in it since that snapshot is lost.").arg(vmName_, name), tr("&Revert")))
            start(SnapshotOp::Revert, name, tr("Reverting to \"%1\"…").arg(name));
    });
    connect(delete_, &QPushButton::clicked, this, [this, confirm] {
        const QString name = selectedName();
        if (confirm(tr("Delete Snapshot"), tr("Delete the snapshot \"%1\"? The virtual machine stays as it is.").arg(name), tr("&Delete")))
            start(SnapshotOp::Delete, name, tr("Deleting \"%1\"…").arg(name));
    });

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(tree_, 1);
    layout->addWidget(busy_);
    layout->addWidget(buttons);
    resize(560, 340);
    reload();
}

QString VmSnapshotsDialog::selectedName() const
{
    const QTreeWidgetItem* item = tree_->currentItem();
    return item ? item->data(0, Qt::UserRole).toString() : QString();
}

void VmSnapshotsDialog::reload(const QString& select)
{
    QString error;
    const std::vector<VmSnapshot> snapshots = list_(error);
    tree_->clear();
    for (const VmSnapshot& s : snapshots) {
        auto* item = new QTreeWidgetItem(tree_, {s.name, QLocale().toString(s.created, QLocale::ShortFormat),
                                                 s.memory ? tr("running") : tr("shut off")});
        item->setData(0, Qt::UserRole, s.name);
        item->setToolTip(0, s.description.isEmpty() ? s.name : s.name + QLatin1Char('\n') + s.description);
        item->setToolTip(2, s.memory ? tr("With its memory: reverting resumes it where it was") : tr("Its disks only: reverting leaves it shut off"));
        if (s.current) {
            QFont bold = item->font(0);
            bold.setBold(true);
            for (int c = 0; c < 3; ++c) item->setFont(c, bold);
            item->setToolTip(1, tr("The current snapshot: the one it changed from last"));
        }
        if (s.name == select || (select.isEmpty() && s.current)) tree_->setCurrentItem(item);
    }
    for (int c = 1; c < 3; ++c) tree_->resizeColumnToContents(c);
    if (!error.isEmpty()) {
        busy_->setText(error);
        busy_->show();
    } else if (snapshots.empty()) {
        busy_->setText(tr("No snapshots yet."));
        busy_->show();
    } else if (!working_) {
        busy_->hide();
    }
    updateButtons();
}

void VmSnapshotsDialog::updateButtons()
{
    take_->setEnabled(!working_);
    revert_->setEnabled(!working_ && tree_->currentItem());
    delete_->setEnabled(!working_ && tree_->currentItem());
}

void VmSnapshotsDialog::start(SnapshotOp op, const QString& name, const QString& busy)
{
    working_ = true;
    busy_->setText(busy);
    busy_->show();
    updateButtons();
    run_(op, name, [self = QPointer<VmSnapshotsDialog>(this), op, name](const QString& error) {
        if (!self) return;
        self->working_ = false;
        self->busy_->hide();
        self->reload(op == SnapshotOp::Delete ? QString() : name);
        if (!error.isEmpty()) QMessageBox::warning(self, self->windowTitle(), error);
    });
}
