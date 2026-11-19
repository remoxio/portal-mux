#include "app/Keymap.h"

#include "core/Properties.h"

#include <QKeyEvent>
#include <QString>

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
#include <QDBusInterface>
#endif

#include <winpr/input.h>

uint32_t xtScancode(const QKeyEvent* event)
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    // nativeScanCode() is the XKB keycode on both X11 and Wayland.
    const DWORD vk = GetVirtualKeyCodeFromKeycode(event->nativeScanCode(), WINPR_KEYCODE_TYPE_XKB);
    return GetVirtualScanCodeFromVirtualKeyCode(vk, WINPR_KBD_TYPE_IBM_ENHANCED);
#elif defined(Q_OS_MACOS)
    // nativeVirtualKey() is the key's code (kVK_*), as WinPR's table wants it; Command is the Windows key
    const DWORD vk = GetVirtualKeyCodeFromKeycode(event->nativeVirtualKey(), WINPR_KEYCODE_TYPE_APPLE);
    return GetVirtualScanCodeFromVirtualKeyCode(vk, WINPR_KBD_TYPE_IBM_ENHANCED);
#else
    // shortcut: Windows scancodes come with that port
    (void)event;
    return 0;
#endif
}

uint32_t keysym(const QKeyEvent* event)
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    return event->nativeVirtualKey();
#else
    (void)event;
    return 0;
#endif
}

uint32_t detectKeyboardLayout()
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    // systemd-localed: the desktop's layout, X11 and Wayland
    static const uint32_t detected = [] {
        QDBusInterface locale1(QStringLiteral("org.freedesktop.locale1"), QStringLiteral("/org/freedesktop/locale1"),
                               QStringLiteral("org.freedesktop.locale1"), QDBusConnection::systemBus());
        const QString first = locale1.property("X11Layout").toString().section(QLatin1Char(','), 0, 0).trimmed();
        for (const rdc::KeyboardLayout& l : rdc::keyboardLayouts())
            if (first == QLatin1String(l.xkb)) return l.klid;
        return 0u;
    }();
    return detected;
#else
    return 0; // shortcut: Linux only, Windows/macOS when ported
#endif
}
