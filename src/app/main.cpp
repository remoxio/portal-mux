#include "app/Look.h"
#include "app/MainWindow.h"

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QCommandLineParser>
#include <QDialog>
#include <QIcon>
#include <QLayout>
#include <QTimer>
#include <QWindow>

#ifdef _WIN32
#include <BreezeIcons>
#include <KIconTheme>
#endif

#include <glib.h>

namespace {

// dialogs: a close button only
class DialogFrames : public QObject {
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        auto* dialog = qobject_cast<QDialog*>(watched);
        if (!dialog || !dialog->isWindow()) return false;
        if (event->type() == QEvent::Show) {
            const Qt::WindowFlags flags =
                (dialog->windowFlags() | Qt::CustomizeWindowHint) & ~(Qt::WindowMinMaxButtonsHint | Qt::WindowContextHelpButtonHint);
            dialog->overrideWindowFlags(flags);
            if (QWindow* window = dialog->windowHandle()) window->setFlags(flags);
            fit(dialog);
        } else if (event->type() == QEvent::LayoutRequest && dialog->isVisible()) {
            QTimer::singleShot(0, dialog, [dialog] { fit(dialog); });
        }
        return false;
    }

private:
    static void fit(QDialog* dialog) // KWin on Wayland ignores the flags: a fixed size hides maximize
    {
        if (!QGuiApplication::platformName().startsWith(QLatin1String("wayland")) || !dialog->layout() ||
            QByteArray(dialog->metaObject()->className()).contains("FileDialog"))
            return;
        dialog->setFixedSize(dialog->size().expandedTo(dialog->layout()->totalMinimumSize()));
    }
};

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("portal-mux"));
    QApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("portal-mux")); // Wayland app id: the launcher's icon
#ifndef Q_OS_MACOS // there it would replace the Dock's icon, the bundle's has the background
    QIcon icon;
    for (int size : {16, 22, 24, 28, 32, 48, 64, 96, 128, 256}) icon.addFile(QStringLiteral(":/hicolor/%1x%1/apps/portal-mux.png").arg(size));
    QApplication::setWindowIcon(icon);
#endif
    look::apply();
#ifdef _WIN32
    // Breeze, recoloured from the palette by KIconThemes' engine, as on KDE
    BreezeIcons::initIcons();
    QCoreApplication::addLibraryPath(QCoreApplication::applicationDirPath() + QStringLiteral("/kiconthemes6"));
    KIconTheme::forceThemeForTests(QStringLiteral("breeze")); // initTheme() without its Breeze palette
    QIcon::setThemeName(QStringLiteral("KIconEngine"));
#endif
    app.installEventFilter(new DialogFrames(&app));
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Remote desktop client for RDP, VNC, SPICE and SSH."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("address"),
                                 QStringLiteral("Connect to it without saving: host (RDP), user@host:port, an rdp://, vnc://, spice:// or "
                                                "ssh:// address, or an .rdp or .vnc file."),
                                 QStringLiteral("[address...]"));
    parser.process(app);

    // spice-glib uses GLib's default context; Qt drives it on Linux, else pump it
    if (!QAbstractEventDispatcher::instance()->inherits("QEventDispatcherGlib")) {
        auto* pump = new QTimer(&app);
        QObject::connect(pump, &QTimer::timeout, [] { while (g_main_context_iteration(nullptr, FALSE)) {} });
        pump->start(5);
    }

    MainWindow window;
    window.show();
    // shortcut: no single instance yet
    for (const QString& address : parser.positionalArguments()) window.openAddress(address);
    return app.exec();
}
