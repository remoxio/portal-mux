#include "app/MainWindow.h"

#include "app/About.h"
#include "app/ActivityBar.h"
#include "app/Addresses.h"
#include "app/ConnectionDialog.h"
#include "app/ElidingDelegate.h"
#include "app/Icons.h"
#include "app/KrdcImport.h"
#include "app/LockKeys.h"
#include "app/Look.h"
#include "app/MonitorWindow.h"
#include "app/AzureSerial.h"
#include "app/AzureView.h"
#include "app/HyperVView.h"
#include "app/PanelTree.h"
#include "app/ProxmoxView.h"
#include "app/Keymap.h"
#ifdef PORTAL_MUX_LIBVIRT
#include "app/LibvirtVms.h"
#include "app/VmView.h"
#endif
#include "app/VmConsole.h"
#include "app/RemoteView.h"
#include "app/PageRail.h"
#include "app/SessionTabs.h"
#include "app/SftpView.h"
#include "app/SshServers.h"
#include "app/WindowEffects.h"
#include "core/Properties.h"
#include "core/TestPatternSession.h"
#include "android/ScrcpySession.h"
#include "moonlight/MoonlightSession.h"
#include "rdp/RdpSession.h"
#include "spice/SpiceDisplaySession.h"
#include "ssh/SshConfig.h"
#include "ssh/SshTerminalSession.h"
#include "ssh/SshTunnel.h"
#include "local/LocalTerminalSession.h"
#include "stream/StreamTerminalSession.h"
#include "terminal/TerminalView.h"
#include "vnc/VncSession.h"

#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPointer>
#include <QProcess>
#include <QProgressBar>
#include <QProgressDialog>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSplitter>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyleHints>
#include <QTabBar>
#include <QTcpSocket>
#include <QUuid>
#include <QToolBar>
#include <QToolButton>
#include <QTimer>
#include <QTreeWidget>
#include <QUdpSocket>
#include <QUrl>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <memory>

using rdc::Connection;
using rdc::Protocol;
using rdc::SessionState;

static constexpr int IdRole = Qt::UserRole;
static constexpr int ReadOnlyRole = Qt::UserRole + 1;
static constexpr int WindowRole = Qt::UserRole + 2; // Sessions: a detached window
static constexpr int FoldRole = Qt::UserRole + 3;   // a group's id for its folding
static constexpr int AlwaysRole = Qt::UserRole + 4; // Settings → Sidebar: a view that can't be hidden

namespace {
class AlwaysOnDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

protected:
    void initStyleOption(QStyleOptionViewItem* option, const QModelIndex& index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        if (!index.data(AlwaysRole).toBool()) return;
        option->state &= ~QStyle::State_Enabled; // greyed, as a disabled menu item: the check too
        option->palette.setCurrentColorGroup(QPalette::Disabled);
    }
};
} // namespace

static std::vector<std::string> secretKeys(rdc::Protocol protocol)
{
    using rdc::Protocol;
    std::vector<std::string> keys;
    if (protocol == Protocol::RDP || protocol == Protocol::VNC || protocol == Protocol::SPICE || protocol == Protocol::SSH)
        keys.push_back(""); // the others have no password field
    for (const rdc::PropertyDef& d : rdc::properties(protocol))
        if (d.kind == rdc::PropertyKind::Secret) keys.push_back(d.key);
    return keys;
}

// Settings → Layout → Appearance; as the system unless chosen
static void applyColorScheme(const QSettings* settings)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    const QString chosen = settings->value(QStringLiteral("appearance/colorScheme")).toString();
    QGuiApplication::styleHints()->setColorScheme(chosen == QLatin1String("light")  ? Qt::ColorScheme::Light
                                                  : chosen == QLatin1String("dark") ? Qt::ColorScheme::Dark
                                                                                    : Qt::ColorScheme::Unknown);
#else
    Q_UNUSED(settings)
#endif
}

// its saved SSH servers too: the tunnel's password, the jump host's address, key and password
static void readSecrets(QObject* context, QSettings* settings, Connection c, std::function<void(Connection)> done)
{
    struct State {
        Connection c;
        std::vector<std::pair<std::string, std::string>> sources; // secret's name, option ("": the password)
        size_t next = 0;
        std::function<void(Connection)> done;
        std::function<void()> step;
    };
    auto state = std::make_shared<State>();
    for (const std::string& key : secretKeys(c.protocol)) state->sources.emplace_back(connectionSecret(c.id, key), key);
    if (const auto via = findSshServer(settings, QString::fromStdString(c.option("tunnel"))); via && via->auth == QLatin1String("password"))
        state->sources.emplace_back(sshServerSecret(via->id), "tunnel.password");
    if (const auto jump = findSshServer(settings, QString::fromStdString(c.option("jumpHost")))) {
        c.options["jumpHost"] = jump->address().toStdString();
        if (jump->auth == QLatin1String("publicKey")) c.options["jumpHost.keyFile"] = jump->keyFile.toStdString();
        else state->sources.emplace_back(sshServerSecret(jump->id), "jumpHost.password");
    }
    state->c = std::move(c);
    state->done = std::move(done);
    state->step = [context, weak = std::weak_ptr<State>(state)] { // weak: no self-reference cycle
        const auto s = weak.lock();
        if (s->next == s->sources.size()) return s->done(s->c);
        const auto [name, key] = s->sources[s->next++];
        secrets::read(name, context, [s, key](QString value) {
            if (!value.isEmpty()) // nothing stored: keep libvirt's display password
                (key.empty() ? s->c.password : s->c.options[key]) = value.toStdString();
            s->step();
        });
    };
    state->step();
}

static QString protocolLabel(Protocol p)
{
    switch (p) {
    case Protocol::RDP: return QStringLiteral("RDP");
    case Protocol::VNC: return QStringLiteral("VNC");
    case Protocol::SPICE: return QStringLiteral("SPICE");
    case Protocol::SSH: return QStringLiteral("SSH");
    case Protocol::Android: return QStringLiteral("Android");
    case Protocol::Moonlight: return QStringLiteral("Moonlight");
    case Protocol::Local: return QStringLiteral("Terminal");
    case Protocol::Telnet: return QStringLiteral("Telnet");
    case Protocol::Serial: return QStringLiteral("Serial");
    }
    return {};
}

static QIcon protocolIcon(const QString& label)
{
    static const std::pair<const char*, const char*> glyphs[] = {
        {"RDP", "grid-2x2"}, {"VNC", "monitor"}, {"SPICE", "monitor-cog"}, {"SSH", "square-terminal"},
        {"SFTP", "folders"}, {"Android", "smartphone"}, {"Moonlight", "gamepad-2"}, {"Terminal", "terminal"}, {"Test", "image"},
        {"Telnet", "network"}, {"Serial", "usb"}};
    for (auto [name, glyph] : glyphs)
        if (label == QLatin1String(name)) return icons::lucide(glyph);
    return {};
}

static QWidget* viewPage(const QString& title, QWidget* body, const QList<QAction*>& actions)
{
    auto* label = new QLabel(title.toUpper());
    QFont font; // family follows the window font
    font.setBold(true);
    font.setPointSizeF(QApplication::font().pointSizeF() * 0.85);
    label->setFont(font);
    label->setMinimumHeight(26);
    label->setContentsMargins(10, 2, 2, 2);
    auto* page = new QWidget;
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(label);
    if (!actions.isEmpty()) { // under the title, which a narrow sidebar then shows whole
        auto* bar = new QHBoxLayout;
        bar->setContentsMargins(6, 0, 2, 4); // the icons start under the title's first letter
        bar->setSpacing(0);
        for (QAction* action : actions) {
            auto* button = new QToolButton;
            button->setDefaultAction(action);
            button->setAutoRaise(true);
            button->setIconSize(QSize(16, 16));
            bar->addWidget(button);
        }
        bar->addStretch();
        layout->addLayout(bar);
    }
    layout->addWidget(body, 1);
    return page;
}

static QProcessEnvironment commandEnvironment(const Connection& c)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PORTAL_MUX_NAME"), QString::fromStdString(c.name));
    env.insert(QStringLiteral("PORTAL_MUX_HOST"), QString::fromStdString(c.host));
    env.insert(QStringLiteral("PORTAL_MUX_PORT"), QString::number(c.effectivePort()));
    env.insert(QStringLiteral("PORTAL_MUX_USER"), QString::fromStdString(c.username));
    return env;
}

static void setShellCommand(QProcess& process, const QString& command)
{
#ifdef Q_OS_WIN
    process.setProgram(QStringLiteral("cmd.exe"));
    process.setNativeArguments(QStringLiteral("/c ") + command);
#else
    process.setProgram(QStringLiteral("/bin/sh"));
    process.setArguments({QStringLiteral("-c"), command});
#endif
}

// A session in its own window; closing it puts the tab back
class DetachedWindow : public QMainWindow {
public:
    DetachedWindow(QWidget* parent, std::function<void(DetachedWindow*)> reattach)
        : QMainWindow(parent, Qt::Window), reattach_(std::move(reattach))
    {
    }
    QWidget* page() const { return centralWidget(); }
    QString title, toolTip;

protected:
    void closeEvent(QCloseEvent* event) override
    {
        if (!page()) return QMainWindow::closeEvent(event);
        event->ignore();
        reattach_(this);
    }

private:
    std::function<void(DetachedWindow*)> reattach_;
};

// set before focusWindowChanged, unlike activeWindow()
static DetachedWindow* activeDetached()
{
    for (QWindow* w = QGuiApplication::focusWindow(); w; w = w->transientParent()) // menu → detached → main window
        for (QWidget* top : QApplication::topLevelWidgets())
            if (auto* d = dynamic_cast<DetachedWindow*>(top); d && d->windowHandle() == w) return d;
    return nullptr;
}

static TerminalView* terminalOf(QWidget* page) // the page, or the terminal in it
{
    if (auto* terminal = qobject_cast<TerminalView*>(page)) return terminal;
    return page ? page->findChild<TerminalView*>() : nullptr;
}

static DetachedWindow* detachedOf(const QTreeWidgetItem* item) // only if it still exists
{
    const QObject* window = item ? item->data(0, WindowRole).value<QObject*>() : nullptr;
    for (QWidget* top : QApplication::topLevelWidgets())
        if (top == window) return dynamic_cast<DetachedWindow*>(top);
    return nullptr;
}

// macOS: icons in capsules (Look_mac.mm), the names in tooltips
static void capsules(QToolBar* bar, const QMargins& margins)
{
#ifdef Q_OS_MACOS
    bar->setProperty("capsules", true);
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    bar->setContentsMargins(margins);
#endif
}

static QColor lineColor() // 20% text on the window colour
{
    const QPalette palette = QApplication::palette();
    const QColor window = palette.color(QPalette::Window), text = palette.color(QPalette::WindowText);
    return QColor((window.red() * 4 + text.red()) / 5, (window.green() * 4 + text.green()) / 5, (window.blue() * 4 + text.blue()) / 5);
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent),
      configDir_(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)),
      store_(configDir_ + QStringLiteral("/connections.json")),
      settings_(new QSettings(configDir_ + QStringLiteral("/portal-mux.conf"), QSettings::IniFormat, this))
{
    setWindowTitle(QStringLiteral("Portal Mux"));
    // before menuBar(): on macOS it creates the native window
    if (windowEffects::translucencyPossible()) setAttribute(Qt::WA_TranslucentBackground);
    if (QFont font; font.fromString(settings_->value(QStringLiteral("font/main")).toString())) QApplication::setFont(font);
    applyColorScheme(settings_);
    resize(1280, 800);
    QDir().mkpath(configDir_);

    auto* newAction = new QAction(icons::lucide("plus"), tr("&New Connection…"), this);
    newAction->setShortcut(QKeySequence::New);
    editAction_ = new QAction(icons::lucide("pencil"), tr("&Edit…"), this);
    editAction_->setShortcut(Qt::Key_F2);
    deleteAction_ = new QAction(icons::lucide("trash"), tr("&Delete"), this);
    deleteAction_->setShortcut(QKeySequence::Delete);
    connectAction_ = new QAction(icons::lucide("plug"), tr("&Connect"), this);
    sftpAction_ = new QAction(icons::lucide("folders"), tr("Open &SFTP Browser"), this);
    for (QAction* a : {editAction_, deleteAction_, connectAction_}) a->setShortcutContext(Qt::WidgetShortcut);

    fullScreenAction_ = new QAction(this);
    fullScreenAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F));
    screenshotAction_ = new QAction(icons::lucide("camera"), tr("Copy Screenshot to Clip&board"), this);
    screenshotAction_->setIconText(tr("Screenshot"));
    viewOnlyAction_ = new QAction(icons::lucide("eye"), tr("&View Only"), this);
    clipboardAction_ = new QAction(icons::lucide("clipboard"), tr("C&lipboard Sharing"), this);
    localCursorAction_ = new QAction(icons::lucide("mouse-pointer-2"), tr("Show Local &Cursor"), this);
    grabKeysAction_ = new QAction(icons::lucide("keyboard"), tr("&Grab All Possible Keys"), this);
    grabKeysAction_->setIconText(tr("Grab Keys"));
    grabKeysAction_->setToolTip(tr("Send every key to the remote side, Alt+Tab and this window's shortcuts too"));
    scaleAction_ = new QAction(icons::lucide("scaling"), tr("&Scale Remote Screen to Fit Window Size"), this);
    scaleAction_->setIconText(tr("Scale"));
    disconnectAction_ = new QAction(icons::lucide("unplug"), tr("&Disconnect"), this);
    reconnectAction_ = new QAction(icons::lucide("refresh-cw"), tr("&Reconnect"), this);
    connect(reconnectAction_, &QAction::triggered, this, [this] {
        if (RemoteView* view = currentView()) emit view->reconnectRequested();
        else if (auto* terminal = terminalOf(currentPage())) emit terminal->reconnectRequested();
    });
    // scancodes for RDP etc., keysyms for VNC
    sendKeysMenu_ = new QMenu(tr("Send &Keys"), this);
    sendKeysMenu_->setIcon(icons::lucide("command"));
    sendKeysMenu_->menuAction()->setIconText(tr("Send Keys"));
    auto key = [](uint32_t xtScancode, uint32_t keysym) {
        rdc::KeyEvent k;
        k.scancode = xtScancode;
        k.keysym = keysym;
        return k;
    };
    const rdc::KeyEvent ctrl = key(0x1D, 0xffe3), alt = key(0x38, 0xffe9);
    auto addKeys = [this](const QString& text, std::vector<rdc::KeyEvent> keys) {
        sendKeysMenu_->addAction(text, this, [this, keys] {
            if (RemoteView* view = currentView()) view->sendKeys(keys);
        });
    };
    addKeys(tr("Ctrl+Alt+Del"), {ctrl, alt, key(0x153, 0xffff)});
    addKeys(tr("Print Screen"), {key(0x137, 0xff61)});
    sendKeysMenu_->addSeparator();
    for (uint32_t i = 0; i < 12; ++i) addKeys(tr("Ctrl+Alt+F%1").arg(i + 1), {ctrl, alt, key(i < 10 ? 0x3B + i : 0x57 + i - 10, 0xffbe + i)});
    addAction(fullScreenAction_); // its shortcut must work with the menu bar hidden
    const std::pair<QAction*, void (RemoteView::*)(bool)> toggles[] = {
        {viewOnlyAction_, &RemoteView::setViewOnly},   {clipboardAction_, &RemoteView::setClipboardSharing},
        {localCursorAction_, &RemoteView::setLocalCursor}, {grabKeysAction_, &RemoteView::setGrabKeys},
        {scaleAction_, &RemoteView::setScaled}};
    for (auto [action, set] : toggles) {
        action->setCheckable(true);
        connect(action, &QAction::triggered, this, [this, set](bool on) {
            if (RemoteView* view = currentView()) (view->*set)(on);
        });
    }
    connect(fullScreenAction_, &QAction::triggered, this, [this] {
        if (QWidget* page = currentPage(); page && page->property("monitorParts").isValid()) return toggleMonitors(page);
        QWidget* window = activeDetached() ? static_cast<QWidget*>(activeDetached()) : this;
        window->setWindowState(window->windowState() ^ Qt::WindowFullScreen);
    });
    connect(screenshotAction_, &QAction::triggered, this, [this] {
        if (RemoteView* view = currentView()) QGuiApplication::clipboard()->setImage(view->screenshot());
    });
    connect(disconnectAction_, &QAction::triggered, this, [this] {
        if (DetachedWindow* window = activeDetached()) delete window->page(); // its window closes with it
        else closeTab(tabs_->currentIndex());
    });
    filesAction_ = new QAction(icons::lucide("panel-left"), tr("&Files"), this);
    filesAction_->setCheckable(true);
    filesAction_->setToolTip(tr("The remote files beside the terminal (SFTP): upload, download, following its folder"));
    connect(filesAction_, &QAction::triggered, this, [this](bool on) {
        TerminalView* terminal = terminalOf(currentPage());
        if (!terminal || !terminal->sidePanel()) return;
        settings_->setValue(QStringLiteral("terminal/filePanel"), on);
        terminal->setSidePanelShown(on);
        if (on && terminal->property("sessionState").toInt() == int(SessionState::Connected))
            if (auto* files = qobject_cast<SftpView*>(terminal->sidePanel())) files->start(terminal->takeTypedPassword());
    });
    detachAction_ = new QAction(icons::lucide("external-link"), tr("&Detach Tab"), this);
    detachAction_->setToolTip(tr("This session in its own window; closing the window puts it back"));
    connect(detachAction_, &QAction::triggered, this, [this] { detachTab(tabs_->currentIndex()); });
    connect(qApp, &QGuiApplication::focusWindowChanged, this, [this] { updateSessionActions(); }); // actions follow the active window

    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(newAction);
    file->addAction(icons::lucide("zap"), tr("&Quick Connection…"), this, &MainWindow::quickConnect);
    file->addActions({editAction_, deleteAction_, connectAction_, sftpAction_});
    file->addSeparator();
    file->addAction(tr("&Import from KRDC…"), this, &MainWindow::importFromKrdc);
    file->addSeparator();
    file->addAction(icons::lucide("log-out"), tr("&Quit"), QKeySequence::Quit, this, &QWidget::close)->setMenuRole(QAction::QuitRole);
    usbAction_ = new QAction(icons::lucide("usb"), tr("&USB Devices…"), this);
    usbAction_->setToolTip(tr("Hand this computer's USB devices to the VM (SPICE)"));
    connect(usbAction_, &QAction::triggered, this, &MainWindow::showUsbDevices);
    relaunchAction_ = new QAction(icons::lucide("refresh-cw"), tr("Reconnect at &Window Size"), this);
    relaunchAction_->setToolTip(tr("Quits the app on the host and starts it again at this window's size (Moonlight; with Apollo's "
                                   "virtual display the host's screen gets that size)"));
    connect(relaunchAction_, &QAction::triggered, this, [this] {
        if (RemoteView* view = currentView()) view->relaunchAtWindowSize();
    });
    sessionsMenu_ = menuBar()->addMenu(tr("S&essions"));
    connect(sessionsMenu_, &QMenu::aboutToShow, this, &MainWindow::fillSessionsMenu);
    sessionsMenu_->addAction(tr("No sessions"))->setEnabled(false); // macOS leaves an empty menu out of the menu bar
    toolsMenu_ = menuBar()->addMenu(tr("&Tools")); // the session's toolbar, and more: updateSessionActions()
    QMenu* view = menuBar()->addMenu(tr("&View"));
    auto addToggle = [&](QAction*& action, const QString& text, const char* key) {
        const QString setting = QStringLiteral("view/") + QLatin1String(key);
        action = view->addAction(text);
        action->setCheckable(true);
        action->setChecked(settings_->value(setting, true).toBool());
        connect(action, &QAction::toggled, this, [this, setting](bool on) {
            settings_->setValue(setting, on);
            applyFullScreen(isFullScreen());
        });
    };
    addToggle(toolBarAction_, tr("Show &Toolbar"), "toolBar");
