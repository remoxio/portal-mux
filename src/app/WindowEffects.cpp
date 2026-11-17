#include "app/WindowEffects.h"

#include <QRegion>
#include <QWidget>

#if defined(Q_OS_WIN)

#include <QOperatingSystemVersion>

#include <windows.h>

bool windowEffects::translucencyPossible() { return QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows10; }

// shortcut: see-through only, no blur (Win10 blurs the native title bar too);
// upgrade with our own title bar or Windows 11's DWMWA_SYSTEMBACKDROP_TYPE
bool windowEffects::blurAvailable() { return false; }

bool windowEffects::setBlurBehind(QWidget* window, bool, const QRegion&)
{
    // Undocumented but stable since Windows 10 1803 (gpui and Electron use it too).
    struct AccentPolicy {
        int state, flags;
        DWORD gradientColor;
        int animationId;
    };
    struct CompositionData {
        int attribute;
        void* data;
        SIZE_T size;
    };
    using SetWindowCompositionAttribute = BOOL(WINAPI*)(HWND, CompositionData*);
    static const auto set = reinterpret_cast<SetWindowCompositionAttribute>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute")));
    if (!set) return false;
    AccentPolicy accent{4 /* ACCENT_ENABLE_ACRYLICBLURBEHIND */, 0, 0, 0};
    CompositionData data{19 /* WCA_ACCENT_POLICY */, &accent, sizeof accent};
    set(reinterpret_cast<HWND>(window->winId()), &data);
    return false;
}

#elif !defined(Q_OS_MACOS) // Linux and other Unix desktops; macOS is in WindowEffects_mac.mm

#include <KWindowEffects>

#include <QGuiApplication>

bool windowEffects::translucencyPossible()
{
    // binds the blur protocol; capabilities arrive later (Wayland)
    const bool now = KWindowEffects::isEffectAvailable(KWindowEffects::BlurBehind);
    return now || QGuiApplication::platformName() == QLatin1String("wayland"); // Wayland always composites
}

bool windowEffects::blurAvailable() { return KWindowEffects::isEffectAvailable(KWindowEffects::BlurBehind); }

bool windowEffects::setBlurBehind(QWidget* window, bool enable, const QRegion& region)
{
    if (!window->windowHandle()) return false;
    enable = enable && blurAvailable();
    KWindowEffects::enableBlurBehind(window->windowHandle(), enable, region);
    return enable;
}

#endif
