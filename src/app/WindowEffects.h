#pragma once

class QRegion;
class QWidget;

// Compositor blur behind a translucent window: KWin protocol, NSVisualEffectView on macOS.
// Windows: see-through only for now.
namespace windowEffects {

// Ask before the window is first shown: translucency is requested at creation.
// On Wayland this is true and setBlurBehind() tells.
bool translucencyPossible();

// Whether the compositor can blur now (Wayland: known shortly after start)
bool blurAvailable();

// Turns blur on or off; true if on. `region`: window coords, KDE only
// (Windows/macOS blur the whole window)
bool setBlurBehind(QWidget* window, bool enable, const QRegion& region);

} // namespace windowEffects