#ifdef Q_OS_MACOS
    toolBarStyle_ = new QActionGroup(this); // in the toolbar's menu, as AppKit's
    for (const auto& [text, labelled] : {std::pair{tr("Icon Only"), false}, std::pair{tr("Icon and Text"), true}}) {
        QAction* a = toolBarStyle_->addAction(text);
        a->setCheckable(true);
        a->setChecked(settings_->value(QStringLiteral("view/toolBarText"), false).toBool() == labelled);
        connect(a, &QAction::triggered, this, [this, labelled] {
            settings_->setValue(QStringLiteral("view/toolBarText"), labelled);
            applyToolBarText();
        });
    }
#endif
    addToggle(sidebarAction_, tr("Show &Sidebar"), "sidebar");
    addToggle(statusBarAction_, tr("Show Stat&us Bar"), "statusBar");
    addToggle(tabBarAction_, tr("Show Ta&b Bar"), "tabBar");
    view->addSeparator();
    view->addAction(tr("Open Test &Pattern"), this, &MainWindow::openTestPattern);
    QMenu* settings = menuBar()->addMenu(tr("&Configuration"));
    settings->addAction(icons::lucide("settings"), tr("&Settings…"), QKeySequence::Preferences, this,
                        [this] { openSettings(); })
        ->setMenuRole(QAction::PreferencesRole);
    // placeholder; in the app menu on macOS
    settings->addAction(icons::lucide("keyboard"), tr("S&hortcuts…"), this, [this] {
        QMessageBox::information(this, tr("Shortcuts"), tr("Configurable shortcuts are coming."));
    })->setMenuRole(QAction::ApplicationSpecificRole);
    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&About Portal Mux"), this, [this] { showAbout(this); })->setMenuRole(QAction::AboutRole);

    // in the work area's layout, not a QMainWindow toolbar
    sessionBar_ = new QToolBar(tr("Session"));
    sessionBar_->setMovable(false);
    sessionBar_->setFloatable(false);
    sessionBar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    capsules(sessionBar_, {8, 0, 8, 6}); // under the tabs' track
    sessionBar_->installEventFilter(this);
    sessionBar_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(sessionBar_, &QWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        std::unique_ptr<QMenu> menu(createPopupMenu());
        menu->exec(sessionBar_->mapToGlobal(at));
    });
    status_ = new QStatusBar;
    auto cell = [this](const QString& widest) {
        auto* label = new QLabel;
        label->setAlignment(Qt::AlignCenter);
        if (!widest.isEmpty()) label->setMinimumWidth(label->fontMetrics().horizontalAdvance(widest) + 14); // no jumping
        status_->addPermanentWidget(label);
        label->hide(); // until a tab fills it
        return label;
    };
    protocolCell_ = cell(QStringLiteral("Moonlight"));
    addressCell_ = cell({});
    sizeCell_ = cell(QStringLiteral("8888×8888"));
    scaleCell_ = cell(QStringLiteral("888%"));
    fpsCell_ = cell(tr("88 fps"));
    modeCell_ = cell({});
    numCell_ = cell(QStringLiteral("NUM"));
    styleStatusCells();
    numCell_->setText(QStringLiteral("NUM"));
    numCell_->setToolTip(tr("Num Lock of this computer; sessions follow it"));
    lockPoll_.setInterval(500); // shortcut: polled, no lock-state signal in Qt
    connect(&lockPoll_, &QTimer::timeout, this, &MainWindow::updateNumLockCell);
    lockPoll_.start();
    updateNumLockCell();

    fullScreenBar_ = new QToolBar(this); // floats over the session
    fullScreenBar_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    capsules(fullScreenBar_, {8, 6, 8, 6});
    fullScreenBar_->setMovable(false);
    fullScreenBar_->setAutoFillBackground(true);
    fullScreenBar_->hide();
    fullScreenHold_.setSingleShot(true);
    fullScreenHold_.setInterval(2000);
    fullScreenPoll_.setInterval(100); // polled: the remote screen takes every mouse event
    connect(&fullScreenPoll_, &QTimer::timeout, this, [this] {
        const QPoint pointer = mapFromGlobal(QCursor::pos());
        if (fullScreenBar_->isHidden() && QRect(0, -3, width(), 4).contains(pointer)) {
            fullScreenBar_->adjustSize();
            fullScreenBar_->move((width() - fullScreenBar_->width()) / 2, 0);
            fullScreenBar_->show();
            fullScreenBar_->raise();
            fullScreenHold_.start();
        } else if (fullScreenBar_->isVisible() && !fullScreenHold_.isActive() &&
                   pointer.y() > fullScreenBar_->geometry().bottom() + 40) {
            fullScreenBar_->hide();
        }
    });

    activityBar_ = new ActivityBar;
    auto* splitter = splitter_ = new QSplitter;
    views_ = new QStackedWidget(splitter);
    views_->setAutoFillBackground(true); // floating over tabs can't be translucent
    tree_ = new QTreeWidget;
    tree_->setHeaderHidden(true);
    tree_->setContextMenuPolicy(Qt::ActionsContextMenu);
    auto* reloadSshConfig = new QAction(icons::lucide("refresh-cw"), tr("&Reload ~/.ssh/config"), this);
    connect(reloadSshConfig, &QAction::triggered, this, [this] { rebuildTree(selectedId()); });
    auto* separator = new QAction(this);
    separator->setSeparator(true);
    tree_->addActions({connectAction_, sftpAction_, editAction_, deleteAction_, newAction, separator, reloadSshConfig});
    active_ = new QTreeWidget;
    active_->setHeaderHidden(true);
    active_->setContextMenuPolicy(Qt::CustomContextMenu);
    for (QTreeWidget* page : {tree_, active_}) {
        page->setFrameShape(QFrame::NoFrame);
        page->setItemDelegate(new ElidingDelegate(page));
    }
    treeFolding_ = new Folding(tree_, settings_, QStringLiteral("folded/connections"), FoldRole);
    sessionsFolding_ = new Folding(active_, settings_, QStringLiteral("folded/sessions"), FoldRole);
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(tr("Search"));
    filter_->setToolTip(tr("Name or address; Enter connects to the selected match (Ctrl+F)"));
    filter_->setClearButtonEnabled(true);
    filter_->installEventFilter(this);
    auto* connectionsPage = new QWidget;
    auto* connectionsLayout = new QVBoxLayout(connectionsPage);
    connectionsLayout->setContentsMargins(0, 0, 0, 0);
    connectionsLayout->setSpacing(0);
    auto* filterRow = new QHBoxLayout;
    filterRow->setContentsMargins(6, 4, 6, 4);
    filterRow->addWidget(filter_);
    connectionsLayout->addLayout(filterRow);
    connectionsLayout->addWidget(tree_, 1);
    const QStringList hiddenViews = settings_->value(QStringLiteral("window/hiddenViews")).toStringList();
    auto addView = [&](const char* key, const QIcon& icon, const QString& title, QWidget* body, const QList<QAction*>& actions,
                       const QString& menuText = {}) { // menuText: can be hidden, with this toggle in the View menu
        const bool optional = !menuText.isEmpty();
        QWidget* page = viewPage(title, body, actions);
        page->setObjectName(QLatin1String(key));
        views_->addWidget(page);
        activityBar_->addView(icon, title);
        auto* shown = new QAction(icon, title, activityBar_); // the bar's context menu, as VS Code's
        shown->setCheckable(true);
        shown->setChecked(!optional || !hiddenViews.contains(QLatin1String(key)));
        shown->setEnabled(optional);
        activityBar_->addAction(shown);
        activityBar_->setViewHidden(views_->indexOf(page), !shown->isChecked());
        sidebarViews_.push_back({QLatin1String(key), title, icon, shown});
        connect(shown, &QAction::toggled, this, [this, page](bool on) { showView(page, on); });
        if (!optional) return;
        auto* inMenu = new QAction(menuText, this);
        inMenu->setCheckable(true);
        inMenu->setChecked(shown->isChecked());
        connect(inMenu, &QAction::toggled, shown, &QAction::setChecked);
        connect(shown, &QAction::toggled, inMenu, &QAction::setChecked);
        view->insertAction(statusBarAction_, inMenu);
        viewToggles_ << inMenu;
    };
    connect(activityBar_, &ActivityBar::clicked, this, [this](int view) {
        if (sidebarMode_ == SidebarMode::Docked) setSidebar(SidebarMode::Docked, view);
        else if (sidebarMode_ == SidebarMode::Overlay && view == views_->currentIndex()) setSidebar(SidebarMode::Collapsed, view);
        else setSidebar(SidebarMode::Overlay, view);
    });
    connect(activityBar_, &ActivityBar::doubleClicked, this, [this](int view) {
        setSidebar(sidebarMode_ == SidebarMode::Docked ? SidebarMode::Collapsed : SidebarMode::Docked, view);
    });
    // a button can't take the toolbar action's icon
    auto* sidebarNew = new QAction(icons::lucide("plus"), newAction->text(), this);
    connect(sidebarNew, &QAction::triggered, newAction, &QAction::trigger);
    addView("connections", icons::lucide("network"), tr("Connections"), connectionsPage, {sidebarNew});
#ifdef PORTAL_MUX_LIBVIRT
    auto* vmView = kvm_ = new VmView(settings_);
    connect(vmView, &VmView::vmsChanged, this, &MainWindow::updateStatusInfo);
    connect(vmView, &VmView::openRequested, this, &MainWindow::openVmConsole);
    connect(vmView, &VmView::serialRequested, this, [this](LibvirtVms* host, const std::string& uuid) { openVmSerial(host, uuid); });
    connect(vmView, &VmView::message, this, [this](const QString& text) { status_->showMessage(text, 5000); });
    addView("kvm", icons::lucide("monitor-cog"), tr("KVM Manager"), vmView, vmView->titleActions(), tr("Show &KVM Manager"));
