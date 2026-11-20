#include "app/VmSettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardPaths>
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
