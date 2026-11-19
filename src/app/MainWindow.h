#pragma once

#include "app/ConnectionStore.h"
#include "core/Types.h"

#include <QFont>
#include <QMainWindow>
#include <QTimer>

#include <functional>
#include <memory>

namespace rdc {
class TerminalSession;
struct TerminalListener;
} // namespace rdc

class ActivityBar;
class LibvirtVms;
class ProxmoxView;
class QAction;
class QLabel;
class QLineEdit;
class QScrollArea;
class QStatusBar;
class QSettings;
class QSplitter;
class QStackedWidget;
class QTabWidget;
class QToolBar;
class RemoteView;
class TerminalView;
class QListWidget;
class QTreeWidget;

// Connection tree left, session tabs right.
class Gamepads;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);

    void quickConnect();
    bool openAddress(const QString& text, rdc::Protocol fallback = rdc::Protocol::RDP);

protected:
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    QMenu* createPopupMenu() override;

private:
    int sidebarOpacity(bool expanded) const;
    bool sidebarBlur(bool expanded) const;
    bool applySidebar(); // false while blur is pending
    void openSettings();
    QFont terminalFont() const;
    void configureTerminal(TerminalView* terminal);
    void rebuildTree(const std::string& selectId = {});
    std::string selectedId() const;
    const rdc::Connection* findConnection(const std::string& id) const;
    static QString sshConfigGroup();
    static QString scrcpyServerPath();
    void updateActions();
    void editConnection(rdc::Connection connection);
    void deleteSelected();
    void importFromKrdc();
    void connectTo(const std::string& id);
    void startSession(const rdc::Connection& connection);
    void runPreCommand(const rdc::Connection& connection, std::function<void()> next);
    void wakeHost(const rdc::Connection& connection, std::function<void()> next);
    // replace: a reconnect, skips the commands
    void openSession(const rdc::Connection& connection, QWidget* replace = nullptr);
    QScrollArea* createDisplay(const rdc::Connection& connection);
    TerminalView* addTerminalTab(const rdc::Connection& connection, const QString& address, int at, const QString& protocol = {}); // not started
    void openVmConsole(LibvirtVms* host, const std::string& uuid);
    void openVmSerial(LibvirtVms* host, const std::string& uuid, QWidget* replace = nullptr);
    void showProxmoxConsole(const QString& key);
    void showSessionState(QWidget* page, const QString& name, rdc::SessionState state);
    void updateNumLockCell();
    void showUsbDevices();
    void toggleMonitors(QWidget* page); // a multi-monitor RDP session full screen: a window per monitor
    QWidget* currentPage() const; // the session in the active window: a detached one, or the current tab
    void detachTab(int index);
    void reattach(QWidget* detachedWindow);
    void styleStatusCells();
    bool confirmCloseTab(int index);
    void openProxmoxConsole(const rdc::Connection& connection, const QString& key, QWidget* replace, const QString& address,
                            QObject* tunnel);
    void openProxmoxTerminal(const QString& key, QWidget* replace, const QString& name, const QString& address,
                             const std::function<std::unique_ptr<rdc::TerminalSession>(rdc::TerminalListener&)>& make);
    void applyFilter();
    // Overlay: floats over the tabs until a click elsewhere
    enum class SidebarMode { Docked, Collapsed, Overlay };
    void showView(QWidget* page, bool on); // its activity bar icon; a hidden view's open panel switches to the first
    void setSidebar(SidebarMode mode, int view);
    void applySidebarLayout();
    void applyTitleBar();
    void openTestPattern();
    std::string knownHostsFile() const;
    void addSessionTab(QWidget* page, const QString& name, const QString& toolTip, int at = -1);
    void updateActiveList();
    void updateWindowTitle();
    void updateStatusInfo();
    void closeTab(int index);
    RemoteView* currentView() const;
    void updateSessionActions();
    void applyFullScreen(bool on);
    bool saveStore();

    QString configDir_;
    ConnectionStore store_;
    std::vector<rdc::Connection> sshConfig_;

    ProxmoxView* proxmox_ = nullptr;
    QSettings* settings_;
    bool blurPending_ = false;
    Gamepads* gamepads_ = nullptr;
    QSplitter* splitter_;
    ActivityBar* activityBar_;
    QStackedWidget* views_;
    QWidget* body_; // activity bar and splitter, under macOS's title bar
    SidebarMode sidebarMode_ = SidebarMode::Docked;
    int panelWidth_ = 300;
    QTreeWidget* tree_;
    QLineEdit* filter_;
    QTreeWidget* active_;
    QTabWidget* tabs_;
    QAction *editAction_, *deleteAction_, *connectAction_, *sftpAction_;
    QAction *fullScreenAction_, *screenshotAction_, *viewOnlyAction_, *clipboardAction_, *localCursorAction_,
        *grabKeysAction_, *scaleAction_, *reconnectAction_, *disconnectAction_;
    QMenu* sendKeysMenu_;
    QAction *toolBarAction_, *sidebarAction_, *statusBarAction_, *tabBarAction_;
    QAction *usbAction_, *detachAction_, *relaunchAction_;
    QList<QAction*> viewToggles_; // View menu: Show KVM Manager, Show Proxmox
    QWidget* titleBar_ = nullptr; // macOS
    QToolBar* sessionBar_;
    int sessionBarTextWidth_ = 0;
    QStatusBar* status_;
    QLabel *protocolCell_, *addressCell_, *sizeCell_, *scaleCell_, *fpsCell_, *modeCell_, *numCell_;
    QTimer lockPoll_;
    QToolBar* fullScreenBar_;
    QTimer fullScreenPoll_;
    QTimer fullScreenHold_;
};