#endif
    proxmox_ = new ProxmoxView(settings_, configDir_);
    connect(proxmox_, &ProxmoxView::openRequested, this, &MainWindow::showProxmoxConsole);
    connect(proxmox_, &ProxmoxView::consoleReady, this, &MainWindow::openProxmoxConsole);
    connect(proxmox_, &ProxmoxView::terminalReady, this, &MainWindow::openProxmoxTerminal);
    connect(proxmox_, &ProxmoxView::message, this, [this](const QString& text) { status_->showMessage(text, 8000); });
    addView("proxmox", icons::brand("proxmox-line"), tr("Proxmox"), proxmox_, proxmox_->titleActions(), tr("Show Pro&xmox"));
    setSshServersEditor(settings_, [this](QWidget* parent) { openSettings(QStringLiteral("sshServers"), parent); });
    hyperv_ = new HyperVView(settings_, configDir_);
    connect(hyperv_, &HyperVView::openRequested, this, &MainWindow::openHyperVConsole);
    connect(hyperv_, &HyperVView::message, this, [this](const QString& text) { status_->showMessage(text, 8000); });
    addView("hyperv", icons::lucide("grid-2x2"), tr("Hyper-V Manager"), hyperv_, hyperv_->titleActions(), tr("Show &Hyper-V Manager"));
    azure_ = new AzureView(settings_);
    connect(azure_, &AzureView::openRequested, this, &MainWindow::openAzureConsole);
    connect(azure_, &AzureView::connectRequested, this, [this](const Connection& c) { openSession(c); });
    connect(azure_, &AzureView::message, this, [this](const QString& text) { status_->showMessage(text, 8000); });
    addView("azure", icons::brand("azure-line"), tr("Azure"), azure_, azure_->titleActions(), tr("Show &Azure"));
    addView("active", icons::lucide("panels-top-left"), tr("Sessions"), active_, {});
    sidebarSeparator_ = new QAction(activityBar_);
    sidebarSeparator_->setSeparator(true);
    sidebarSettings_ = new QAction(icons::lucide("settings"), tr("&Settings…"), activityBar_);
    connect(sidebarSettings_, &QAction::triggered, this, [this] { openSettings(QStringLiteral("sidebar")); });
    applyViewOrder();
    const QString savedView = settings_->value(QStringLiteral("window/sidebarView")).toString();
    for (int i = 0; i < views_->count(); ++i)
        if (views_->widget(i)->objectName() == savedView && !hiddenViews.contains(savedView)) views_->setCurrentIndex(i);
    sidebarMode_ = settings_->value(QStringLiteral("window/sidebarCollapsed"), false).toBool() ? SidebarMode::Collapsed : SidebarMode::Docked;
    panelWidth_ = std::max(120, settings_->value(QStringLiteral("window/sidebarWidth"), 300).toInt());
    activityBar_->setCurrent(sidebarMode_ == SidebarMode::Docked ? views_->currentIndex() : -1);
    auto* work = new QWidget(splitter);
    auto* workLayout = new QVBoxLayout(work);
    workLayout->setContentsMargins(0, 0, 0, 0);
    workLayout->setSpacing(0);
    tabs_ = new SessionTabs;
    tabs_->setToolBar(sessionBar_);
    workLayout->addWidget(tabs_, 1);
    workLayout->addWidget(status_);
    tabs_->setTabsClosable(settings_->value(QStringLiteral("tabs/closeButtons"), true).toBool());
    tabs_->setMovable(true);
    tabs_->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tabs_->tabBar(), &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        const int index = tabs_->tabBar()->tabAt(pos);
        if (index < 0) return;
        QMenu menu;
        menu.addAction(detachAction_->icon(), tr("&Detach"), this, [this, index] { detachTab(index); });
        menu.addAction(icons::lucide("x"), tr("&Close"), this, [this, index] {
            if (confirmCloseTab(index)) closeTab(index);
        });
        menu.exec(tabs_->tabBar()->mapToGlobal(pos));
    });
    splitter->setStretchFactor(1, 1);
    splitter->setCollapsible(0, false);
    connect(splitter, &QSplitter::splitterMoved, this, [this] { panelWidth_ = splitter_->sizes().value(0, panelWidth_); });
    splitter->installEventFilter(this); // panel keeps its width on resize
    splitter->setSizes({panelWidth_, 1000});
    body_ = new QWidget;
    auto* bodyLayout = new QHBoxLayout(body_);
    bodyLayout->setContentsMargins(0, 1, 0, 0);
    bodyLayout->setSpacing(0); // meet: one rectangle to blur
    bodyLayout->addWidget(activityBar_);
    bodyLayout->addWidget(splitter, 1);
    setCentralWidget(body_);
    applyToolBarText();
    body_->installEventFilter(this);
    activityBar_->installEventFilter(this);
    if (restoreGeometry(settings_->value(QStringLiteral("window/geometry")).toByteArray()))
        setWindowState(windowState() & ~Qt::WindowFullScreen);

    if (testAttribute(Qt::WA_TranslucentBackground)) {
        for (QWidget* w : std::initializer_list<QWidget*>{menuBar(), work, sessionBar_, status_, tabs_, splitter->handle(1)})
            w->setAutoFillBackground(true);
        views_->installEventFilter(this);
    }

    connect(newAction, &QAction::triggered, this, [this] {
        Connection c;
        if (QTreeWidgetItem* item = tree_->currentItem()) {
            QTreeWidgetItem* group = item->data(0, IdRole).toString().isEmpty() ? item : item->parent();
            if (group && !group->data(0, ReadOnlyRole).toBool()) c.group = group->text(0).toStdString();
        }
        editConnection(c);
    });
    connect(editAction_, &QAction::triggered, this, [this] {
        const Connection* c = findConnection(selectedId());
        if (!c) return;
        Connection copy = *c;
        if (!store_.find(copy.id)) { // ssh config is never written: edit a copy
            copy.id.clear();
            copy.group.clear();
        }
        editConnection(copy);
    });
    connect(deleteAction_, &QAction::triggered, this, &MainWindow::deleteSelected);
    connect(connectAction_, &QAction::triggered, this, [this] { connectTo(selectedId()); });
    connect(sftpAction_, &QAction::triggered, this, [this] {
        const Connection* found = findConnection(selectedId());
        if (!found || !serversKnown(*found)) return;
        readSecrets(this, settings_, *found, [this](Connection c) {
            auto* view = new SftpView(rdc::SshConnection::fromConnection(c, knownHostsFile()));
            view->setProperty("protocol", QStringLiteral("SFTP"));
            view->setProperty("address", QString::fromStdString(c.username.empty() ? c.host : c.username + "@" + c.host));
            connect(view, &SftpView::closeRequested, view, &QObject::deleteLater);
            addSessionTab(view, tr("%1 (files)").arg(QString::fromStdString(c.name)), QString::fromStdString(c.host));
            view->start();
        });
    });
    connect(tree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item) {
        connectTo(item->data(0, IdRole).toString().toStdString());
    });
    connect(tree_, &QTreeWidget::currentItemChanged, this, &MainWindow::updateActions);
    for (auto [signal, icon] : {std::pair{&QTreeWidget::itemExpanded, "folder-open"}, std::pair{&QTreeWidget::itemCollapsed, "folder"}})
        connect(tree_, signal, this, [icon](QTreeWidgetItem* group) {
            if (!group->data(0, ReadOnlyRole).toBool()) group->setIcon(0, icons::material(icon));
        });
    connect(filter_, &QLineEdit::textChanged, this, &MainWindow::applyFilter);
    connect(filter_, &QLineEdit::returnPressed, this, [this] { connectTo(selectedId()); });
    auto* findAction = new QAction(this); // in a session the key goes remote
    findAction->setShortcut(QKeySequence::Find);
    addAction(findAction);
    connect(findAction, &QAction::triggered, this, [this] {
        setSidebar(sidebarMode_ == SidebarMode::Docked ? SidebarMode::Docked : SidebarMode::Overlay, 0);
        filter_->setFocus();
        filter_->selectAll();
    });
    connect(active_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) {
        if (item && item->data(0, IdRole).isValid()) tabs_->setCurrentIndex(item->data(0, IdRole).toInt());
    });
    connect(active_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
        DetachedWindow* window = detachedOf(item);
        if (!window && !item->data(0, IdRole).isValid()) return;
        if (sidebarMode_ == SidebarMode::Overlay) setSidebar(SidebarMode::Collapsed, -1);
        if (window) {
            window->raise();
            window->activateWindow();
            window->page()->setFocus();
        } else if (RemoteView* view = currentView()) {
            view->setFocus();
        } else if (QWidget* page = tabs_->currentWidget()) {
            page->setFocus();
        }
    });
    connect(active_, &QWidget::customContextMenuRequested, this, [this](QPoint at) {
        QTreeWidgetItem* item = active_->itemAt(at);
        QMenu menu;
        if (DetachedWindow* window = detachedOf(item)) {
            menu.addAction(tr("&Show Window"), window, [window] {
                window->raise();
                window->activateWindow();
            });
            menu.addAction(icons::lucide("undo-2"), tr("&Attach to the Main Window"), this, [this, window] { reattach(window); });
            menu.addAction(disconnectAction_->icon(), disconnectAction_->text(), window, [window] { delete window->page(); });
        } else if (item && item->data(0, IdRole).isValid()) {
            menu.addActions({reconnectAction_, disconnectAction_}); // the item is current: its tab is shown
        } else {
            return;
        }
        menu.exec(active_->viewport()->mapToGlobal(at));
    });
    connect(tabs_, &SessionTabs::tabCloseRequested, this, [this](int index) {
        if (confirmCloseTab(index)) closeTab(index);
    });
    connect(tabs_->tabBar(), &QTabBar::tabMoved, this, &MainWindow::updateActiveList);
    connect(tabs_, &SessionTabs::currentChanged, this, [this] {
        if (isFullScreen()) setWindowState(windowState() & ~Qt::WindowFullScreen);
        tabs_->setTabIcon(tabs_->currentIndex(), QIcon());
        updateSessionActions();
        updateActiveList();
        updateWindowTitle();
        updateStatusInfo();
        status_->clearMessage(); // it was about the previous tab
        if (QWidget* page = tabs_->currentWidget()) {
            QWidget* session = page->property("sessionState").isValid() ? page : nullptr;
            for (QWidget* child : page->findChildren<QWidget*>())
                if (!session && child->property("sessionState").isValid()) session = child;
            if (session && session->property("sessionState").toInt() == int(SessionState::Connecting))
                status_->showMessage(tr("%1: Connecting…").arg(tabs_->tabText(tabs_->currentIndex()).remove(QLatin1Char('&'))));
        }
    });
    applyFullScreen(false);

    QString error;
    if (!store_.load(&error))
        QMessageBox::warning(this, tr("Connections"),
                             tr("Could not read %1: %2\nA copy was saved as %1.broken.").arg(store_.path(), error));
    if (store_.ensureLocalTerminal()) saveStore();
    rebuildTree();
}

void MainWindow::rebuildTree(const std::string& selectId)
{
    const Rebuild quiet(tree_);
    tree_->clear();
    std::map<QString, QTreeWidgetItem*> groups;
    QTreeWidgetItem* selected = nullptr;
    for (const Connection& c : store_.all()) {
        QTreeWidgetItem* parent = nullptr;
        if (!c.group.empty()) {
            const QString name = QString::fromStdString(c.group);
            auto& g = groups[name];
            if (!g) {
                g = new QTreeWidgetItem(tree_, {name});
                g->setIcon(0, icons::material("folder")); // opening sets folder-open
                g->setData(0, FoldRole, QStringLiteral("group:") + name);
            }
            parent = g;
        }
        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree_);
        item->setText(0, QString::fromStdString(c.name));
        item->setIcon(0, protocolIcon(protocolLabel(c.protocol)));
        item->setToolTip(0, c.protocol == Protocol::Local ? tr("A terminal on this computer")
                                                          : QStringLiteral("%1://%2").arg(QLatin1String(protocolName(c.protocol)),
                                                                                          QString::fromStdString(c.host)));
        item->setData(0, IdRole, QString::fromStdString(c.id));
        if (c.id == selectId) selected = item;
    }
    QList<QTreeWidgetItem*> readOnly; // never merged with a same-named user group
    auto addReadOnly = [&](const QString& title, const char* icon, const QString& tip, const std::vector<Connection>& list,
                           const std::function<QString(const Connection&)>& address) {
        if (list.empty()) return;
        auto* g = new QTreeWidgetItem(tree_, {title});
        g->setIcon(0, icons::material(icon));
        g->setToolTip(0, tip);
        g->setData(0, ReadOnlyRole, true);
        g->setData(0, FoldRole, QStringLiteral("readonly:") + title);
        for (const Connection& c : list) {
            auto* item = new QTreeWidgetItem(g, {QString::fromStdString(c.name)});
            item->setIcon(0, protocolIcon(protocolLabel(c.protocol)));
            item->setToolTip(0, address(c));
            item->setData(0, IdRole, QString::fromStdString(c.id));
            if (c.id == selectId) selected = item;
        }
        readOnly << g;
    };
    sshConfig_ = rdc::readSshConfig(QDir::homePath().toStdString() + "/.ssh/config");
    addReadOnly(sshConfigGroup(), "folder-secure", tr("Hosts from ~/.ssh/config (read-only; Edit saves a copy)"), sshConfig_,
                [](const Connection& c) {
                    return QStringLiteral("ssh://%1%2").arg(c.username.empty() ? QString() : QString::fromStdString(c.username) + u'@',
                                                           QString::fromStdString(c.host));
                });
    tree_->sortItems(0, Qt::AscendingOrder);
    for (auto& [name, g] : groups) treeFolding_->apply(g);
    for (QTreeWidgetItem* g : readOnly) treeFolding_->apply(g);
    if (selected) tree_->setCurrentItem(selected);
    applyFilter();
    updateActions();
}

void MainWindow::applyFilter()
{
    const QString text = filter_->text().trimmed();
    auto matches = [&text](const QTreeWidgetItem* item) {
        return item->text(0).contains(text, Qt::CaseInsensitive) || item->toolTip(0).contains(text, Qt::CaseInsensitive);
    };
    QTreeWidgetItem* first = nullptr;
    auto show = [&](QTreeWidgetItem* item, bool visible) {
        item->setHidden(!visible);
        if (visible && !first) first = item;
    };
    for (int i = 0; i < tree_->topLevelItemCount(); ++i) {
        QTreeWidgetItem* top = tree_->topLevelItem(i);
        if (!top->data(0, IdRole).toString().isEmpty()) {
            show(top, matches(top));
            continue;
        }
        const bool wholeGroup = top->text(0).contains(text, Qt::CaseInsensitive);
        int shown = 0;
        for (int j = 0; j < top->childCount(); ++j) {
            show(top->child(j), wholeGroup || matches(top->child(j)));
            shown += !top->child(j)->isHidden();
        }
        top->setHidden(shown == 0);
    }
    QTreeWidgetItem* current = tree_->currentItem();
    if (!text.isEmpty() && first && (!current || current->isHidden() || current->data(0, IdRole).toString().isEmpty()))
        tree_->setCurrentItem(first);
}

int MainWindow::sidebarOpacity(bool expanded) const
{
    const int fallback = expanded ? 85 : 100;
    return std::clamp(settings_->value(expanded ? QStringLiteral("sidebar/opacity") : QStringLiteral("sidebar/collapsedOpacity"), fallback)
                          .toInt(), 0, 100) / 5 * 5;
}

bool MainWindow::sidebarBlur(bool expanded) const
{
    return settings_->value(expanded ? QStringLiteral("sidebar/blur") : QStringLiteral("sidebar/collapsedBlur"), true).toBool();
}

bool MainWindow::applySidebar()
{
    if (!testAttribute(Qt::WA_TranslucentBackground)) return true;
    // floating panel is opaque: blur can't show the session
    const bool expanded = sidebarMode_ == SidebarMode::Docked;
    const int opacity = sidebarOpacity(expanded);
    QRect side(activityBar_->mapTo(this, QPoint()), activityBar_->size());
    if (expanded && views_->isVisible()) side = side.united(QRect(views_->mapTo(this, QPoint()), views_->size()));
    const bool wantBlur = sidebarBlur(expanded) && opacity < 100 && activityBar_->isVisible();
    const bool blurred = windowEffects::setBlurBehind(this, wantBlur, side);
    const bool waiting = wantBlur && !blurred && blurPending_; // opaque until the compositor answers: no flash
    QColor tint = QApplication::palette().color(QPalette::Window);
    tint.setAlpha(waiting ? 255 : opacity * 255 / 100);
    QColor opaque = tint;
    opaque.setAlpha(255);
    for (QWidget* w : std::initializer_list<QWidget*>{activityBar_, views_}) {
        const QColor color = w == views_ && sidebarMode_ == SidebarMode::Overlay ? opaque : tint;
        QPalette palette = w->palette();
        if (palette.color(QPalette::Window) != color) {
            palette.setColor(QPalette::Window, color);
            palette.setColor(QPalette::Base, Qt::transparent);
            w->setPalette(palette);
        }
        w->setAutoFillBackground(true);
    }
#ifdef Q_OS_MACOS
    // AppKit's sidebar selection: faint, text unchanged
    const QColor faint = tint.lightness() < 128 ? QColor(255, 255, 255, 31) : QColor(0, 0, 0, 26);
    const QColor text = QApplication::palette().color(QPalette::Text);
    for (QTreeView* list : views_->findChildren<QTreeView*>()) {
        QPalette palette = list->palette();
        if (palette.color(QPalette::Highlight) == faint && palette.color(QPalette::HighlightedText) == text) continue;
        palette.setColor(QPalette::Highlight, faint);
        palette.setColor(QPalette::HighlightedText, text);
        list->setPalette(palette);
    }
#endif
    return !waiting;
}

