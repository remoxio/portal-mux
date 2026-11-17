#include "app/About.h"

#include <QApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

#include <span>

namespace {

struct Part {
    const char* name;
    const char* what;
    const char* licence;
    const char* website;
};

const Part shipped[] = {
    {"moonlight-common-c", QT_TRANSLATE_NOOP("About", "Game streaming with Sunshine, Apollo and GeForce hosts"), "GPL 3.0",
     "https://github.com/moonlight-stream/moonlight-common-c"},
    {"ENet", QT_TRANSLATE_NOOP("About", "Moonlight's network transport"), "MIT", "http://enet.bespin.org"},
    {"nanors", QT_TRANSLATE_NOOP("About", "Moonlight's error correction"), "MIT", "https://github.com/sleepybishop/nanors"},
    {"scrcpy", QT_TRANSLATE_NOOP("About", "Android screens: its server runs on the device"), "Apache 2.0", "https://github.com/Genymobile/scrcpy"},
    {"Material Icon Theme", QT_TRANSLATE_NOOP("About", "Folder icons"), "MIT", "https://github.com/material-extensions/vscode-material-icon-theme"},
    {"Tabler Icons", QT_TRANSLATE_NOOP("About", "Icons"), "MIT", "https://tabler.io/icons"},
    {"Lucide", QT_TRANSLATE_NOOP("About", "Sidebar icons"), "ISC, MIT", "https://lucide.dev"},
};

const Part libraries[] = {
    {"Qt 6", QT_TRANSLATE_NOOP("About", "The window, network, websockets and sound"), "LGPL 3.0", "https://www.qt.io"},
    {"QtKeychain", QT_TRANSLATE_NOOP("About", "Passwords in the system's keychain"), "BSD 3-Clause", "https://github.com/frankosterfeld/qtkeychain"},
#ifdef Q_OS_LINUX
    {"KWindowSystem", QT_TRANSLATE_NOOP("About", "Blur behind the sidebar on KDE Plasma"), "LGPL 2.1 or later",
     "https://invent.kde.org/frameworks/kwindowsystem"},
#endif
    {"FreeRDP", QT_TRANSLATE_NOOP("About", "RDP"), "Apache 2.0", "https://www.freerdp.com"},
    {"LibVNCClient", QT_TRANSLATE_NOOP("About", "VNC"), "GPL 2.0 or later", "https://libvnc.github.io"},
    {"spice-gtk", QT_TRANSLATE_NOOP("About", "SPICE (its spice-client-glib library)"), "LGPL 2.1 or later", "https://www.spice-space.org"},
    {"GLib", QT_TRANSLATE_NOOP("About", "SPICE's event loop"), "LGPL 2.1 or later", "https://docs.gtk.org/glib/"},
    {"libssh", QT_TRANSLATE_NOOP("About", "SSH, SFTP and SSH tunnels"), "LGPL 2.1", "https://www.libssh.org"},
    {"libvterm", QT_TRANSLATE_NOOP("About", "The terminal"), "MIT", "https://www.leonerd.org.uk/code/libvterm/"},
    {"FFmpeg", QT_TRANSLATE_NOOP("About", "Video and audio of Android and Moonlight"), "LGPL 2.1 or later", "https://ffmpeg.org"},
    {"OpenSSL", QT_TRANSLATE_NOOP("About", "Moonlight pairing and TLS"), "Apache 2.0", "https://www.openssl.org"},
    {"SDL 3", QT_TRANSLATE_NOOP("About", "Game controllers"), "zlib", "https://www.libsdl.org"},
#ifdef PORTAL_MUX_LIBVIRT
    {"libvirt", QT_TRANSLATE_NOOP("About", "The KVM Manager"), "LGPL 2.1 or later", "https://libvirt.org"},
#endif
#ifdef PORTAL_MUX_WAYLAND
    {"wayland-protocols", QT_TRANSLATE_NOOP("About", "Grabbing all keys on Wayland"), "MIT", "https://gitlab.freedesktop.org/wayland/wayland-protocols"},
#endif
};

QString tr(const char* text) { return QApplication::translate("About", text); }

} // namespace

void showAbout(QWidget* parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(tr("About Portal Mux"));
    const QString dim = dialog.palette().color(QPalette::PlaceholderText).name();

    auto* icon = new QLabel;
    icon->setPixmap(QApplication::windowIcon().pixmap(72, 72));
    icon->setAlignment(Qt::AlignTop);
    auto* name = new QLabel(QStringLiteral("Portal Mux"));
    QFont big = name->font();
    big.setPointSizeF(big.pointSizeF() * 1.6);
    big.setWeight(QFont::DemiBold);
    name->setFont(big);
    auto* facts = new QLabel(tr("<span style='color:%1'>Version %2</span><br>Remote access to computers, VMs, phones and game "
                                "hosts, free software.<br>Licence: <a href='https://www.gnu.org/licenses/gpl-3.0.html'>GNU General "
                                "Public License 3.0 or later</a>")
                                 .arg(dim, QApplication::applicationVersion()));
    facts->setOpenExternalLinks(true);
    auto* about = new QVBoxLayout;
    about->setSpacing(4);
    about->addWidget(name);
    about->addWidget(facts);
    auto* header = new QHBoxLayout;
    header->setSpacing(16);
    header->addWidget(icon);
    header->addLayout(about, 1);

    auto* line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    auto* madeWith = new QLabel(tr("<b>Made with the work of these projects</b>"));

    // each project: its name to its site, its licence, what it does here
    QString list;
    for (const auto& [title, parts] : {std::pair{tr("Comes with it"), std::span<const Part>(shipped)},
                                       std::pair{tr("Built on"), std::span<const Part>(libraries)}}) {
        list += QStringLiteral("<p style='margin-top:10px'><b>%1</b></p>").arg(title);
        for (const Part& p : parts)
            list += QStringLiteral("<p style='margin:4px 0'><a href='%1'>%2</a>&nbsp;&nbsp;<small style='color:%3'>%4</small><br>"
                                   "<small style='color:%3'>%5</small></p>")
                        .arg(QLatin1String(p.website), QLatin1String(p.name), dim, QLatin1String(p.licence), tr(p.what));
    }
    list += QStringLiteral("<p style='margin-top:12px'><small style='color:%1'>%2</small></p>")
                .arg(dim, tr("Proxmox® is a registered trademark of Proxmox Server Solutions GmbH. Other product names and "
                             "logos are trademarks of their owners."));
    auto* parts = new QLabel(list);
    parts->setWordWrap(true);
    parts->setOpenExternalLinks(true);
    parts->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    parts->setContentsMargins(0, 0, 12, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidget(parts);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setFixedHeight(340);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(12);
    layout->addLayout(header);
    layout->addWidget(line);
    layout->addWidget(madeWith);
    layout->addWidget(scroll);
    layout->addWidget(buttons);
    dialog.resize(600, dialog.sizeHint().height());
    dialog.exec();
}
