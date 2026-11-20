#pragma once

#include <algorithm>
#include <cstdint>
#include <string>

namespace rdc {

struct Rect {
    int x = 0, y = 0, width = 0, height = 0;

    bool empty() const { return width <= 0 || height <= 0; }

    Rect united(const Rect& o) const
    {
        if (empty()) return o;
        if (o.empty()) return *this;
        int l = std::min(x, o.x), t = std::min(y, o.y);
        int r = std::max(x + width, o.x + o.width), b = std::max(y + height, o.y + o.height);
        return {l, t, r - l, b - t};
    }

    Rect intersected(const Rect& o) const
    {
        int l = std::max(x, o.x), t = std::max(y, o.y);
        int r = std::min(x + width, o.x + o.width), b = std::min(y + height, o.y + o.height);
        return r > l && b > t ? Rect{l, t, r - l, b - t} : Rect{};
    }

    bool operator==(const Rect&) const = default;
};

enum class SessionState { Disconnected, Connecting, Connected, Error };

enum LockKey : uint8_t { ScrollLock = 1, NumLock = 2, CapsLock = 4 }; // RDP's and SPICE's bits

// each protocol uses one: scancode, keysym or text
struct KeyEvent {
    uint32_t scancode = 0;
    uint32_t keysym = 0;
    std::string text;      // UTF-8 typed with layout and modifiers (presses only)
    bool pressed = false;
};

struct PointerEvent {
    enum Button : uint8_t { Left = 1, Middle = 2, Right = 4 };
    int x = 0, y = 0;
    uint8_t buttons = 0;
    int wheelDelta = 0;  // positive = away; 120 per notch
    int hwheelDelta = 0; // positive = right; 120 per notch
};

// XInput conventions; stick Y positive = up
struct GamepadState {
    // South = A on Xbox; values match moonlight-common-c
    enum Button : uint32_t {
        Up = 0x0001, Down = 0x0002, Left = 0x0004, Right = 0x0008, Start = 0x0010, Back = 0x0020,
        LeftStick = 0x0040, RightStick = 0x0080, LeftShoulder = 0x0100, RightShoulder = 0x0200, Guide = 0x0400,
        South = 0x1000, East = 0x2000, West = 0x4000, North = 0x8000,
        Paddle1 = 0x010000, Paddle2 = 0x020000, Paddle3 = 0x040000, Paddle4 = 0x080000,
        Touchpad = 0x100000, Misc = 0x200000,
    };
    enum class Type : uint8_t { Unknown, Xbox, PlayStation, Nintendo };
    enum Capability : uint16_t { AnalogTriggers = 0x01, Rumble = 0x02, TriggerRumble = 0x04 };

    int index = 0;
    bool present = true;  // false = unplugged (others zero)
    Type type = Type::Unknown;
    uint32_t supportedButtons = 0;
    uint16_t capabilities = 0;

    uint32_t buttons = 0;
    int16_t leftX = 0, leftY = 0, rightX = 0, rightY = 0;
    uint8_t leftTrigger = 0, rightTrigger = 0;
};

} // namespace rdc
