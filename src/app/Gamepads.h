#pragma once

#include "core/Types.h"

#include <QObject>
#include <QTimer>

#include <map>
#include <vector>

struct SDL_Gamepad;

// Game controllers through SDL3 (hotplug, state, rumble), polled on the UI thread.
class Gamepads : public QObject {
    Q_OBJECT

public:
    explicit Gamepads(QObject* parent = nullptr);
    ~Gamepads() override;

    std::vector<rdc::GamepadState> states() const;

    // Until the next call; 0, 0 stops it
    void rumble(int index, uint16_t lowFrequency, uint16_t highFrequency);
    void rumbleTriggers(int index, uint16_t left, uint16_t right);

signals:
    void changed(const rdc::GamepadState& state); // a controller was plugged in, moved, or unplugged (present = false)

private:
    struct Pad {
        SDL_Gamepad* gamepad = nullptr;
        rdc::GamepadState state;
    };
    void poll();
    void add(uint32_t id);
    void remove(uint32_t id);
    bool update(Pad& pad);
    SDL_Gamepad* byIndex(int index) const;

    std::map<uint32_t, Pad> pads_;
    QTimer timer_;
    bool ok_ = false;
};
