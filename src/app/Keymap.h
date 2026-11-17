#pragma once

#include <cstdint>

class QKeyEvent;

// XT (set 1) scancode, 0x100 = E0 key; 0 if unknown
uint32_t xtScancode(const QKeyEvent* event);

// X11 keysym as typed, for VNC; 0 if unknown
uint32_t keysym(const QKeyEvent* event);

// Local layout as an RDP KLID, 0 if unknown
uint32_t detectKeyboardLayout();
