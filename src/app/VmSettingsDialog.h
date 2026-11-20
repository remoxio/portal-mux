#pragma once

#include "app/VmSettings.h"

#include <QDialog>

#include <functional>

class QDialogButtonBox;

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
