#pragma once

class QWidget;

// Desktop shortcuts (Alt+Tab, Meta) reach `widget`
// X11: grab; Wayland: shortcuts-inhibit protocol; else app-only
namespace keyboardGrab {

void set(QWidget* widget, bool on);

} // namespace keyboardGrab
