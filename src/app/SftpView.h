#pragma once

#include "app/Prompts.h"
#include "ssh/SftpClient.h"

#include <QWidget>

class QLabel;
class QLineEdit;
class QToolButton;
class QTreeWidget;

class SftpView : public QWidget, public rdc::SessionListener {
    Q_OBJECT

public:
    // panel: narrow, beside a terminal, following its folder
    SftpView(rdc::SshConnection::Params params, bool panel = false, QWidget* parent = nullptr);
    ~SftpView() override;

    void start(const std::string& password = {}); // connects; password: if the parameters have none
    bool started() const { return started_; }
    void follow(const QString& path);       // the terminal's folder, by OSC 7
    void followTitle(const QString& title); // "user@host: ~/dir", as many prompts set it, without OSC 7

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
    void upload(const QStringList& files);
    bool eventFilter(QObject* watched, QEvent* event) override; // files dropped on the list
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
    QToolButton* follow_ = nullptr;
    bool started_ = false;
    bool precise_ = false; // OSC 7 seen: titles are ignored
};