void MainWindow::showEvent(QShowEvent* event)
{
    QMainWindow::showEvent(event);
    if (!testAttribute(Qt::WA_TranslucentBackground)) return;
    // Wayland announces blur asynchronously; retry ~1s
    auto retry = std::make_shared<std::function<void(int)>>();
    *retry = [this, retry](int left) {
        blurPending_ = left > 0;
        if (applySidebar()) blurPending_ = false;
        else QTimer::singleShot(50, this, [retry, left] { (*retry)(left - 1); });
    };
    (*retry)(QGuiApplication::platformName() == QLatin1String("wayland") ? 20 : 0);
}

QFont MainWindow::terminalFont() const
{
    QFont font = look::systemFont(QFontDatabase::FixedFont);
    if (const QString saved = settings_->value(QStringLiteral("font/terminal")).toString(); !saved.isEmpty()) font.fromString(saved);
    return font;
}

void MainWindow::configureTerminal(TerminalView* terminal)
{
    const QFont font = terminalFont();
    const QString family = terminal->property("ownFont").toString();
    const int size = terminal->property("ownFontSize").toInt();
    terminal->configure(family.isEmpty() ? font.family() : family, size > 0 ? size : font.pointSize(),
                        terminal->property("scrollback").toInt());
}

void MainWindow::quickConnect()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Quick Connection"));
    auto* protocol = new QComboBox;
    for (Protocol p : {Protocol::RDP, Protocol::VNC, Protocol::SPICE, Protocol::SSH, Protocol::Telnet})
        protocol->addItem(QString::fromLatin1(protocolName(p)).toUpper(), int(p));
    protocol->setCurrentIndex(std::max(0, protocol->findData(settings_->value(QStringLiteral("quick/protocol")).toInt())));
    protocol->setToolTip(tr("Protocol for an address that names none"));
    auto* address = new QLineEdit;
    address->setPlaceholderText(tr("host or user@host:port"));
    address->setToolTip(tr("Connect without saving: host, user@host:port, or an rdp://, vnc://, spice:// or ssh:// address"));
    address->setMinimumWidth(address->fontMetrics().averageCharWidth() * 44);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    QPushButton* go = buttons->addButton(tr("&Connect"), QDialogButtonBox::AcceptRole);
    go->setDefault(true);
    go->setEnabled(false);
    connect(address, &QLineEdit::textChanged, go, [go](const QString& text) { go->setEnabled(!text.trimmed().isEmpty()); });
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        settings_->setValue(QStringLiteral("quick/protocol"), protocol->currentData());
        if (openAddress(address->text(), Protocol(protocol->currentData().toInt()))) dialog.accept();
    });
    auto* form = new QFormLayout;
    form->addRow(tr("&Protocol:"), protocol);
    form->addRow(tr("&Address:"), address);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(form);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::openSettings(const QString& page, QWidget* parent)
{
    QDialog dialog(parent ? parent : this);
    dialog.setWindowTitle(tr("Settings"));
    // pages apply at once; Cancel restores these as last applied
    const QStringList groups = {QStringLiteral("appearance"), QStringLiteral("sidebar"), QStringLiteral("tabs"), QStringLiteral("font"),
                                QStringLiteral("sshServers")};
    auto current = [this, groups] {
        QMap<QString, QVariant> values;
        for (const QString& key : settings_->allKeys())
            if (groups.contains(key.section(QLatin1Char('/'), 0, 0)) || key == QLatin1String("window/hiddenViews")) values[key] = settings_->value(key);
        return values;
    };
    QMap<QString, QVariant> applied = current();

    auto* appearance = new QGroupBox(tr("Appearance"));
    auto* schemes = new QHBoxLayout(appearance);
    const QString scheme = settings_->value(QStringLiteral("appearance/colorScheme")).toString();
    for (const auto& [value, name] : std::initializer_list<std::pair<QString, QString>>{
             {QString(), tr("As the system")}, {QStringLiteral("light"), tr("Light")}, {QStringLiteral("dark"), tr("Dark")}}) {
        auto* radio = new QRadioButton(name);
        radio->setChecked(scheme == value);
        schemes->addWidget(radio);
        connect(radio, &QRadioButton::toggled, &dialog, [this, value](bool on) {
            if (!on) return;
            if (value.isEmpty()) settings_->remove(QStringLiteral("appearance/colorScheme"));
            else settings_->setValue(QStringLiteral("appearance/colorScheme"), value);
            applyColorScheme(settings_);
        });
    }
    schemes->addStretch();
#if QT_VERSION < QT_VERSION_CHECK(6, 8, 0)
    appearance->hide(); // Qt can't choose before 6.8
#endif

    auto* sidebar = new QGroupBox(tr("Transparency"));
    auto* box = new QVBoxLayout(sidebar);
    box->addWidget(new QLabel(tr("How much it covers the desktop behind it:")));
    auto* grid = new QGridLayout;
    box->addLayout(grid);
    auto addMode = [&](int row, const QString& name, const QString& tip, bool expanded) {
        auto* label = new QLabel(name);
        label->setToolTip(tip);
        auto* slider = new QSlider(Qt::Horizontal);
        slider->setRange(0, 20);
        slider->setPageStep(2);
        slider->setValue(sidebarOpacity(expanded) / 5);
        label->setBuddy(slider);
        auto* value = new QLabel(tr("%1%").arg(slider->value() * 5));
        value->setMinimumWidth(value->fontMetrics().horizontalAdvance(tr("%1%").arg(100)));
        auto* blur = new QCheckBox(tr("Blur the desktop behind it"));
        blur->setChecked(sidebarBlur(expanded));
        blur->setEnabled(windowEffects::blurAvailable());
        grid->addWidget(label, row, 0);
        grid->addWidget(slider, row, 1);
        grid->addWidget(value, row, 2);
        grid->addWidget(blur, row + 1, 1, 1, 2);
        auto apply = [this, slider, blur, value, expanded] {
            value->setText(tr("%1%").arg(slider->value() * 5));
            settings_->setValue(expanded ? QStringLiteral("sidebar/opacity") : QStringLiteral("sidebar/collapsedOpacity"), slider->value() * 5);
            settings_->setValue(expanded ? QStringLiteral("sidebar/blur") : QStringLiteral("sidebar/collapsedBlur"), blur->isChecked());
            applySidebar();
        };
        connect(slider, &QSlider::valueChanged, &dialog, apply);
        connect(blur, &QCheckBox::toggled, &dialog, apply);
    };
    addMode(0, tr("Open"), tr("The icon column and the panel"), true);
    addMode(2, tr("Collapsed"), tr("The icon column alone. A panel opened over the session with a click is always opaque."), false);
    grid->setColumnStretch(1, 1);
    QString note;
    if (!testAttribute(Qt::WA_TranslucentBackground)) {
        note = tr("This desktop cannot show what is behind a window.");
        sidebar->setEnabled(false);
    } else if (!windowEffects::blurAvailable()) {
#ifdef Q_OS_WIN
        note = tr("This desktop does not blur: Transparency effects are off, or it is a Remote Desktop session.");
#else
        note = tr("This desktop does not offer blur.");
#endif
    }
    if (!note.isEmpty()) box->addWidget(new QLabel(note));

    auto fontBox = [this, &dialog](const QString& title, const QString& hint, const QString& key, QFontDatabase::SystemFont kind,
                                   std::function<void(const QFont&)> apply) {
        const QFont system = look::systemFont(kind);
        auto* group = new QGroupBox(title);
        QFont current = system;
        const QString saved = settings_->value(key).toString();
        if (!saved.isEmpty()) current.fromString(saved);
        // not QFontComboBox: its filter misses Noto Sans Mono
        auto* family = new QComboBox;
        QStringList families;
        for (const QString& f : QFontDatabase::families())
            if (!QFontDatabase::isPrivateFamily(f) && (kind != QFontDatabase::FixedFont || QFontDatabase::isFixedPitch(f))) families << f;
        for (const QString& f : {system.family(), current.family()})
            if (!families.contains(f)) families << f;
        families.sort(Qt::CaseInsensitive);
        family->addItems(families);
        auto* size = new QComboBox;
        auto* reset = new QPushButton(tr("As the system"));
        auto* status = new QLabel;
        auto* sample = new QLabel(QStringLiteral("ABCDEFGHIJKLMNOPQRSTUVWXYZ abcdefghijklmnopqrstuvwxyz\näöüéèêàñç 0123456789"));
        sample->setStyleSheet(QStringLiteral("QLabel { border: 1px solid palette(mid); border-radius: 6px; padding: 8px; }"));
        sample->setTextInteractionFlags(Qt::TextSelectableByMouse);
        auto show = [=](const QFont& font, bool chosen) {
            const QSignalBlocker blockFamily(family), blockSize(size);
            family->setCurrentText(font.family());
            QList<int> sizes = QFontDatabase::standardSizes();
            if (!sizes.contains(font.pointSize())) sizes.append(font.pointSize());
            std::sort(sizes.begin(), sizes.end());
            size->clear();
            for (int s : sizes) size->addItem(QString::number(s));
            size->setCurrentText(QString::number(font.pointSize()));
            status->setText(chosen ? tr("Chosen; the system's is %1, %2").arg(system.family()).arg(system.pointSize())
                                   : tr("The system's font"));
            reset->setEnabled(chosen);
            sample->setFont(font);
        };
        show(current, !saved.isEmpty());
        auto changed = [this, system, family, size, key, show, apply] {
            QFont font = system;
            font.setFamily(family->currentText());
            font.setPointSize(size->currentText().toInt());
            settings_->setValue(key, font.toString());
            show(font, true);
            apply(font);
        };
        connect(family, &QComboBox::currentTextChanged, &dialog, changed);
        connect(size, &QComboBox::currentTextChanged, &dialog, changed);
        connect(reset, &QPushButton::clicked, &dialog, [this, system, key, show, apply] {
            settings_->remove(key);
            show(system, false);
            apply(system);
        });
        auto* layout = new QVBoxLayout(group);
        if (!hint.isEmpty()) layout->addWidget(new QLabel(hint));
        auto* pick = new QHBoxLayout;
        pick->addWidget(family, 1);
        pick->addWidget(size);
        pick->addWidget(reset);
        layout->addLayout(pick);
        layout->addWidget(status);
        layout->addWidget(sample);
        return group;
    };
    QGroupBox* mainFont = fontBox(tr("Main window font"), {}, QStringLiteral("font/main"), QFontDatabase::GeneralFont,
                                  [](const QFont& font) { QApplication::setFont(font); });
    QGroupBox* sshFont = fontBox(tr("SSH terminal"), tr("Fixed-width fonts only; connections with their own font keep it."),
                                 QStringLiteral("font/terminal"), QFontDatabase::FixedFont, [this](const QFont&) {
                                     for (int i = 0; i < tabs_->count(); ++i)
                                         if (auto* terminal = terminalOf(tabs_->widget(i))) configureTerminal(terminal);
                                 });

    auto* viewsBox = new QGroupBox(tr("Views"));
    auto* viewList = new QListWidget;
    viewList->setDragDropMode(QAbstractItemView::InternalMove);
    viewList->setDefaultDropAction(Qt::MoveAction);
    for (int i : viewOrder()) {
        const SidebarView& v = sidebarViews_[size_t(i)];
        auto* item = new QListWidgetItem(v.icon, v.title, viewList);
        item->setData(Qt::UserRole, v.key);
        item->setCheckState(v.shown->isChecked() ? Qt::Checked : Qt::Unchecked);
        if (!v.shown->isEnabled()) { // always there: drawn as the activity bar's menu shows it, and still moved
            item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
            item->setData(AlwaysRole, true);
        }
    }
    viewList->setItemDelegate(new AlwaysOnDelegate(viewList));
    viewList->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // all rows shown: as tall as they are
    viewList->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    viewList->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    auto saveOrder = [this, viewList] {
        QStringList keys;
        for (int row = 0; row < viewList->count(); ++row) keys << viewList->item(row)->data(Qt::UserRole).toString();
        settings_->setValue(QStringLiteral("sidebar/viewOrder"), keys);
        applyViewOrder();
    };
    connect(viewList->model(), &QAbstractItemModel::rowsMoved, &dialog, saveOrder);
    connect(viewList, &QListWidget::itemChanged, &dialog, [this](QListWidgetItem* item) {
        for (const SidebarView& v : sidebarViews_)
            if (v.key == item->data(Qt::UserRole).toString() && v.shown->isEnabled()) v.shown->setChecked(item->checkState() == Qt::Checked);
    });
    auto* moveButtons = new QVBoxLayout;
    std::vector<QToolButton*> movers;
    for (const auto& [icon, tip, by] : {std::tuple{"arrow-up", tr("Move up"), -1}, std::tuple{"arrow-down", tr("Move down"), 1}}) {
        auto* button = new QToolButton;
        button->setIcon(icons::lucide(icon));
        button->setToolTip(tip);
        connect(button, &QToolButton::clicked, &dialog, [viewList, saveOrder, by] {
            const int row = viewList->currentRow(), to = row + by;
            if (row < 0 || to < 0 || to >= viewList->count()) return;
            viewList->insertItem(to, viewList->takeItem(row));
            viewList->setCurrentRow(to);
            saveOrder();
        });
        moveButtons->addWidget(button);
        movers.push_back(button);
    }
    moveButtons->addStretch();
    auto* viewRow = new QHBoxLayout;
    viewRow->addWidget(viewList, 1);
    viewRow->addLayout(moveButtons);
    auto* viewsLayout = new QVBoxLayout(viewsBox);
    auto* viewsHint = new QLabel(tr("Drag them, or move the selected one, to put them in order; untick to hide one. "
                                    "Connections and Sessions are always there."));
    viewsHint->setWordWrap(true);
    viewsLayout->addWidget(viewsHint);
    viewsLayout->addLayout(viewRow);

    auto* pages = new QListWidget;
    auto* stack = new QStackedWidget;
    auto addPage = [pages, stack](const char* icon, const QString& name, std::initializer_list<QWidget*> sections) {
        auto* page = new QWidget;
        auto* layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        auto* title = new QLabel(name);
        QFont big; // family follows the window font
        big.setWeight(QFont::DemiBold);
#ifdef Q_OS_MACOS
        big.setPointSizeF(QApplication::font().pointSizeF() * 1.25);
#else
        big.setPointSizeF(QApplication::font().pointSizeF() * 1.3);
#endif
        title->setFont(big);
        layout->addWidget(title);
        bool tall = false; // a section that takes the height
        for (QWidget* section : sections) {
            layout->addWidget(section);
            tall = tall || section->sizePolicy().verticalPolicy() & QSizePolicy::ExpandFlag;
        }
        if (!tall) layout->addStretch();
        stack->addWidget(page);
        new QListWidgetItem(icons::lucide(icon), name, pages);
    };
    auto* tabs = new QGroupBox(tr("Tabs"));
    auto* closeButtons = new QCheckBox(tr("A close button on each tab"));
    closeButtons->setToolTip(tr("Without it a session ends from the menu or the toolbar (Disconnect)"));
    closeButtons->setChecked(settings_->value(QStringLiteral("tabs/closeButtons"), true).toBool());
    connect(closeButtons, &QCheckBox::toggled, &dialog, [this](bool on) {
        settings_->setValue(QStringLiteral("tabs/closeButtons"), on);
        tabs_->setTabsClosable(on);
    });
    (new QVBoxLayout(tabs))->addWidget(closeButtons);
    addPage("panels-top-left", tr("Layout"), {appearance, tabs});
    addPage("panel-left", tr("Sidebar"), {viewsBox, sidebar});
    const int sidebarRow = pages->count() - 1;
    addPage("type", tr("Font"), {mainFont, sshFont});
    auto* sshServers = new SshServersPage(settings_);
    addPage("server", tr("SSH Servers"), {sshServers});
    const int sshRow = pages->count() - 1;
    addPage("lock", tr("Passwords"), {secrets::page()});
    connect(pages, &QListWidget::currentRowChanged, stack, &QStackedWidget::setCurrentIndex);
    pages->setCurrentRow(page == QLatin1String("sshServers") ? sshRow : page == QLatin1String("sidebar") ? sidebarRow : 0);

    // OK, Cancel, Apply on every platform
    auto* ok = new QPushButton(tr("OK"));
    auto* cancel = new QPushButton(tr("Cancel"));
    auto* apply = new QPushButton(tr("Apply"));
    ok->setDefault(true);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    for (QPushButton* b : {ok, cancel, apply}) {
        b->setMinimumWidth(80);
        buttons->addWidget(b);
    }
    auto changed = [&] { apply->setEnabled(current() != applied || sshServers->pending()); };
    changed();
    // every setting the pages write passes here
    for (QObject* control : stack->findChildren<QObject*>()) {
        if (auto* slider = qobject_cast<QSlider*>(control)) connect(slider, &QSlider::valueChanged, &dialog, changed);
        if (auto* check = qobject_cast<QCheckBox*>(control)) connect(check, &QCheckBox::toggled, &dialog, changed);
        if (auto* radio = qobject_cast<QRadioButton*>(control)) connect(radio, &QRadioButton::toggled, &dialog, changed);
        if (auto* combo = qobject_cast<QComboBox*>(control)) connect(combo, &QComboBox::currentTextChanged, &dialog, changed);
        if (auto* button = qobject_cast<QPushButton*>(control)) connect(button, &QPushButton::clicked, &dialog, changed);
    }
    connect(sshServers, &SshServersPage::changed, &dialog, changed);
    connect(viewList->model(), &QAbstractItemModel::rowsMoved, &dialog, changed);
    connect(viewList, &QListWidget::itemChanged, &dialog, changed);
    for (QToolButton* button : movers) connect(button, &QToolButton::clicked, &dialog, changed);
    auto commit = [&] {
        sshServers->apply();
        hyperv_->reloadServers();
        applied = current();
        changed();
    };
    connect(apply, &QPushButton::clicked, &dialog, commit);
    connect(ok, &QPushButton::clicked, &dialog, [&] {
        commit();
        dialog.accept();
    });
    connect(cancel, &QPushButton::clicked, &dialog, &QDialog::reject);
    auto* body = new QHBoxLayout;
    body->setSpacing(12);
    body->addWidget(pageRail(pages));
    body->addWidget(stack, 1);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addLayout(body, 1);
    layout->addLayout(buttons);
    dialog.resize(720, 640);
    if (dialog.exec() == QDialog::Accepted || current() == applied) return;
    // Cancel, Escape or the close button
    for (const QString& group : groups) settings_->remove(group);
    for (auto it = applied.cbegin(); it != applied.cend(); ++it) settings_->setValue(it.key(), it.value());
    applyColorScheme(settings_);
    applySidebar();
    applyViewOrder();
    const QStringList hidden = applied.value(QStringLiteral("window/hiddenViews")).toStringList();
    for (const SidebarView& v : sidebarViews_)
        if (v.shown->isEnabled()) v.shown->setChecked(!hidden.contains(v.key));
    tabs_->setTabsClosable(settings_->value(QStringLiteral("tabs/closeButtons"), true).toBool());
    QFont font = look::systemFont(QFontDatabase::GeneralFont);
    if (const QString saved = settings_->value(QStringLiteral("font/main")).toString(); !saved.isEmpty()) font.fromString(saved);
    QApplication::setFont(font);
    for (int i = 0; i < tabs_->count(); ++i)
        if (auto* terminal = terminalOf(tabs_->widget(i))) configureTerminal(terminal);
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event)
{
    // set before the window has its size: QSplitter would spread it
    if (watched == splitter_ && event->type() == QEvent::Resize && sidebarMode_ == SidebarMode::Docked &&
        views_->parentWidget() == splitter_ && !views_->isHidden())
        splitter_->setSizes({panelWidth_, std::max(0, splitter_->width() - panelWidth_ - splitter_->handleWidth())});
    if (watched == body_ && event->type() == QEvent::Paint && body_->layout()->contentsMargins().top() > 0) {
        QPainter(body_).fillRect(0, 0, body_->width(), 1, lineColor());
    }
    if (event->type() == QEvent::Paint && ((watched == views_ && sidebarMode_ == SidebarMode::Overlay) || watched == activityBar_)) {
        auto* w = static_cast<QWidget*>(watched);
        QPainter(w).fillRect(w->width() - 1, 0, 1, w->height(), lineColor());
    }
    if (watched == sessionBar_ && event->type() == QEvent::Resize && !sessionBar_->property("capsules").toBool()) {
        if (sessionBar_->toolButtonStyle() == Qt::ToolButtonTextBesideIcon) sessionBarTextWidth_ = sessionBar_->sizeHint().width();
        const auto style = sessionBar_->width() >= sessionBarTextWidth_ ? Qt::ToolButtonTextBesideIcon : Qt::ToolButtonIconOnly;
        if (style != sessionBar_->toolButtonStyle()) sessionBar_->setToolButtonStyle(style);
    }
    if (watched == filter_ && event->type() == QEvent::KeyPress) {
        const int key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Down) {
            tree_->setFocus();
            return true;
        }
        if (key == Qt::Key_Escape && !filter_->text().isEmpty()) {
            filter_->clear();
            return true;
        }
    }
    if (sidebarMode_ == SidebarMode::Overlay) {
        const auto* w = qobject_cast<QWidget*>(watched);
        const bool outside = w && w->window() == this && w != views_ && !views_->isAncestorOf(w) && w != activityBar_ &&
                             !activityBar_->isAncestorOf(w);
        if ((event->type() == QEvent::MouseButtonPress && outside) ||
            (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape && w &&
             views_->isAncestorOf(w) && w != filter_))
            setSidebar(SidebarMode::Collapsed, -1);
        if (watched == body_ && event->type() == QEvent::Resize) applySidebarLayout();
    }
    if ((watched == activityBar_ || watched == views_ || watched == body_) &&
        (event->type() == QEvent::Resize || event->type() == QEvent::Move))
        applySidebar();
    return QMainWindow::eventFilter(watched, event);
}

