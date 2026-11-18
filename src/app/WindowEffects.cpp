#include "app/WindowEffects.h"

#include <QRegion>
#include <QWidget>

#if defined(Q_OS_WIN)

#include <QOperatingSystemVersion>

#include <windows.h>

bool windowEffects::translucencyPossible() { return QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows10; }

// as spark-commander: Transparency effects on, and not Remote Desktop, which never blurs
bool windowEffects::blurAvailable()
{
    DWORD on = 1, size = sizeof on;
    RegGetValueW(HKEY_CURRENT_USER, LR"(Software\Microsoft\Windows\CurrentVersion\Themes\Personalize)", L"EnableTransparency",
                 RRF_RT_REG_DWORD, nullptr, &on, &size);
    return on && !GetSystemMetrics(SM_REMOTESESSION);
}

bool windowEffects::setBlurBehind(QWidget* window, bool enable, const QRegion&)
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
    enable = enable && blurAvailable();
    // as gpui: acrylic needs a non-zero alpha; else transparent gradient
    AccentPolicy accent = enable ? AccentPolicy{4 /* ACCENT_ENABLE_ACRYLICBLURBEHIND */, 0, 0x01000000, 0}
                                 : AccentPolicy{2 /* ACCENT_ENABLE_TRANSPARENTGRADIENT */, 2, 0, 0};
    CompositionData data{19 /* WCA_ACCENT_POLICY */, &accent, sizeof accent};
    set(reinterpret_cast<HWND>(window->winId()), &data);
    return enable;
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
