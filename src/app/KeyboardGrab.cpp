#include "app/KeyboardGrab.h"

#include <QGuiApplication>
#include <QWidget>
#include <QWindow>

#ifdef PORTAL_MUX_WAYLAND
#include "qwayland-keyboard-shortcuts-inhibit-unstable-v1.h"

#include <QWaylandClientExtensionTemplate>
#include <qpa/qplatformnativeinterface.h>

#include <map>

namespace {

class InhibitManager : public QWaylandClientExtensionTemplate<InhibitManager>,
                       public QtWayland::zwp_keyboard_shortcuts_inhibit_manager_v1 {
public:
    InhibitManager() : QWaylandClientExtensionTemplate(1) { initialize(); }
};

// the protocol forbids a second inhibitor per surface
std::map<QWindow*, zwp_keyboard_shortcuts_inhibitor_v1*> inhibitors;

void inhibit(QWindow* window, bool on)
{
    static auto* manager = new InhibitManager; // leaked on purpose: destroying it after the display is gone crashes
    auto it = inhibitors.find(window);
    if (!on) {
        if (it == inhibitors.end()) return;
        zwp_keyboard_shortcuts_inhibitor_v1_destroy(it->second);
        inhibitors.erase(it);
        return;
    }
    if (it != inhibitors.end() || !manager->isActive()) return;
    QPlatformNativeInterface* native = QGuiApplication::platformNativeInterface();
    auto* surface = static_cast<wl_surface*>(native->nativeResourceForWindow("surface", window));
    auto* seat = static_cast<wl_seat*>(native->nativeResourceForIntegration("wl_seat"));
    if (surface && seat) inhibitors[window] = manager->inhibit_shortcuts(surface, seat);
}

} // namespace
#endif

void keyboardGrab::set(QWidget* widget, bool on)
{
    // shortcut: no Alt+Tab, Win/Cmd on Windows/macOS, needs a keyboard hook/event tap
    if (on)
        widget->grabKeyboard();
    else
        widget->releaseKeyboard();
#ifdef PORTAL_MUX_WAYLAND
    if (QGuiApplication::platformName().startsWith(QLatin1String("wayland")))
        if (QWindow* window = widget->window()->windowHandle()) inhibit(window, on);
#endif
}
