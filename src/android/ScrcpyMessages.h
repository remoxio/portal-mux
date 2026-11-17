#pragma once

// scrcpy 5.0.1 control messages (app/src/control_msg.c)

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace rdc::scrcpy {

enum : uint8_t {
    InjectKeycode = 0, InjectText = 1, InjectTouch = 2, InjectScroll = 3, BackOrScreenOn = 4, SetClipboard = 9,
    SetDisplayPower = 10, UhidCreate = 12, UhidInput = 13,
};
enum : uint8_t { KeyDown = 0, KeyUp = 1 };
enum : uint8_t { TouchDown = 0, TouchUp = 1, TouchMove = 2, HoverMove = 7 };
enum : uint32_t { ButtonPrimary = 1 };
enum : uint32_t {
    MetaShift = 0x01 | 0x40, MetaAlt = 0x02 | 0x10, MetaCtrl = 0x1000 | 0x2000, MetaMeta = 0x10000 | 0x20000,
    MetaCapsLock = 0x100000,
};
enum : uint32_t { KeycodeHome = 3 };
constexpr uint64_t PointerIdMouse = UINT64_C(0xFFFFFFFFFFFFFFFF);
constexpr size_t InjectTextMax = 300;
constexpr size_t ClipboardMax = (1 << 18) - 14;

using Bytes = std::vector<uint8_t>;

inline void put16(Bytes& b, uint16_t v) { b.insert(b.end(), {uint8_t(v >> 8), uint8_t(v)}); }
inline void put32(Bytes& b, uint32_t v) { b.insert(b.end(), {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}); }
inline void put64(Bytes& b, uint64_t v)
{
    put32(b, uint32_t(v >> 32));
    put32(b, uint32_t(v));
}
inline void putPosition(Bytes& b, int x, int y, int width, int height)
{
    put32(b, uint32_t(x));
    put32(b, uint32_t(y));
    put16(b, uint16_t(width));
    put16(b, uint16_t(height));
}
inline void putString(Bytes& b, const std::string& utf8, size_t max)
{
    size_t len = std::min(utf8.size(), max);
    while (len > 0 && len < utf8.size() && (uint8_t(utf8[len]) & 0xC0) == 0x80) --len;
    put32(b, uint32_t(len));
    b.insert(b.end(), utf8.begin(), utf8.begin() + long(len));
}

inline Bytes keycode(uint8_t action, uint32_t code, uint32_t metaState)
{
    Bytes b{InjectKeycode, action};
    put32(b, code);
    put32(b, 0);
    put32(b, metaState);
    return b;
}

inline Bytes text(const std::string& utf8)
{
    Bytes b{InjectText};
    putString(b, utf8, InjectTextMax);
    return b;
}

inline Bytes touch(uint8_t action, int x, int y, int width, int height, bool pressed, uint32_t actionButton, uint32_t buttons)
{
    Bytes b{InjectTouch, action};
    put64(b, PointerIdMouse);
    putPosition(b, x, y, width, height);
    put16(b, pressed ? 0xFFFF : 0); // pressure, 16-bit fixed point
    put32(b, actionButton);
    put32(b, buttons);
    return b;
}

// + = up/left; sent as i16 fixed point of notches/16
inline Bytes scroll(int x, int y, int width, int height, float hNotches, float vNotches, uint32_t buttons)
{
    auto fixed = [](float notches) {
        const int32_t v = int32_t(std::clamp(notches / 16.0f, -1.0f, 1.0f) * 32768.0f);
        return uint16_t(int16_t(std::min(v, 0x7FFF)));
    };
    Bytes b{InjectScroll};
    putPosition(b, x, y, width, height);
    put16(b, fixed(hNotches));
    put16(b, fixed(vNotches));
    put32(b, buttons);
    return b;
}

inline Bytes backOrScreenOn(uint8_t action) { return {BackOrScreenOn, action}; }
inline Bytes displayPower(bool on) { return {SetDisplayPower, uint8_t(on)}; }

inline Bytes setClipboard(uint64_t sequence, const std::string& utf8)
{
    Bytes b{SetClipboard};
    put64(b, sequence);
    b.push_back(0);
    putString(b, utf8, ClipboardMax);
    return b;
}

// 0 = send the typed text instead
inline uint32_t androidKeycode(uint32_t keysym)
{
    if (keysym >= 'a' && keysym <= 'z') return 29 + (keysym - 'a');
    if (keysym >= 'A' && keysym <= 'Z') return 29 + (keysym - 'A');
    if (keysym >= 0xFFBE && keysym <= 0xFFC9) return 131 + (keysym - 0xFFBE);
    switch (keysym) {
    case ' ': return 62;
    case 0xFF0D: return 66;  // Return -> ENTER
    case 0xFF8D: return 160; // KP_Enter
    case 0xFF08: return 67;  // Return
    case 0xFFFF: return 112; // BackSpace
    case 0xFF09:
    case 0xFE20: return 61;  // Tab, ISO_Left_Tab
    case 0xFF1B: return 111;
    case 0xFF50: return 122; // Home
    case 0xFF57: return 123; // End
    case 0xFF51: return 21;  // Left
    case 0xFF52: return 19;  // Up
    case 0xFF53: return 22;  // Right
    case 0xFF54: return 20;  // Down
    case 0xFF55: return 92;  // Page_Up
    case 0xFF56: return 93;  // Page_Down
    case 0xFF63: return 124; // Insert
    case 0xFF67: return 82;  // Menu
    case 0xFFE1: return 59;  // Shift_L
    case 0xFFE2: return 60;  // Shift_R
    case 0xFFE3: return 113; // Control_L
    case 0xFFE4: return 114; // Control_R
    case 0xFFE9: return 57;  // Alt_L
    case 0xFFEA: return 58;  // Alt_R (not AltGr: it types text)
    case 0xFFEB: return 117; // Super_L
    case 0xFFEC: return 118; // Super_R
    default: return 0;
    }
}

inline uint32_t metaOf(uint32_t keysym)
{
    switch (keysym) {
    case 0xFFE1: case 0xFFE2: return MetaShift;
    case 0xFFE3: case 0xFFE4: return MetaCtrl;
    case 0xFFE9: case 0xFFEA: return MetaAlt;
    case 0xFFEB: case 0xFFEC: return MetaMeta;
    default: return 0;
    }
}

// HID keyboard (scrcpy --keyboard=uhid): the device applies its physical layout

constexpr uint16_t HidKeyboardId = 1;
constexpr int HidKeyboardKeys = 0x66; // modifiers travel as bits in byte 0
constexpr uint8_t HidKeyboardDescriptor[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,                         // Generic Desktop keyboard
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01, // modifiers
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x75, 0x08, 0x95, 0x01, 0x81, 0x01,                         // reserved byte
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x75, 0x01, 0x95, 0x05, 0x91, 0x02, // LEDs (output)
    0x75, 0x03, 0x95, 0x01, 0x91, 0x01,                         // LED padding
    0x05, 0x07, 0x19, 0x00, 0x29, HidKeyboardKeys - 1, 0x15, 0x00, 0x25, HidKeyboardKeys - 1, // 6 key slots
    0x75, 0x08, 0x95, 0x06, 0x81, 0x00,
    0xC0,
};
constexpr uint8_t HidLedCapsLock = 0x02;
constexpr uint8_t HidCapsLock = 0x39;

inline Bytes uhidCreate(uint16_t id, const std::string& name, const uint8_t* descriptor, uint16_t size)
{
    Bytes b{UhidCreate};
    put16(b, id);
    put16(b, 0);
    put16(b, 0);
    const size_t len = std::min<size_t>(name.size(), 127);
    b.push_back(uint8_t(len));
    b.insert(b.end(), name.begin(), name.begin() + long(len));
    put16(b, size);
    b.insert(b.end(), descriptor, descriptor + size);
    return b;
}

inline Bytes uhidInput(uint16_t id, const uint8_t* data, uint16_t size)
{
    Bytes b{UhidInput};
    put16(b, id);
    put16(b, size);
    b.insert(b.end(), data, data + size);
    return b;
}

// 0x100 = E0-extended; 0 = no such key
inline uint8_t hidUsage(uint32_t xt)
{
    static constexpr uint8_t base[0x59] = {
        // 0x00-0x0F: -, Esc, 1..0, -, =, Backspace, Tab
        0, 0x29, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x2A, 0x2B,
        // 0x10-0x1F: Q W E R T Y U I O P [ ] Enter LCtrl A S
        0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, 0x12, 0x13, 0x2F, 0x30, 0x28, 0xE0, 0x04, 0x16,
        // 0x20-0x2F: D F G H J K L ; ' ` LShift \ Z X C V
        0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x33, 0x34, 0x35, 0xE1, 0x31, 0x1D, 0x1B, 0x06, 0x19,
        // 0x30-0x3F: B N M , . / RShift KP* LAlt Space CapsLock F1..F5
        0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0xE5, 0x55, 0xE2, 0x2C, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E,
        // 0x40-0x4F: F6..F10 NumLock ScrollLock KP7 KP8 KP9 KP- KP4 KP5 KP6 KP+ KP1
        0x3F, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5F, 0x60, 0x61, 0x56, 0x5C, 0x5D, 0x5E, 0x57, 0x59,
        // 0x50-0x58: KP2 KP3 KP0 KP. - - ISO \ (the extra key next to Left Shift) F11 F12
        0x5A, 0x5B, 0x62, 0x63, 0, 0, 0x64, 0x44, 0x45,
    };
    if (xt < sizeof base) return base[xt];
    switch (xt) {
    case 0x11C: return 0x58; // KP Enter
    case 0x11D: return 0xE4; // Right Ctrl
    case 0x135: return 0x54; // KP /
    case 0x137: return 0x46; // Print Screen
    case 0x138: return 0xE6; // Right Alt / AltGr
    case 0x147: return 0x4A; // Home
    case 0x148: return 0x52; // Up
    case 0x149: return 0x4B; // Page Up
    case 0x14B: return 0x50; // Left
    case 0x14D: return 0x4F; // Right
    case 0x14F: return 0x4D; // End
    case 0x150: return 0x51; // Down
    case 0x151: return 0x4E; // Page Down
    case 0x152: return 0x49; // Insert
    case 0x153: return 0x4C; // Delete
    case 0x15B: return 0xE3; // Left GUI
    case 0x15C: return 0xE7; // Right GUI
    case 0x15D: return 0x65; // Application (menu)
    default: return 0;
    }
}

} // namespace rdc::scrcpy
