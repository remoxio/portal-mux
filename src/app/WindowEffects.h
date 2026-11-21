#pragma once

class QRegion;
class QWidget;

// Compositor blur behind a translucent window: KWin protocol, NSVisualEffectView on macOS.
namespace windowEffects {

// Before the window exists (macOS: before menuBar()); on Wayland setBlurBehind() tells
bool translucencyPossible();

// Whether the compositor can blur now (Wayland: known shortly after start)
bool blurAvailable();

// true if on; `region` in window coords (macOS: its bounding rect, Windows: the window)
bool setBlurBehind(QWidget* window, bool enable, const QRegion& region);

} // namespace windowEffects
