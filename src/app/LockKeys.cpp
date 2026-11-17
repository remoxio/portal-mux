#include "app/LockKeys.h"

#include "core/Types.h"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <filesystem>
#include <fstream>
#include <string>
#endif

std::optional<uint8_t> localLockKeys()
{
#if defined(_WIN32)
    uint8_t locks = 0;
    if (GetKeyState(VK_NUMLOCK) & 1) locks |= rdc::NumLock;
    if (GetKeyState(VK_CAPITAL) & 1) locks |= rdc::CapsLock;
    if (GetKeyState(VK_SCROLL) & 1) locks |= rdc::ScrollLock;
    return locks;
#elif defined(__linux__)
    // keyboard LEDs: set by the compositor or X server alike
    std::error_code ec;
    uint8_t locks = 0;
    bool found = false;
    for (const auto& led : std::filesystem::directory_iterator("/sys/class/leds", ec)) {
        const std::string name = led.path().filename().string();
        const auto is = [&](const char* suffix) { return name.ends_with(suffix); };
        const uint8_t bit = is("::numlock") ? rdc::NumLock : is("::capslock") ? rdc::CapsLock : is("::scrolllock") ? rdc::ScrollLock : 0;
        if (!bit) continue;
        found = found || bit == rdc::NumLock;
        int brightness = 0;
        if (std::ifstream(led.path() / "brightness") >> brightness; brightness > 0) locks |= bit;
    }
    if (!found) return std::nullopt;
    return locks;
#else
    return std::nullopt; // shortcut: macOS has no Num Lock; read Caps Lock via CGEventSource if needed
#endif
}