QString MainWindow::scrcpyServerPath()
{
    // build tree: next to the executable; installed: share/portal-mux
    const QString dir = QCoreApplication::applicationDirPath();
    for (const QString& path : {dir + QStringLiteral("/scrcpy-server"), dir + QStringLiteral("/../share/portal-mux/scrcpy-server")})
        if (QFile::exists(path)) return path;
    return dir + QStringLiteral("/scrcpy-server");
}

const Connection* MainWindow::findConnection(const std::string& id) const
{
    if (const Connection* c = store_.find(id)) return c;
    for (const Connection& c : sshConfig_)
        if (c.id == id) return &c;
    return nullptr;
}

QString MainWindow::sshConfigGroup() { return tr("SSH config"); }

void MainWindow::updateActions()
{
    const Connection* c = findConnection(selectedId());
    for (QAction* a : {editAction_, connectAction_}) a->setEnabled(c != nullptr);
    deleteAction_->setEnabled(c && store_.find(c->id) && c->id != localTerminalId);
    sftpAction_->setEnabled(c && c->protocol == Protocol::SSH);
}

std::string MainWindow::selectedId() const
{
    QTreeWidgetItem* item = tree_->currentItem();
    return item ? item->data(0, IdRole).toString().toStdString() : std::string();
}

bool MainWindow::saveStore()
{
    QString error;
    if (store_.save(&error)) return true;
    QMessageBox::critical(this, tr("Connections"), tr("Could not save %1: %2").arg(store_.path(), error));
    return false;
}

void MainWindow::editConnection(Connection connection)
{
    QStringList groups;
    for (const Connection& c : store_.all())
        if (!c.group.empty() && !groups.contains(QString::fromStdString(c.group))) groups << QString::fromStdString(c.group);
    groups.sort();

    ConnectionDialog dialog(connection, groups, loadSshServers(settings_), this);
    if (dialog.exec() != QDialog::Accepted) return;
    const std::string id = store_.upsert(dialog.connection());
    if (!saveStore()) return;
    for (const auto& [key, value] : dialog.secrets())
        secrets::write(connectionSecret(id, key), value, this, [this](QString error) {
            if (!error.isEmpty())
                QMessageBox::warning(this, tr("Passwords"),
                                     tr("The password was not saved: %1\n"
                                        "You will be asked for it when connecting.")
                                         .arg(error));
        });
    rebuildTree(id);
}

void MainWindow::importFromKrdc()
{
    const QString bookmarksPath = QStandardPaths::locate(QStandardPaths::GenericDataLocation, QStringLiteral("krdc/bookmarks.xml"));
    if (bookmarksPath.isEmpty()) {
        QMessageBox::information(this, tr("Import from KRDC"), tr("No KRDC bookmarks were found."));
        return;
    }
    QFile bookmarks(bookmarksPath), config(QStandardPaths::locate(QStandardPaths::GenericConfigLocation, QStringLiteral("krdcrc")));
    if (!bookmarks.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, tr("Import from KRDC"), tr("Could not read %1: %2").arg(bookmarksPath, bookmarks.errorString()));
        return;
    }
    (void)config.open(QIODevice::ReadOnly); // optional: without it, default bookmark settings

    auto same = [](const Connection& a, const Connection& b) {
        return a.protocol == b.protocol && a.effectivePort() == b.effectivePort() &&
               QString::fromStdString(a.host).compare(QString::fromStdString(b.host), Qt::CaseInsensitive) == 0 &&
               QString::fromStdString(a.username).compare(QString::fromStdString(b.username), Qt::CaseInsensitive) == 0;
    };
    int added = 0, existing = 0;
    std::string last;
    for (Connection& c : importKrdc(bookmarks.readAll(), config.readAll())) {
        if (std::any_of(store_.all().begin(), store_.all().end(), [&](const Connection& o) { return same(o, c); })) {
            ++existing;
            continue;
        }
        last = store_.upsert(std::move(c));
        ++added;
    }
    if (added && !saveStore()) return;
    rebuildTree(last);
    QString text = tr("Imported %n connection(s).", "", added);
    if (existing) text += QLatin1Char(' ') + tr("%n already existed and were skipped.", "", existing);
    if (added) text += QLatin1Char(' ') + tr("Passwords stay in KWallet: enter them with Edit, or when asked while connecting.");
    QMessageBox::information(this, tr("Import from KRDC"), text);
}

void MainWindow::deleteSelected()
{
    const Connection* c = store_.find(selectedId());
    if (!c || c->id == localTerminalId) return;
    QMessageBox box(QMessageBox::Question, tr("Delete Connection"), tr("Delete \"%1\"?").arg(QString::fromStdString(c->name)),
                    QMessageBox::Yes | QMessageBox::No, this);
    box.setDefaultButton(QMessageBox::No);
    box.setOption(QMessageBox::Option::DontUseNativeDialog); // KDE's dialog: Enter presses the first button
    if (box.exec() != QMessageBox::Yes) return;
    const std::string id = c->id;
    const std::vector<std::string> keys = secretKeys(c->protocol);
    store_.remove(id); // c dangles from here on
    if (saveStore())
        for (const std::string& key : keys) secrets::remove(connectionSecret(id, key));
    rebuildTree();
}

bool MainWindow::serversKnown(const Connection& c)
{
    for (const char* key : {"tunnel", "jumpHost"}) {
        const QString id = QString::fromStdString(c.option(key));
        if (QUuid::fromString(id).isNull() || findSshServer(settings_, id)) continue;
        QMessageBox::warning(this, tr("SSH Servers"), tr("This connection's SSH server was removed in Settings. Choose another with Edit."));
        return false;
    }
    return true;
}

void MainWindow::connectTo(const std::string& id)
{
    const Connection* found = findConnection(id);
    if (!found) return;
    if (found->protocol == Protocol::RDP) // RDP: a second session would log the first out
        for (int i = 0; i < tabs_->count(); ++i)
            if (tabs_->widget(i)->property("connectionId") == QString::fromStdString(id)) {
                tabs_->setCurrentIndex(i);
                if (RemoteView* view = currentView()) view->setFocus();
                return;
            }
    if (!serversKnown(*found)) return;
    readSecrets(this, settings_, *found, [this](Connection c) { startSession(c); });
}

bool MainWindow::openAddress(const QString& text, Protocol fallback)
{
    const QUrl url(text.trimmed());
    const QString path = url.isLocalFile() ? url.toLocalFile() : text.trimmed(); // file managers hand over file:// URLs
    const QFileInfo file(path);
    static const QStringList suffixes{QStringLiteral("rdp"), QStringLiteral("vnc"), QStringLiteral("tigervnc")};
    QString error;
    const std::optional<Connection> c = url.isLocalFile() || (file.isFile() && suffixes.contains(file.suffix().toLower()))
                                            ? connectionFromFile(path, &error)
                                            : connectionFromAddress(text, fallback, &error);
    if (!c) {
        QMessageBox::warning(this, tr("Connect"), error);
        return false;
    }
    startSession(*c);
    return true;
}

void MainWindow::startSession(const Connection& c)
{
    runPreCommand(c, [this, c] { wakeHost(c, [this, c] { openSession(c); }); });
}

void MainWindow::runPreCommand(const Connection& c, std::function<void()> next)
{
    const QString command = QString::fromStdString(c.option("preCommand"));
    if (command.isEmpty()) return next();
    auto* process = new QProcess(this);
    setShellCommand(*process, command);
    process->setProcessEnvironment(commandEnvironment(c));
    auto* progress = new QProgressDialog(tr("Running the command before connecting to %1…").arg(QString::fromStdString(c.name)),
                                         tr("Cancel"), 0, 0, this);
    progress->setWindowTitle(tr("Connecting"));
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(500); // counted from setValue(0)
    progress->setValue(0);
    auto done = [process, progress] {
        progress->reset(); // deleteLater() waits out the message box below
        process->disconnect(); // no finished() after a cancel
        progress->disconnect();
        process->deleteLater();
        progress->deleteLater();
    };
    connect(progress, &QProgressDialog::canceled, this, done);
    connect(process, &QProcess::finished, this, [this, process, done, next](int code, QProcess::ExitStatus status) {
        done();
        if (status == QProcess::NormalExit && code == 0) return next();
        QString text = status == QProcess::NormalExit ? tr("The command before connecting ended with exit code %1.").arg(code)
                                                      : tr("The command before connecting crashed.");
        if (const QString output = QString::fromLocal8Bit(process->readAllStandardError()).trimmed(); !output.isEmpty())
            text += QStringLiteral("\n\n") + output.right(2000);
        QMessageBox box(QMessageBox::Warning, tr("Command before connecting"), text + QStringLiteral("\n\n") + tr("Connect anyway?"),
                        QMessageBox::Yes | QMessageBox::No, this);
        box.setDefaultButton(QMessageBox::No);
        box.setOption(QMessageBox::Option::DontUseNativeDialog); // KDE's dialog: Enter presses the first button
        if (box.exec() == QMessageBox::Yes) next();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process, done](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        done();
        QMessageBox::warning(this, tr("Command before connecting"), tr("The command could not be started: %1").arg(process->errorString()));
    });
    process->start();
}

