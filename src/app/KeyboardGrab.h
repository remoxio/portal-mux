#pragma once

class QWidget;

// Desktop shortcuts reach `widget`: X11 grab, Wayland shortcuts-inhibit, else app-only
namespace keyboardGrab {

void set(QWidget* widget, bool on);

} // namespace keyboardGrab
