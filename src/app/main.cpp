#include "app/MainWindow.h"

#include <QAbstractEventDispatcher>
#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QTimer>

#include <glib.h>

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("portal-mux"));
    QApplication::setApplicationVersion(QStringLiteral(PROJECT_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("portal-mux")); // Wayland app id: the launcher's icon
    QIcon icon;
    for (int size : {16, 22, 24, 28, 32, 48, 64, 96, 128, 256}) icon.addFile(QStringLiteral(":/hicolor/%1x%1/apps/portal-mux.png").arg(size));
    QApplication::setWindowIcon(icon);
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
    // shortcut: one window per start, hand addresses to a running instance (D-Bus/QLocalServer) if needed
    for (const QString& address : parser.positionalArguments()) window.openAddress(address);
    return app.exec();
}