void MainWindow::wakeHost(const Connection& c, std::function<void()> next)
{
    const QString mac = QString::fromStdString(c.option("wakeOnLan"));
    if (mac.isEmpty()) return next();
    const QByteArray packet = magicPacket(mac);
    if (packet.isEmpty()) {
        QMessageBox::warning(this, tr("Wake-on-LAN"), tr("\"%1\" is not a MAC address; connecting without waking the computer.").arg(mac));
        return next();
    }
    // shortcut: local-network broadcast only, use a subnet broadcast behind a router
    auto wake = [packet] { QUdpSocket().writeDatagram(packet, QHostAddress::Broadcast, 9); };
    wake();
    if (!c.option("tunnel").empty()) return next(); // tunnel: its state can't be seen from here
    auto* progress = new QProgressDialog(tr("Waking up %1…").arg(QString::fromStdString(c.name)), tr("Cancel"), 0, 120, this);
    progress->setWindowTitle(tr("Wake-on-LAN"));
    auto* seconds = new QProgressBar;
    seconds->setFormat(tr("%v s of %m s"));
    progress->setBar(seconds);
    progress->setRange(0, 120);
    progress->setWindowModality(Qt::WindowModal);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    auto* socket = new QTcpSocket(progress);
    auto* timer = new QTimer(progress);
    progress->setMinimumDuration(1500);
    progress->setValue(0);
    auto finish = [progress, socket, timer, next] {
        timer->stop();
        progress->reset();
        progress->hide();
        socket->disconnect();
        progress->disconnect();
        progress->deleteLater();
        next();
    };
    auto knock = [socket, wake, host = QString::fromStdString(c.host), port = quint16(c.effectivePort())] {
        wake(); // a packet can get lost
        socket->abort();
        socket->connectToHost(host, port);
    };
    const auto started = std::chrono::steady_clock::now();
    connect(socket, &QTcpSocket::connected, progress, finish);
    connect(timer, &QTimer::timeout, progress, [progress, finish, knock, started] {
        const auto waited = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started).count();
        if (waited >= 120) return finish();
        knock();
        progress->setValue(int(waited));
    });
    connect(progress, &QProgressDialog::canceled, progress, &QObject::deleteLater);
    timer->start(3000);
    knock();
}

void MainWindow::openSession(const Connection& c, QWidget* replace)
{
    int at = -1;
    if (replace) {
        at = tabs_->indexOf(replace);
        tabs_->removeTab(at);
        replace->deleteLater();
    }
    // runs once the tab closes, unless a reconnect replaced it
    auto armed = std::make_shared<bool>(true);
    auto runPostCommand = [this, &c, armed](QWidget* page) {
        const QString command = QString::fromStdString(c.option("postCommand"));
        if (command.isEmpty()) return;
        connect(page, &QObject::destroyed, [command, env = commandEnvironment(c), armed] { // no receiver: also runs when the app quits
            if (!*armed) return;
            QProcess process;
            setShellCommand(process, command);
            process.setProcessEnvironment(env);
            process.startDetached();
        });
    };
    const QString name = QString::fromStdString(c.name);

    if (c.protocol == Protocol::SSH || c.protocol == Protocol::Local || c.protocol == Protocol::Telnet || c.protocol == Protocol::Serial) {
        const bool local = c.protocol == Protocol::Local;
        TerminalView* terminal = addTerminalTab(c,
                                                local ? (c.option("shell").empty() ? tr("this computer")
                                                                                   : tr("this computer · %1").arg(QFileInfo(QString::fromStdString(c.option("shell"))).fileName()))
                                                : c.protocol == Protocol::Serial ? QStringLiteral("%1 · %2 baud").arg(QString::fromStdString(c.host)).arg(c.number("baud"))
                                                : QString::fromStdString(c.username.empty() ? c.host : c.username + "@" + c.host) +
                                                      (c.port > 0 ? QStringLiteral(":%1").arg(c.port) : QString()),
                                                at);
        runPostCommand(terminal);
        connect(terminal, &TerminalView::reconnectRequested, this, [this, terminal, c, armed] {
            *armed = false;
            openSession(c, terminal);
        });
        if (local) terminal->start(std::make_unique<rdc::LocalTerminalSession>(*terminal, c));
        else if (c.protocol != Protocol::SSH) terminal->start(std::make_unique<rdc::StreamTerminalSession>(*terminal, c));
        else {
            terminal->start(std::make_unique<rdc::SshTerminalSession>(*terminal, c, knownHostsFile()));
            addFilePanel(terminal, c);
        }
        terminal->setFocus();
        return;
    }
    QScrollArea* scroll = createDisplay(c);
    if (!scroll) return;
    auto* view = qobject_cast<RemoteView*>(scroll->widget());
    scroll->setProperty("connectionId", QString::fromStdString(c.id)); // connecting again shows this tab (RDP)
    addSessionTab(scroll, name, QString::fromStdString(c.host), at);
    runPostCommand(scroll);
    connect(view, &RemoteView::reconnectRequested, this, [this, scroll, c, armed] {
        *armed = false;
        openSession(c, scroll);
    });
    connect(view, &RemoteView::closeRequested, scroll, &QObject::deleteLater);
    view->setFocus();
    updateSessionActions(); // now that the view has its options
}

TerminalView* MainWindow::addTerminalTab(const Connection& c, const QString& address, int at, const QString& protocol)
{
    TerminalView* terminal = createTerminal(c, address, protocol);
    addSessionTab(terminal, QString::fromStdString(c.name), c.protocol == Protocol::Local ? tr("This computer") : QString::fromStdString(c.host), at);
    return terminal;
}

TerminalView* MainWindow::createTerminal(const Connection& c, const QString& address, const QString& protocol)
{
    const QString name = QString::fromStdString(c.name);
    auto* terminal = new TerminalView;
    terminal->setProperty("protocol", protocol.isEmpty() ? protocolLabel(c.protocol) : protocol);
    terminal->setProperty("address", address);
    connect(terminal, &TerminalView::gridChanged, this, &MainWindow::updateStatusInfo);
    connect(terminal, &TerminalView::stateChanged, this, [this, terminal, name](SessionState s) { showSessionState(terminal, name, s); });
    connect(terminal, &TerminalView::closeRequested, terminal, &QObject::deleteLater);
    terminal->setProperty("ownFont", QString::fromStdString(c.option("font")));
    terminal->setProperty("ownFontSize", c.number("fontSize"));
    terminal->setProperty("scrollback", c.number("scrollback"));
    configureTerminal(terminal);
    connect(terminal, &TerminalView::titleChanged, this, [this, terminal, host = QString::fromStdString(c.host)](const QString& title) {
        terminal->setProperty("title", title);
        tabs_->setTabToolTip(tabs_->indexOf(terminal), title.isEmpty() ? host : title);
        updateActiveList();
        updateWindowTitle();
    });
    connect(terminal, &TerminalView::bell, this, [this, terminal] {
        if (tabs_->currentWidget() != terminal) {
            tabs_->setTabIcon(tabs_->indexOf(terminal), icons::lucide("bell"));
            updateActiveList();
        }
        QApplication::alert(this); // only acts while the window is inactive
    });
    return terminal;
}

QScrollArea* MainWindow::createDisplay(const Connection& c)
{
    const QString name = QString::fromStdString(c.name);
    auto* view = new RemoteView;
    auto* scroll = new QScrollArea;
    scroll->setBackgroundRole(QPalette::Shadow);
    scroll->setWidget(view);
    scroll->setAlignment(Qt::AlignCenter);
    scroll->setProperty("protocol", protocolLabel(c.protocol));
    scroll->setProperty("address", !c.option("address").empty() ? QString::fromStdString(c.option("address")) // set by a manager
                                   : c.protocol == Protocol::Android && c.host.empty()
                                       ? tr("the only device")
                                       : QString::fromStdString(c.host) + (c.protocol == Protocol::Android ? QString()
                                                                                                           : QStringLiteral(":%1").arg(c.effectivePort())));
    connect(view, &RemoteView::infoChanged, this, &MainWindow::updateStatusInfo);
    connect(view, &RemoteView::stateChanged, this, [this, scroll, name](SessionState s) { showSessionState(scroll, name, s); });
    connect(view, &RemoteView::message, status_, [this](const QString& text, int timeoutMs) {
        text.isEmpty() ? status_->clearMessage() : status_->showMessage(text, timeoutMs);
    });
    view->setWindowShortcuts(fullScreenAction_->shortcuts());
    connect(view, &RemoteView::alwaysTrustChosen, this, [this, id = c.id] {
        const Connection* saved = store_.find(id);
        if (!saved) return;
        Connection updated = *saved;
        updated.options["alwaysTrust"] = "true";
        store_.upsert(std::move(updated));
        saveStore();
    });

    RemoteView::Options options;
    const std::string mode = c.option("scaling");
    options.mode = mode == "scale" ? RemoteView::Options::Mode::Scale
                 : mode == "none"  ? RemoteView::Options::Mode::None
                                   : RemoteView::Options::Mode::Resize;
    options.viewOnly = c.flag("viewOnly");
    options.clipboard = c.flag("clipboard");
    const bool fixed = c.protocol == Protocol::RDP && mode != "resize" && c.number("width") > 0 && c.number("height") > 0;
    if (fixed) options.desktopSize = QSize(c.number("width"), c.number("height"));
    options.fitRemote = !fixed && (c.protocol == Protocol::RDP || c.protocol == Protocol::VNC || c.protocol == Protocol::SPICE);
    if (c.protocol != Protocol::RDP) // RDP always needs a size; others keep the server's
        options.sendInitialSize = options.followsTab() || c.protocol == Protocol::Moonlight;

    Connection target = c;
    if (c.protocol == Protocol::RDP && c.flag("multimon") && QGuiApplication::screens().size() > 1) {
        // each screen in device pixels; its place in the remote desktop is relative to their bounding box
        QRect box;
        std::vector<std::pair<QScreen*, QRect>> screens;
        for (QScreen* screen : QGuiApplication::screens()) {
            const qreal dpr = screen->devicePixelRatio();
            const QRect g = screen->geometry();
            screens.emplace_back(screen, QRect(QPoint(qRound(g.x() * dpr), qRound(g.y() * dpr)), (QSizeF(g.size()) * dpr).toSize()));
            box |= screens.back().second;
        }
        QStringList defs, names;
        QVariantList parts;
        for (const auto& [screen, r] : screens) {
            defs << QStringLiteral("%1,%2,%3,%4,%5,%6").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height())
                        .arg(screen == QGuiApplication::primaryScreen()).arg(qRound(screen->devicePixelRatio() * 100));
            names << screen->name();
            parts << r.translated(-box.topLeft());
        }
        target.options["monitors"] = defs.join(QLatin1Char(';')).toStdString();
        scroll->setProperty("monitorScreens", names);
        scroll->setProperty("monitorParts", parts);
        options.fitRemote = false; // the tab shows it all, fitted
        options.mode = RemoteView::Options::Mode::Scale;
        options.desktopSize = box.size();
    }
    std::unique_ptr<rdc::SshTunnel> tunnel;
    if (const QString via = QString::fromStdString(c.option("tunnel")); !via.isEmpty()) {
        rdc::SshConnection::Params jump;
        if (const auto server = findSshServer(settings_, via)) {
            jump = rdc::SshTunnel::parse(server->address().toStdString());
            if (server->auth == QLatin1String("publicKey")) jump.keyFile = server->keyFile.toStdString();
            jump.password = c.option("tunnel.password");
        } else { // imported [user@]host[:port]
            jump = rdc::SshTunnel::parse(c.option("tunnel"));
            jump.keyFile = c.option("tunnel.keyFile");
        }
        jump.knownHostsFile = knownHostsFile();
        tunnel = std::make_unique<rdc::SshTunnel>(*view, jump, c.host, c.effectivePort());
        // same local port: stable host:port for RDP's cert store
        const int port = tunnel->listen(40000 + int(qHash(QString::fromStdString(c.id)) % 20000));
        if (!port) {
            QMessageBox::warning(this, tr("SSH tunnel"), tr("Could not open a local port for the tunnel."));
            delete scroll;
            return nullptr;
        }
        target.host = "127.0.0.1";
        target.port = port;
    }

    std::unique_ptr<rdc::DisplaySession> session;
    if (c.protocol == Protocol::Moonlight) {
        session = std::make_unique<rdc::MoonlightSession>(*view, target, (configDir_ + QStringLiteral("/moonlight")).toStdString());
    } else if (c.protocol == Protocol::Android) {
        session = std::make_unique<rdc::ScrcpySession>(*view, target, scrcpyServerPath().toStdString());
    } else if (c.protocol == Protocol::VNC) {
        session = std::make_unique<rdc::VncSession>(*view, target);
    } else if (c.protocol == Protocol::SPICE) {
        session = std::make_unique<rdc::SpiceDisplaySession>(*view, target);
    } else {
        if (target.number("keyboardLayout") == 0) target.options["keyboardLayout"] = std::to_string(detectKeyboardLayout());
        session = std::make_unique<rdc::RdpSession>(*view, target, configDir_.toStdString());
    }
    if (tunnel) session = std::make_unique<rdc::TunneledDisplaySession>(*view, std::move(tunnel), std::move(session));
    view->start(std::move(session), options);
    if (c.protocol == Protocol::Moonlight) {
        if (!gamepads_) gamepads_ = new Gamepads(this);
        view->setGamepads(gamepads_);
    }
    return scroll;
}

void MainWindow::openVmConsole(LibvirtVms* host, const std::string& uuid)
{
#ifdef PORTAL_MUX_LIBVIRT
    const QString key = host->uri() + QLatin1Char('/') + QString::fromStdString(uuid);
    for (int i = 0; i < tabs_->count(); ++i)
        if (tabs_->widget(i)->property("vmKey").toString() == key) {
            tabs_->setCurrentIndex(i);
            tabs_->widget(i)->setFocus();
            return;
        }
    const auto vm = std::find_if(host->vms().begin(), host->vms().end(), [&](const LibvirtVms::Vm& v) { return v.uuid == uuid; });
    if (vm == host->vms().end()) return;
    VmConsole* console = kvm_->console(host, uuid, [this, host, uuid](VmConsole* console) {
        const auto vm = std::find_if(host->vms().begin(), host->vms().end(), [&](const LibvirtVms::Vm& v) { return v.uuid == uuid; });
        if (vm == host->vms().end() || !vm->display) return;
        const auto& d = *vm->display;
        const QString where = host->remote() ? host->name() + QStringLiteral(" · ") : QString();
        console->setProperty("protocol", protocolLabel(d.protocol));
        console->setProperty("address", where + QStringLiteral("%1:%2").arg(QString::fromStdString(d.host)).arg(d.port > 0 ? d.port : d.number("tlsPort")));
        console->setDisplay(createDisplay(d));
    });
    console->setProperty("vmKey", key);
    console->setProperty("protocol", vm->display ? protocolLabel(vm->display->protocol) : QStringLiteral("SPICE"));
    addVmTab(console);
#else
    (void)host;
    (void)uuid;
#endif
}

void MainWindow::addFilePanel(TerminalView* terminal, const Connection& c)
{
    auto* files = new SftpView(rdc::SshConnection::fromConnection(c, knownHostsFile()), true);
    terminal->setSidePanel(files);
    terminal->setSidePanelShown(settings_->value(QStringLiteral("terminal/filePanel"), true).toBool());
    connect(terminal, &TerminalView::stateChanged, files, [terminal, files](SessionState s) {
        if (s == SessionState::Connected && terminal->sidePanelShown()) files->start(terminal->takeTypedPassword()); // its host key known now
    });
    connect(terminal, &TerminalView::directoryChanged, files, &SftpView::follow);
    connect(terminal, &TerminalView::titleChanged, files, &SftpView::followTitle);
    updateSessionActions(); // its Files button
}

void MainWindow::addVmTab(VmConsole* console)
{
    connect(console, &VmConsole::closeRequested, console, &QObject::deleteLater);
    connect(console, &VmConsole::changed, this, [this] {
        updateSessionActions();
        updateActiveList();
        updateStatusInfo();
    });
    addSessionTab(console, console->windowTitle(), tr("Virtual machine"));
    console->follow();
    if (!console->display()) console->setFocus();
    updateSessionActions();
}

