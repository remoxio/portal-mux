#pragma once

#include "app/Prompts.h"
#include "ssh/SftpClient.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QTreeWidget;

class SftpView : public QWidget, public rdc::SessionListener {
    Q_OBJECT

public:
    SftpView(rdc::SshConnection::Params params, QWidget* parent = nullptr);
    ~SftpView() override;

    // SessionListener (worker thread)
    void onStateChanged(rdc::SessionState) override {}
    void onError(std::string) override {}
    bool askTrust(const std::string& title, const std::string& details, bool* always) override;
    bool askCredentials(const std::string& prompt, rdc::Credentials& creds) override;

signals:
    void closeRequested(); // the user cancelled a login or trust prompt

private:
    void open(const QString& path);
    void download();
    void upload();
    void setBusy(const QString& message);
    // Runs fn on the UI thread later; dropped if the view is gone.
    template <class Fn>
    void later(Fn fn) { QMetaObject::invokeMethod(this, std::move(fn), Qt::QueuedConnection); }

    PromptBridge prompts_{this, [this] { emit closeRequested(); }};
    rdc::SftpClient client_;
    QString path_;
    QLineEdit* pathEdit_;
    QTreeWidget* list_;
    QLabel* status_;
};
