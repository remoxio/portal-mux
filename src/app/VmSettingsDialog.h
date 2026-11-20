#pragma once

#include "app/VmSettings.h"

#include <QDialog>

#include <functional>
#include <vector>

class QDialogButtonBox;
class QLabel;
class QPushButton;
class QTreeWidget;

// Processors, memory and the CD/DVD of one VM; OK applies, and stays open on an error
class VmSettingsDialog : public QDialog {
    Q_OBJECT

public:
    using Apply = std::function<QString(const VmSettings&)>; // the error, empty if done
    VmSettingsDialog(const QString& name, VmSettings settings, Apply apply, QWidget* parent = nullptr);

    void addMore(const QString& text, std::function<void()> run); // a button for what isn't here
    void accept() override;

private:
    VmSettings settings_;
    Apply apply_;
    QDialogButtonBox* buttons_;
    std::function<VmSettings()> read_;
};

// A VM's snapshots: take, revert to, delete, one at a time
class VmSnapshotsDialog : public QDialog {
    Q_OBJECT

public:
    using List = std::function<std::vector<VmSnapshot>(QString& error)>;
    using Done = std::function<void(const QString& error)>;
    using Run = std::function<void(SnapshotOp op, const QString& name, Done done)>;
    VmSnapshotsDialog(const QString& vmName, std::function<bool()> running, List list, Run run, QWidget* parent = nullptr);

private:
    void reload(const QString& select = {});
    void start(SnapshotOp op, const QString& name, const QString& busy);
    QString selectedName() const;
    void updateButtons();

    QString vmName_;
    std::function<bool()> running_;
    List list_;
    Run run_;
    QTreeWidget* tree_;
    QLabel* busy_;
    QPushButton *take_, *revert_, *delete_;
    bool working_ = false;
};