void MainWindow::showProxmoxConsole(const QString& key)
{
    for (int i = 0; i < tabs_->count(); ++i)
        if (tabs_->widget(i)->property("proxmoxKey").toString() == key) {
            tabs_->setCurrentIndex(i);
            tabs_->widget(i)->setFocus();
            return;
        }
    VmConsole* console = proxmox_->console(key);
    if (!console) return;
    console->setProperty("proxmoxKey", key);
    console->setProperty("protocol", key.section(QLatin1Char('/'), 2, 2) == QLatin1String("lxc") ? QStringLiteral("Terminal") : QStringLiteral("VNC"));
    addVmTab(console);
}

void MainWindow::openProxmoxConsole(const Connection& c, const QString&, QWidget* replace, const QString& address, QObject* tunnel)
{
    auto* console = qobject_cast<VmConsole*>(replace);
    QScrollArea* scroll = console ? createDisplay(c) : nullptr;
    if (!scroll) {
        delete tunnel;
        return;
    }
    if (tunnel) tunnel->setParent(scroll);
    console->setProperty("protocol", protocolLabel(c.protocol));
    console->setProperty("address", address);
    console->setDisplay(scroll);
}

void MainWindow::openHyperVConsole(const QString& hostId, const std::string& vmId)
{
    const QString key = hostId + QLatin1Char('/') + QString::fromStdString(vmId);
    for (int i = 0; i < tabs_->count(); ++i)
        if (tabs_->widget(i)->property("hypervKey").toString() == key) {
            tabs_->setCurrentIndex(i);
            tabs_->widget(i)->setFocus();
            return;
        }
    VmConsole* console = hyperv_->console(hostId, vmId, [this](VmConsole* console, const Connection& c) {
        readSecrets(this, settings_, c, [this, target = QPointer<VmConsole>(console)](Connection c) {
            if (!target) return;
            if (c.password.empty()) c.password = c.option("tunnel.password"); // the server's Windows account
            target->setProperty("address", QString::fromStdString(c.option("address")));
            target->setDisplay(createDisplay(c));
        });
    });
    if (!console) return;
    console->setProperty("hypervKey", key);
    console->setProperty("protocol", QStringLiteral("RDP"));
    addVmTab(console);
}

void MainWindow::openAzureConsole(const QString& key)
{
    for (int i = 0; i < tabs_->count(); ++i)
        if (tabs_->widget(i)->property("azureKey").toString() == key) {
            tabs_->setCurrentIndex(i);
            tabs_->widget(i)->setFocus();
            return;
        }
    VmConsole* console = azure_->console(key, [this](VmConsole* console, const QString& name, const QString& address, const QUrl& url,
                                                     const QString& token) {
        Connection c;
        c.protocol = Protocol::Serial;
        c.name = name.toStdString();
        c.host = name.toStdString();
        TerminalView* terminal = createTerminal(c, address, {});
        console->setProperty("address", address);
        console->setDisplay(terminal);
        terminal->start(std::make_unique<AzureSerial>(*terminal, url, token));
    });
    if (!console) return;
    console->setProperty("azureKey", key);
    console->setProperty("protocol", protocolLabel(Protocol::Serial));
    addVmTab(console);
}

void MainWindow::openProxmoxTerminal(const QString&, QWidget* replace, const QString& name, const QString& address,
                                     const std::function<std::unique_ptr<rdc::TerminalSession>(rdc::TerminalListener&)>& make)
{
    auto* console = qobject_cast<VmConsole*>(replace);
    if (!console) return;
    Connection c;
    c.protocol = Protocol::Serial;
    c.name = name.toStdString();
    c.host = address.toStdString();
    TerminalView* terminal = createTerminal(c, address, QStringLiteral("Terminal"));
    console->setProperty("address", address);
    console->setDisplay(terminal);
    terminal->start(make(*terminal));
}

void MainWindow::openVmSerial(LibvirtVms* host, const std::string& uuid, QWidget* replace)
{
#ifdef PORTAL_MUX_LIBVIRT
    if (!host) return; // removed meanwhile
    const QString key = host->uri() + QLatin1Char('/') + QString::fromStdString(uuid);
    for (int i = 0; i < tabs_->count() && !replace; ++i) // one per VM
        if (tabs_->widget(i)->property("vmSerial").toString() == key) {
            tabs_->setCurrentIndex(i);
            tabs_->widget(i)->setFocus();
            return;
        }
    const auto vm = std::find_if(host->vms().begin(), host->vms().end(), [&](const LibvirtVms::Vm& v) { return v.uuid == uuid; });
    if (vm == host->vms().end()) return;
    int at = -1;
    if (replace) {
        at = tabs_->indexOf(replace);
        tabs_->removeTab(at);
        replace->deleteLater();
    }
    Connection c;
    c.protocol = Protocol::Serial;
    c.name = vm->name + " (serial)";
    c.host = vm->name;
    const QString where = host->remote() ? host->name() + QStringLiteral(" · ") : QString();
    TerminalView* terminal = addTerminalTab(c, where + tr("%1 · serial console").arg(QString::fromStdString(vm->name)), at);
    terminal->setProperty("vmSerial", key);
    connect(host, &QObject::destroyed, terminal, &QObject::deleteLater); // its host was removed
    connect(terminal, &TerminalView::reconnectRequested, this, [this, terminal, uuid, alive = QPointer<LibvirtVms>(host)] {
        openVmSerial(alive, uuid, terminal);
    });
    if (auto session = host->serialConsole(uuid, *terminal)) terminal->start(std::move(session));
    terminal->setFocus();
#else
    (void)host;
    (void)uuid;
    (void)replace;
#endif
}

std::string MainWindow::knownHostsFile() const { return (configDir_ + QStringLiteral("/known_hosts")).toStdString(); }

void MainWindow::openTestPattern()
{
    auto* view = new RemoteView;
    auto* scroll = new QScrollArea;
    scroll->setBackgroundRole(QPalette::Shadow);
    scroll->setWidget(view);
    scroll->setProperty("protocol", QStringLiteral("Test"));
    addSessionTab(scroll, tr("Test pattern"), {});
    view->setWindowShortcuts(fullScreenAction_->shortcuts());
    view->start(std::make_unique<rdc::TestPatternSession>(*view), RemoteView::Options{});
    view->setFocus();
    updateSessionActions();
}

void MainWindow::addSessionTab(QWidget* page, const QString& name, const QString& toolTip, int at)
{
    if (sidebarMode_ == SidebarMode::Overlay) setSidebar(SidebarMode::Collapsed, -1);
    const int index = at < 0 ? tabs_->addTab(page, name) : tabs_->insertTab(at, page, name);
    tabs_->setTabToolTip(index, toolTip);
    connect(page, &QObject::destroyed, this, &MainWindow::updateActiveList, Qt::QueuedConnection); // once the tab is gone
    tabs_->setCurrentIndex(index);
    updateActiveList();
}

void MainWindow::updateActiveList()
{
    static const char* const order[] = {"RDP", "VNC", "SPICE", "SSH", "SFTP", "Telnet", "Serial", "Terminal", "Android", "Moonlight", "Test"};
    const QSignalBlocker block(active_);
    const Rebuild quiet(active_);
    active_->clear();
    std::map<int, QTreeWidgetItem*> groups;
    auto groupOf = [&](QWidget* page) {
        const QString protocol = page->property("protocol").toString();
        const int rank = int(std::find_if(std::begin(order), std::end(order), [&](const char* p) { return protocol == QLatin1String(p); }) -
                             std::begin(order));
        QTreeWidgetItem*& group = groups[rank];
        if (!group) {
            group = new QTreeWidgetItem({protocol});
            group->setIcon(0, protocolIcon(protocol));
            group->setFlags(Qt::ItemIsEnabled);
            group->setData(0, FoldRole, protocol);
        }
        return group;
    };
    for (int i = 0; i < tabs_->count(); ++i) {
        auto* item = new QTreeWidgetItem(groupOf(tabs_->widget(i)), {tabs_->tabText(i)});
        item->setIcon(0, tabs_->tabIcon(i));
        item->setToolTip(0, tabs_->tabToolTip(i));
        item->setData(0, IdRole, i);
        if (i == tabs_->currentIndex()) active_->setCurrentItem(item);
    }
    for (QWidget* top : QApplication::topLevelWidgets()) {
        auto* window = dynamic_cast<DetachedWindow*>(top);
        if (!window || !window->page()) continue; // being reattached or closed
        auto* item = new QTreeWidgetItem(groupOf(window->page()), {tr("%1 (detached)").arg(window->title)});
        item->setToolTip(0, window->toolTip);
        item->setData(0, WindowRole, QVariant::fromValue<QObject*>(window));
    }
    for (auto& [rank, group] : groups) {
        group->setText(0, QStringLiteral("%1 (%2)").arg(group->text(0)).arg(group->childCount()));
        active_->addTopLevelItem(group);
        sessionsFolding_->apply(group);
    }
    for (int i = 0; i < active_->topLevelItemCount(); ++i)
        for (int j = 0; j < active_->topLevelItem(i)->childCount(); ++j)
            if (const QVariant id = active_->topLevelItem(i)->child(j)->data(0, IdRole); id.isValid() && id.toInt() == tabs_->currentIndex())
                active_->setCurrentItem(active_->topLevelItem(i)->child(j)); // after adding: items outside the tree can't be current
    fillSessionsMenu(); // also for macOS's menu search, which doesn't open it
}

void MainWindow::updateStatusInfo()
{
    QWidget* page = tabs_->currentWidget(); // the main window's tab, also while a detached window is active
    QString size, scale, fps, mode;
    if (RemoteView* view = page ? page->findChild<RemoteView*>() : nullptr) {
        const RemoteView::DisplayInfo d = view->displayInfo();
        if (d.size.isValid()) {
            size = QStringLiteral("%1×%2").arg(d.size.width()).arg(d.size.height());
            scale = d.percent == 100 ? tr("1:1") : tr("%1%").arg(d.percent);
            fps = tr("%1 fps").arg(d.fps);
        }
        mode = d.viewOnly ? tr("View only") : d.grabbed ? tr("Keys grabbed") : QString();
    } else if (auto* terminal = terminalOf(page)) {
        size = QStringLiteral("%1×%2").arg(terminal->grid().width()).arg(terminal->grid().height());
    }
    if (auto* console = qobject_cast<VmConsole*>(page)) mode = console->state();
    const auto set = [](QLabel* cell, const QString& text) {
        cell->setText(text);
        cell->setVisible(!text.isEmpty());
    };
    set(protocolCell_, page ? page->property("protocol").toString() : QString());
    set(addressCell_, page ? page->property("address").toString() : QString());
    set(sizeCell_, size);
    set(scaleCell_, scale);
    set(fpsCell_, fps);
    set(modeCell_, mode);
}

void MainWindow::showSessionState(QWidget* page, const QString& name, SessionState state)
{
    page->setProperty("sessionState", int(state));
    if (state == SessionState::Connected && !page->property("wasConnected").toBool()) { // not on reconnects in the same tab
        page->setProperty("wasConnected", true);
        if (QWidget* sessions = views_->findChild<QWidget*>(QStringLiteral("active"), Qt::FindDirectChildrenOnly);
            sessions && sidebarMode_ == SidebarMode::Docked)
            setSidebar(SidebarMode::Docked, views_->indexOf(sessions));
    }
    QWidget* current = tabs_->currentWidget();
    if (!current || (current != page && !current->isAncestorOf(page))) return; // another tab's
    static const char* text[] = {QT_TR_NOOP("Disconnected"), QT_TR_NOOP("Connecting…"), QT_TR_NOOP("Connected"), QT_TR_NOOP("Error")};
    status_->showMessage(QStringLiteral("%1: %2").arg(name, tr(text[int(state)])), state == SessionState::Connected ? 3000 : 0);
}

void MainWindow::styleStatusCells()
{
    status_->setStyleSheet(QStringLiteral("QStatusBar QLabel { border: 1px solid %1; border-radius: 2px; padding: 0 6px; }").arg(lineColor().name()));
}

