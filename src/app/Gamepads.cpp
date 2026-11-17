#include "app/Gamepads.h"

#include <QDebug>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <set>

using rdc::GamepadState;

namespace {

constexpr std::pair<SDL_GamepadButton, uint32_t> kButtons[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, GamepadState::South},
    {SDL_GAMEPAD_BUTTON_EAST, GamepadState::East},
    {SDL_GAMEPAD_BUTTON_WEST, GamepadState::West},
    {SDL_GAMEPAD_BUTTON_NORTH, GamepadState::North},
    {SDL_GAMEPAD_BUTTON_BACK, GamepadState::Back},
    {SDL_GAMEPAD_BUTTON_GUIDE, GamepadState::Guide},
    {SDL_GAMEPAD_BUTTON_START, GamepadState::Start},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, GamepadState::LeftStick},
    {SDL_GAMEPAD_BUTTON_RIGHT_STICK, GamepadState::RightStick},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, GamepadState::LeftShoulder},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, GamepadState::RightShoulder},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, GamepadState::Up},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, GamepadState::Down},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, GamepadState::Left},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, GamepadState::Right},
    {SDL_GAMEPAD_BUTTON_MISC1, GamepadState::Misc},
    {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1, GamepadState::Paddle1}, // Xbox Elite P1..P4
    {SDL_GAMEPAD_BUTTON_LEFT_PADDLE1, GamepadState::Paddle2},
    {SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2, GamepadState::Paddle3},
    {SDL_GAMEPAD_BUTTON_LEFT_PADDLE2, GamepadState::Paddle4},
    {SDL_GAMEPAD_BUTTON_TOUCHPAD, GamepadState::Touchpad},
};

int16_t up(int16_t v) { return int16_t(-std::max<int>(v, -32767)); } // SDL's Y grows downwards
uint8_t trigger(int16_t v) { return uint8_t(std::max<int>(v, 0) * 255 / 32767); }

GamepadState::Type typeOf(SDL_Gamepad* g)
{
    switch (SDL_GetGamepadType(g)) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE: return GamepadState::Type::Xbox;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
    case SDL_GAMEPAD_TYPE_PS5: return GamepadState::Type::PlayStation;
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
    case SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR: return GamepadState::Type::Nintendo;
    default: return GamepadState::Type::Unknown;
    }
}

} // namespace

Gamepads::Gamepads(QObject* parent) : QObject(parent)
{
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");                // Ctrl+C stays Qt's
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1"); // no SDL window; RemoteView decides who gets input
    ok_ = SDL_Init(SDL_INIT_GAMEPAD);
    if (!ok_) {
        qWarning("Game controllers are unavailable: %s", SDL_GetError());
        return;
    }
    connect(&timer_, &QTimer::timeout, this, &Gamepads::poll);
    timer_.start(250);
    poll();
}

Gamepads::~Gamepads()
{
    if (!ok_) return;
    for (auto& [id, pad] : pads_) SDL_CloseGamepad(pad.gamepad);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    SDL_Quit();
}

std::vector<GamepadState> Gamepads::states() const
{
    std::vector<GamepadState> out;
    for (const auto& [id, pad] : pads_) out.push_back(pad.state);
    return out;
}

void Gamepads::poll()
{
    std::set<uint32_t> moved;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_GAMEPAD_ADDED: add(e.gdevice.which); break;
        case SDL_EVENT_GAMEPAD_REMOVED: remove(e.gdevice.which); break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION: moved.insert(e.gaxis.which); break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
        case SDL_EVENT_GAMEPAD_BUTTON_UP: moved.insert(e.gbutton.which); break;
        default: break;
        }
    }
    // shortcut: press+release within one 4 ms poll is lost, queue them if needed
    for (uint32_t id : moved)
        if (auto it = pads_.find(id); it != pads_.end() && update(it->second)) emit changed(it->second.state);
    timer_.setInterval(pads_.empty() ? 250 : 4); // hotplug checks when idle; ~250 Hz input with a controller
}

void Gamepads::add(uint32_t id)
{
    if (pads_.count(id)) return;
    int index = 0;
    while (index < 16 && byIndex(index)) ++index;
    if (index == 16) return; // GameStream's limit
    SDL_Gamepad* g = SDL_OpenGamepad(id);
    if (!g) return;
    SDL_SetGamepadPlayerIndex(g, index);

    GamepadState s;
    s.index = index;
    s.type = typeOf(g);
    for (auto [button, bit] : kButtons)
        if (SDL_GamepadHasButton(g, button)) s.supportedButtons |= bit;
    if (SDL_GamepadHasAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) && SDL_GamepadHasAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))
        s.capabilities |= GamepadState::AnalogTriggers;
    const SDL_PropertiesID props = SDL_GetGamepadProperties(g);
    if (SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, false)) s.capabilities |= GamepadState::Rumble;
    if (SDL_GetBooleanProperty(props, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, false))
        s.capabilities |= GamepadState::TriggerRumble;

    Pad& pad = pads_[id];
    pad.gamepad = g;
    pad.state = s;
    if (qEnvironmentVariableIsSet("PORTAL_MUX_DEBUG"))
        std::fprintf(stderr, "gamepad %d: %s, type %d, capabilities 0x%x\n", index, SDL_GetGamepadName(g), int(s.type),
                     s.capabilities);
    update(pad);
    emit changed(pad.state); // also when idle: a new index announces the controller
}

void Gamepads::remove(uint32_t id)
{
    auto it = pads_.find(id);
    if (it == pads_.end()) return;
    SDL_CloseGamepad(it->second.gamepad);
    GamepadState gone;
    gone.index = it->second.state.index;
    gone.present = false;
    pads_.erase(it);
    emit changed(gone);
}

bool Gamepads::update(Pad& pad)
{
    SDL_Gamepad* g = pad.gamepad;
    GamepadState s = pad.state;
    s.buttons = 0;
    for (auto [button, bit] : kButtons)
        if (SDL_GetGamepadButton(g, button)) s.buttons |= bit;
    s.leftX = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX);
    s.leftY = up(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY));
    s.rightX = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTX);
    s.rightY = up(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHTY));
    s.leftTrigger = trigger(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFT_TRIGGER));
    s.rightTrigger = trigger(SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER));
    const bool same = s.buttons == pad.state.buttons && s.leftX == pad.state.leftX && s.leftY == pad.state.leftY &&
                      s.rightX == pad.state.rightX && s.rightY == pad.state.rightY &&
                      s.leftTrigger == pad.state.leftTrigger && s.rightTrigger == pad.state.rightTrigger;
    pad.state = s;
    return !same;
}

SDL_Gamepad* Gamepads::byIndex(int index) const
{
    for (const auto& [id, pad] : pads_)
        if (pad.state.index == index) return pad.gamepad;
    return nullptr;
}

void Gamepads::rumble(int index, uint16_t lowFrequency, uint16_t highFrequency)
{
    // shortcut: rumble over 30 s stops early (SDL wants a duration), as in moonlight-qt
    if (SDL_Gamepad* g = byIndex(index)) SDL_RumbleGamepad(g, lowFrequency, highFrequency, 30000);
}

void Gamepads::rumbleTriggers(int index, uint16_t left, uint16_t right)
{
    if (SDL_Gamepad* g = byIndex(index)) SDL_RumbleGamepadTriggers(g, left, right, 30000);
}
