#pragma once

#include "app/Prompts.h"
#include "ssh/SftpClient.h"

#include <QHash>
#include <QKeyEvent>
#include <QSet>
#include <QWidget>

#include <memory>

class QAction;
class QFileSystemWatcher;
class QLabel;
class QTemporaryDir;
class QLineEdit;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

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
    void open(const QString& path, const QString& select = {}, const QString& message = {}); // message: instead of the count
    QString remote(const QString& name) const;
    QList<QTreeWidgetItem*> chosen() const; // the selection without ".."
    rdc::SftpClient::Done finished(int operations, const QString& select = {}); // lists the folder again after the last
    void activate(QTreeWidgetItem* item);
    void openFile(const QString& name); // in its app; saving it sends it back
    void saveBack(const QString& local);
    void rename();
    void remove();
    void create(bool folder);
    void editPermissions();
    void updateActions();
    void download();
    void upload();
    void upload(const QStringList& files);
    bool eventFilter(QObject* watched, QEvent* event) override; // files dropped on the list
    void keyPressEvent(QKeyEvent* event) override; // beside a terminal: none for its shell
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
    QAction *open_, *download_, *rename_, *delete_, *permissions_, *copyPath_, *newFolder_, *newFile_, *upload_, *refresh_, *hidden_;
    QSet<QString> names_; // the folder's, hidden ones too
    QString renaming_;
    std::unique_ptr<QTemporaryDir> edits_;
    QFileSystemWatcher* watcher_ = nullptr;
    QHash<QString, QString> edited_; // local copy -> remote file
    QSet<QString> saving_;
    bool started_ = false;
    bool precise_ = false; // OSC 7 seen: titles are ignored
};