void MainWindow::showUsbDevices()
{
    QPointer<RemoteView> view = currentView();
    if (!view || !view->session()) return;
    if (!view->session()->canRedirectUsb()) {
        QMessageBox::information(this, tr("USB Devices"),
                                 tr("This VM can't take USB devices: it needs USB redirectors (virt-manager: Add Hardware → USB "
                                    "Redirection, type SPICE channel), and the session must be connected."));
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle(tr("USB Devices"));
    auto* hint = new QLabel(tr("A ticked device belongs to the VM: this computer can't use it until you untick it or disconnect."));
    hint->setWordWrap(true);
    auto* list = new QListWidget;
    std::function<void()> fill = [&] {
        if (!view) return dialog.reject(); // the session closed
        const QSignalBlocker block(list);
        list->clear();
        for (const auto& d : view->session()->usbDevices()) {
            auto* item = new QListWidgetItem(QString::fromStdString(d.name), list);
            item->setData(Qt::UserRole, QString::fromStdString(d.id));
            item->setCheckState(d.redirected ? Qt::Checked : Qt::Unchecked);
        }
        if (!list->count()) new QListWidgetItem(tr("No USB devices found"), list);
    };
    connect(list, &QListWidget::itemChanged, &dialog, [&](QListWidgetItem* item) {
        if (!view || !item->data(Qt::UserRole).isValid()) return;
        item->setFlags(item->flags() & ~Qt::ItemIsEnabled); // until spice-glib answers
        view->session()->redirectUsb(item->data(Qt::UserRole).toString().toStdString(), item->checkState() == Qt::Checked,
                                     [&, alive = QPointer<QDialog>(&dialog)](std::string error) {
                                         if (!alive) return; // closed meanwhile: dialog and fill are gone
                                         if (!error.empty()) QMessageBox::warning(&dialog, tr("USB Devices"), QString::fromStdString(error));
                                         QTimer::singleShot(0, &dialog, fill); // not inside the item's own signal
                                     });
    });
    fill();
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(hint);
    layout->addWidget(list);
    layout->addWidget(buttons);
    dialog.resize(560, 360);
    dialog.exec();
}

void MainWindow::toggleMonitors(QWidget* page)
{
    const auto open = page->findChildren<QWidget*>(QStringLiteral("monitorWindow"), Qt::FindDirectChildrenOnly);
    for (QWidget* w : open) w->deleteLater();
    if (!open.isEmpty()) return;
    auto* view = page->findChild<RemoteView*>();
    const QStringList names = page->property("monitorScreens").toStringList();
    const QVariantList parts = page->property("monitorParts").toList();
    QPointer<QWidget> alive(page);
    QWidget* first = nullptr;
    for (int i = 0; view && i < names.size() && i < parts.size(); ++i) {
        const auto screens = QGuiApplication::screens();
        const auto screen = std::find_if(screens.begin(), screens.end(), [&](QScreen* s) { return s->name() == names[i]; });
        if (screen == screens.end()) continue; // unplugged since connecting
        auto* window = new MonitorWindow(view, parts[i].toRect(), *screen, fullScreenAction_->shortcut(),
                                         [this, alive] { if (alive) toggleMonitors(alive); }, page);
        window->setObjectName(QStringLiteral("monitorWindow"));
        window->showFullScreen();
        if (!first) first = window;
    }
    if (first) {
        first->activateWindow();
        first->setFocus();
        status_->showMessage(tr("%1 leaves full screen").arg(fullScreenAction_->shortcut().toString(QKeySequence::NativeText)), 5000);
    }
}

void MainWindow::updateNumLockCell()
{
    const auto locks = localLockKeys();
    numCell_->setVisible(locks.has_value());
    numCell_->setEnabled(locks && (*locks & rdc::NumLock)); // dimmed while off
}

void MainWindow::updateWindowTitle()
{
    const QWidget* page = tabs_->currentWidget();
    const QString title = page ? page->property("title").toString() : QString();
    setWindowTitle(title.isEmpty() ? QStringLiteral("Portal Mux") : tr("%1 — Portal Mux").arg(title));
}

void MainWindow::closeTab(int index)
{
    delete tabs_->widget(index);
}

bool MainWindow::confirmCloseTab(int index)
{
    QWidget* page = tabs_->widget(index);
    const auto* view = page ? page->findChild<RemoteView*>() : nullptr;
    const auto* terminal = terminalOf(page);
    if (!(view && view->live()) && !(terminal && terminal->live())) return true;
    QMessageBox box(QMessageBox::Question, tr("Close Tab"),
                    tr("Disconnect from \"%1\" and close its tab?").arg(tabs_->tabText(index).remove(QLatin1Char('&'))),
                    QMessageBox::Cancel, this);
    const QAbstractButton* disconnect = box.addButton(tr("&Disconnect"), QMessageBox::AcceptRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.setOption(QMessageBox::Option::DontUseNativeDialog);
    box.exec();
    return box.clickedButton() == disconnect;
}

QWidget* MainWindow::currentPage() const
{
    if (DetachedWindow* window = activeDetached()) return window->page();
    return tabs_->currentWidget();
}

RemoteView* MainWindow::currentView() const
{
    QWidget* page = currentPage();
    return page ? page->findChild<RemoteView*>() : nullptr;
}

void MainWindow::detachTab(int index)
{
    QWidget* page = tabs_->widget(index);
    if (!page) return;
    auto* window = new DetachedWindow(this, [this](DetachedWindow* w) { reattach(w); });
    window->title = tabs_->tabText(index);
    window->toolTip = tabs_->tabToolTip(index);
    window->setWindowTitle(tr("%1 — Portal Mux").arg(QString(window->title).remove(QLatin1Char('&'))));
    const QSize size = page->size();
    tabs_->removeTab(index);
    window->setCentralWidget(page);
    page->show(); // the tab widget hid it
    connect(page, &QObject::destroyed, window, &QObject::deleteLater); // the session ended
    auto* bar = window->addToolBar(tr("Session"));
    bar->setMovable(false);
    bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    capsules(bar, {8, 6, 8, 6});
    applyToolBarText();
    auto* attach = bar->addAction(icons::lucide("undo-2"), tr("&Attach to the Main Window"));
    attach->setToolTip(tr("Back into a tab (closing this window does the same)"));
    connect(attach, &QAction::triggered, window, [this, window] { reattach(window); });
    fillSessionBar(bar, QList<QAction*>{attach, nullptr} + sessionTools(page));
    window->resize(size + QSize(0, bar->sizeHint().height()));
    window->move(geometry().topLeft() + QPoint(48, 48));
    window->show();
    page->setFocus();
    updateActiveList();
    updateSessionActions();
    updateStatusInfo();
}

void MainWindow::reattach(QWidget* detached)
{
    auto* window = static_cast<DetachedWindow*>(detached);
    QWidget* page = window->takeCentralWidget();
    if (!page) return;
    disconnect(page, &QObject::destroyed, window, nullptr);
    if (window->isFullScreen()) window->setWindowState(window->windowState() & ~Qt::WindowFullScreen);
    window->deleteLater();
    const int index = tabs_->addTab(page, window->title);
    tabs_->setTabToolTip(index, window->toolTip);
    tabs_->setCurrentIndex(index);
    page->show();
    activateWindow();
    page->setFocus();
    updateActiveList();
}

QList<QAction*> MainWindow::sessionTools(QWidget* page) const
{
    if (!page) return {};
    const QString protocol = page->property("protocol").toString();
    auto is = [&](std::initializer_list<const char*> names) {
        return std::any_of(names.begin(), names.end(), [&](const char* name) { return protocol == QLatin1String(name); });
    };
    QList<QAction*> tools; // nullptr: a separator
    if (qobject_cast<VmConsole*>(page)) tools << page->actions() << nullptr; // its power buttons
    tools << fullScreenAction_;
    if (page->findChild<RemoteView*>()) {
        tools << scaleAction_ << nullptr << screenshotAction_;
        if (!is({"Moonlight", "Test"})) tools << clipboardAction_;
        if (!is({"Android", "Test"})) tools << nullptr << grabKeysAction_;
        if (!is({"Android", "Moonlight", "Test"})) tools << sendKeysMenu_->menuAction();
        if (is({"SPICE"})) tools << nullptr << usbAction_;
        if (is({"Moonlight"})) tools << nullptr << relaunchAction_;
    } else if (TerminalView* terminal = terminalOf(page)) {
        if (terminal->sidePanel()) tools << filesAction_;
        tools << nullptr << reconnectAction_;
    }
    return tools << nullptr << disconnectAction_;
}

void MainWindow::fillSessionBar(QToolBar* bar, const QList<QAction*>& tools)
{
    QList<QAction*> actions;
    int separator = 0;
    for (QAction* a : tools) {
        if (!a && separator == separators_.size()) { // as many as a toolbar needs: a VM's SPICE console has five
            separators_ << new QAction(this);
            separators_.last()->setSeparator(true);
        }
        actions << (a ? a : separators_.at(separator++));
    }
    if (bar->actions() == actions) return;
    bar->clear();
    bar->addActions(actions);
    if (auto* button = qobject_cast<QToolButton*>(bar->widgetForAction(sendKeysMenu_->menuAction())))
        button->setPopupMode(QToolButton::InstantPopup); // the menu opens on click
    if (bar == sessionBar_ && !bar->property("capsules").toBool()) { // as wide as it can, with text
        bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        sessionBarTextWidth_ = bar->sizeHint().width();
        if (bar->width() < sessionBarTextWidth_) bar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    }
}

void MainWindow::updateSessionActions()
{
    QWidget* page = tabs_->currentWidget();
    fillSessionBar(sessionBar_, sessionTools(page));
    fillSessionBar(fullScreenBar_, sessionTools(page));
    QList<QAction*> tools = sessionTools(page); // the toolbar's, then what has no button, Disconnect last
    if (page) {
        tools.resize(tools.size() - 2);
        if (page->findChild<RemoteView*>()) tools << nullptr << viewOnlyAction_ << localCursorAction_;
        tools << nullptr << detachAction_ << nullptr << disconnectAction_;
    }
    toolsMenu_->clear();
    for (QAction* a : tools) {
        if (a) toolsMenu_->addAction(a);
        else toolsMenu_->addSeparator();
    }
    if (toolsMenu_->isEmpty()) toolsMenu_->addAction(tr("No session"))->setEnabled(false);
    sessionBar_->setVisible(tabs_->count() > 0 && !isFullScreen() && toolBarAction_->isChecked());
    RemoteView* view = currentView();
    for (QAction* a : {screenshotAction_, viewOnlyAction_, clipboardAction_, localCursorAction_, grabKeysAction_, scaleAction_,
                       sendKeysMenu_->menuAction()})
        a->setEnabled(view);
    reconnectAction_->setEnabled(view || terminalOf(currentPage()));
    const TerminalView* terminal = terminalOf(currentPage());
    filesAction_->setEnabled(terminal && terminal->sidePanel());
    filesAction_->setChecked(terminal && terminal->sidePanelShown());
    usbAction_->setEnabled(view && currentPage()->property("protocol").toString() == QLatin1String("SPICE"));
    relaunchAction_->setEnabled(view && currentPage()->property("protocol").toString() == QLatin1String("Moonlight"));
    detachAction_->setEnabled(tabs_->count() > 0);
    fullScreenAction_->setEnabled(currentPage() || isFullScreen());
    disconnectAction_->setEnabled(currentPage());
    const RemoteView::Options o = view ? view->options() : RemoteView::Options{};
    viewOnlyAction_->setChecked(view && o.viewOnly);
    clipboardAction_->setChecked(view && o.clipboard);
    localCursorAction_->setChecked(view && o.localCursor);
    grabKeysAction_->setChecked(view && o.grabKeys);
    scaleAction_->setChecked(view && o.mode == RemoteView::Options::Mode::Scale);
}

void MainWindow::changeEvent(QEvent* event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        styleStatusCells();
        if (isVisible()) applySidebar(); // the tint was taken from the old palette
    }
    if (event->type() == QEvent::WindowStateChange && isFullScreen() != fullScreenPoll_.isActive())
        applyFullScreen(isFullScreen()); // also when the window manager switched it
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    for (QMainWindow* window : findChildren<QMainWindow*>(Qt::FindDirectChildrenOnly))
        if (dynamic_cast<DetachedWindow*>(window)) delete window; // their sessions end with the app
    settings_->setValue(QStringLiteral("window/geometry"), saveGeometry());
    if (sidebarMode_ == SidebarMode::Docked && views_->isVisible()) panelWidth_ = views_->width();
    settings_->setValue(QStringLiteral("window/sidebarWidth"), panelWidth_);
    settings_->setValue(QStringLiteral("window/sidebarView"), views_->currentWidget()->objectName());
    settings_->setValue(QStringLiteral("window/sidebarCollapsed"), sidebarMode_ != SidebarMode::Docked);
    QMainWindow::closeEvent(event);
}

std::vector<int> MainWindow::viewOrder() const
{
    std::vector<int> order;
    for (const QString& key : settings_->value(QStringLiteral("sidebar/viewOrder")).toStringList())
        for (int i = 0; i < int(sidebarViews_.size()); ++i)
            if (sidebarViews_[size_t(i)].key == key && std::find(order.begin(), order.end(), i) == order.end()) order.push_back(i);
    for (int i = 0; i < int(sidebarViews_.size()); ++i) // new ones, and all when none was moved
        if (std::find(order.begin(), order.end(), i) == order.end()) order.push_back(i);
    return order;
}

void MainWindow::applyViewOrder()
{
    const std::vector<int> order = viewOrder();
    activityBar_->setOrder(order);
    for (const SidebarView& v : sidebarViews_) activityBar_->removeAction(v.shown);
    for (int i : order) activityBar_->addAction(sidebarViews_[size_t(i)].shown);
    for (QAction* a : {sidebarSeparator_, sidebarSettings_}) {
        activityBar_->removeAction(a);
        activityBar_->addAction(a);
    }
}

void MainWindow::fillSessionsMenu()
{
    sessionsMenu_->clear();
    auto add = [this](QWidget* page, const QString& text, bool current, std::function<void()> show) {
        QAction* a = sessionsMenu_->addAction(protocolIcon(page->property("protocol").toString()), text);
        a->setCheckable(true);
        a->setChecked(current);
        connect(a, &QAction::triggered, this, show);
    };
    for (int i = 0; i < tabs_->count(); ++i)
        add(tabs_->widget(i), tabs_->tabText(i), i == tabs_->currentIndex(), [this, page = QPointer<QWidget>(tabs_->widget(i))] {
            if (!page) return;
            tabs_->setCurrentIndex(tabs_->indexOf(page));
            if (auto* view = page->findChild<RemoteView*>()) view->setFocus();
            else page->setFocus();
        });
    for (QWidget* top : QApplication::topLevelWidgets())
        if (auto* window = dynamic_cast<DetachedWindow*>(top); window && window->page())
            add(window->page(), tr("%1 (detached)").arg(window->title), false, [window = QPointer<DetachedWindow>(window)] {
                if (!window || !window->page()) return;
                window->raise();
                window->activateWindow();
                window->page()->setFocus();
            });
    if (sessionsMenu_->isEmpty()) sessionsMenu_->addAction(tr("No sessions"))->setEnabled(false);
}

void MainWindow::showView(QWidget* page, bool on)
{
    const int index = views_->indexOf(page);
    activityBar_->setViewHidden(index, !on);
    if (!on && views_->currentIndex() == index) setSidebar(sidebarMode_ == SidebarMode::Docked ? SidebarMode::Docked : SidebarMode::Collapsed, 0);
    QStringList hidden = settings_->value(QStringLiteral("window/hiddenViews")).toStringList();
    hidden.removeAll(page->objectName());
    if (!on) hidden << page->objectName();
    settings_->setValue(QStringLiteral("window/hiddenViews"), hidden);
}

void MainWindow::setSidebar(SidebarMode mode, int view)
{
    if (sidebarMode_ == SidebarMode::Docked && views_->isVisible() && views_->parentWidget() == splitter_)
        panelWidth_ = views_->width();
    if (view >= 0) views_->setCurrentIndex(view);
    const bool floating = views_->parentWidget() != splitter_;
    if (mode == SidebarMode::Overlay && !floating) {
        views_->setParent(body_);
        views_->setContentsMargins(0, 0, 1, 0); // room for its border
        qApp->installEventFilter(this);
    } else if (mode != SidebarMode::Overlay && floating) {
        qApp->removeEventFilter(this);
        views_->setContentsMargins(0, 0, 0, 0);
        splitter_->insertWidget(0, views_);
    }
    const bool docking = mode == SidebarMode::Docked && sidebarMode_ != SidebarMode::Docked;
    sidebarMode_ = mode;
    activityBar_->setCurrent(mode == SidebarMode::Collapsed ? -1 : views_->currentIndex());
    activityBar_->update();
    applySidebarLayout();
    if (docking) splitter_->setSizes({panelWidth_, std::max(0, splitter_->width() - panelWidth_ - splitter_->handleWidth())});
}

void MainWindow::applySidebarLayout()
{
    const bool shown = sidebarAction_->isChecked() && !isFullScreen();
    activityBar_->setVisible(shown);
    views_->setVisible(shown && sidebarMode_ != SidebarMode::Collapsed);
    if (sidebarMode_ == SidebarMode::Overlay) {
        const int top = body_->layout()->contentsMargins().top();
        views_->setGeometry(activityBar_->width(), top, std::min(panelWidth_, body_->width() - activityBar_->width()), body_->height() - top);
        views_->raise();
    }
    if (isVisible()) applySidebar();
}

QMenu* MainWindow::createPopupMenu()
{
    auto* menu = new QMenu(this);
    menu->addActions(QList<QAction*>{toolBarAction_, sidebarAction_} + viewToggles_ + QList<QAction*>{statusBarAction_, tabBarAction_});
    if (toolBarStyle_) {
        menu->addSeparator();
        menu->addActions(toolBarStyle_->actions());
    }
    return menu;
}

void MainWindow::applyToolBarText()
{
    const bool labelled = settings_->value(QStringLiteral("view/toolBarText"), false).toBool();
    for (QWidget* top : QApplication::topLevelWidgets())
        for (QToolBar* bar : top->findChildren<QToolBar*>())
            if (bar->property("capsules").toBool()) bar->setToolButtonStyle(labelled ? Qt::ToolButtonTextUnderIcon : Qt::ToolButtonIconOnly);
}

void MainWindow::applyFullScreen(bool on)
{
    menuBar()->setVisible(!on);
    for (auto [w, shown] : std::initializer_list<std::pair<QWidget*, QAction*>>{
             {status_, statusBarAction_},
             {tabs_->tabBar(), tabBarAction_}})
        w->setVisible(!on && shown->isChecked());
    if (on && sidebarMode_ == SidebarMode::Overlay) setSidebar(SidebarMode::Collapsed, -1);
    body_->layout()->setContentsMargins(0, on ? 0 : 1, 0, 0);
    applySidebarLayout();
    fullScreenAction_->setText(on ? tr("Leave &Full Screen Mode") : tr("Switch to &Full Screen Mode"));
    fullScreenAction_->setIconText(on ? tr("Leave Full Screen") : tr("Full Screen"));
    fullScreenAction_->setIcon(icons::lucide(on ? "minimize" : "maximize"));
    if (on) {
        fullScreenPoll_.start();
    } else {
        fullScreenPoll_.stop();
        fullScreenBar_->hide();
    }
    updateSessionActions();
    if (isVisible()) applySidebar();
}
