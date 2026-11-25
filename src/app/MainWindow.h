#pragma once

#include "app/ConnectionStore.h"
#include "core/Types.h"

#include <QFont>
#include <QMainWindow>
#include <QTimer>

#include <functional>
#include <map>
#include <memory>

namespace rdc {
class TerminalSession;
struct TerminalListener;
} // namespace rdc

class ActivityBar;
class LibvirtVms;
class Folding;
class AzureView;
class HyperVView;
class ProxmoxView;
class QAction;
class QActionGroup;
class QLabel;
class QLineEdit;
class QScrollArea;
class QStatusBar;
class QSettings;
class QSplitter;
class QStackedWidget;
class Reachability;
class QToolBar;
class RemoteView;
class SessionTabs;
class TerminalView;
class TiledView;
class VmConsole;
class VmView;
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
    void reopenTabs(); // the saved connections open when the app last closed, if so set

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
    void openSettings(const QString& page = {}, QWidget* parent = nullptr); // page: "sshServers"
    QFont terminalFont() const;
    void configureTerminal(TerminalView* terminal);
    void rebuildTree(const std::string& selectId = {});
    std::string selectedId() const;
    const rdc::Connection* findConnection(const std::string& id) const;
    static QString sshConfigGroup();
    static QString scrcpyServerPath();
    void updateActions();
    void editConnection(rdc::Connection connection);
    void editGroup(const QString& group); // its defaults: Settings for "As the group"
    void deleteSelected();
    void duplicateSelected(); // saved passwords too
    QString dropGroup(QPoint at, bool* ok) const; // the group a connection dropped there goes to
    void importFromKrdc();
    void importFromApp(const QString& app); // Remmina, PuTTY, MobaXterm, mRemoteNG
    void importExport();                    // a Portal Mux export or connections.json
    void exportConnections();
    // skips those already saved (same id, or unless `exact`, same protocol, address and user); says what it did
    void importConnections(const QString& title, std::vector<rdc::Connection> found, const QString& note, bool exact = false);
    void checkOnline(bool all); // false: only those not known yet
    void connectTo(const std::string& id);
    bool serversKnown(const rdc::Connection& c); // false, and says so, if its tunnel or jump host was removed
    void startSession(const rdc::Connection& connection);
    void runPreCommand(const rdc::Connection& connection, std::function<void()> next);
    void wakeHost(const rdc::Connection& connection, std::function<void()> next);
    // replace: a reconnect, skips the commands
    void openSession(const rdc::Connection& connection, QWidget* replace = nullptr);
    QScrollArea* createDisplay(const rdc::Connection& connection);
    TerminalView* createTerminal(const rdc::Connection& connection, const QString& address, const QString& protocol = {});
    TerminalView* addTerminalTab(const rdc::Connection& connection, const QString& address, int at, const QString& protocol = {}); // not started
    void openVmConsole(LibvirtVms* host, const std::string& uuid);
    void addVmTab(VmConsole* console); // named by its windowTitle()
    void addFilePanel(TerminalView* terminal, const rdc::Connection& c); // SFTP beside an SSH terminal
    void copyKeyToServer();
    void fillSnippetsMenu();
    void typeIntoOthers(TerminalView* source, const std::function<void(TerminalView*)>& type); // with Type in All Terminals on
    void setTiled(bool on);
    QWidget* tabPage() const; // the current tab's session: in Tile Terminals, the tile last focused
    TiledView* tiled_ = nullptr;
    struct TiledTab {
        QWidget* page;
        int index;
        QString text, toolTip;
    };
    std::vector<TiledTab> tiledTabs_; // where they go back to
    QLabel* typeAllCell_ = nullptr;
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
    void openHyperVConsole(const QString& hostId, const std::string& vmId);
    void openAzureConsole(const QString& key);
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
    void openTestPattern();
    std::string knownHostsFile() const;
    void addSessionTab(QWidget* page, const QString& name, const QString& toolTip, int at = -1);
    void updateActiveList();
    void updateWindowTitle();
    void updateStatusInfo();
    void closeTab(int index);
    RemoteView* currentView() const;
    void updateSessionActions();
    QList<QAction*> sessionTools(QWidget* page) const; // its kind's toolbar, nullptr between groups
    void fillSessionBar(QToolBar* bar, const QList<QAction*>& tools);
    void applyFullScreen(bool on);
    void applyToolBarStyle(); // macOS: as chosen in the toolbar's menu
    bool saveStore();

    QString configDir_;
    ConnectionStore store_;
    std::vector<rdc::Connection> sshConfig_;
    Reachability* reachability_;
    std::map<QString, bool> online_; // by connection id

    ProxmoxView* proxmox_ = nullptr;
    VmView* kvm_ = nullptr; // with libvirt
    HyperVView* hyperv_ = nullptr;
    AzureView* azure_ = nullptr;
    QSettings* settings_;
    bool blurPending_ = false;
    Gamepads* gamepads_ = nullptr;
    QSplitter* splitter_;
    ActivityBar* activityBar_;
    QStackedWidget* views_;
    QWidget* body_; // activity bar and splitter
    SidebarMode sidebarMode_ = SidebarMode::Docked;
    int panelWidth_ = 300;
    QTreeWidget* tree_;
    QLineEdit* filter_;
    QTreeWidget* active_;
    Folding *treeFolding_, *sessionsFolding_;
    SessionTabs* tabs_;
    QAction *editAction_, *duplicateAction_, *deleteAction_, *connectAction_, *sftpAction_;
    QAction *fullScreenAction_, *screenshotAction_, *viewOnlyAction_, *clipboardAction_, *localCursorAction_,
        *grabKeysAction_, *scaleAction_, *reconnectAction_, *disconnectAction_;
    QMenu *sendKeysMenu_, *snippetsMenu_;
    QAction *toolBarAction_, *sidebarAction_, *statusBarAction_, *tabBarAction_;
    QActionGroup* toolBarStyle_ = nullptr; // macOS: Icon Only, Icon and Text, Text Only
    QAction *usbAction_, *detachAction_, *relaunchAction_, *filesAction_, *copyKeyAction_, *typeAllAction_, *tileAction_;
    QList<QAction*> viewToggles_; // View menu: Show KVM Manager, Show Proxmox
    struct SidebarView {
        QString key, title;
        QIcon icon;
        QAction* shown; // the activity bar's menu; disabled: always shown
    };
    std::vector<SidebarView> sidebarViews_; // as made: the activity bar's and the panel's indexes
    QAction *sidebarSeparator_, *sidebarSettings_; // the activity bar's menu, after the views
    QMenu *sessionsMenu_, *toolsMenu_;
    void fillSessionsMenu(); // every session: a tab or a detached window
    std::vector<int> viewOrder() const;     // as the user put them (sidebar/viewOrder)
    void applyViewOrder();
    QToolBar* sessionBar_;
    QList<QAction*> separators_; // shared by the session toolbars
    int sessionBarTextWidth_ = 0;
    QStatusBar* status_;
    QLabel *protocolCell_, *addressCell_, *sizeCell_, *scaleCell_, *fpsCell_, *modeCell_, *numCell_;
    QTimer lockPoll_;
    QToolBar* fullScreenBar_;
    QTimer fullScreenPoll_;
    QTimer fullScreenHold_;
};
