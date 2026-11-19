#pragma once

#include <QtGlobal>

class QRegion;
class QWidget;

// Compositor blur behind a translucent window: KWin protocol, NSVisualEffectView on macOS.
namespace windowEffects {

// Ask before the window is created (macOS: before menuBar()): translucency is requested at creation.
// On Wayland this is true and setBlurBehind() tells.
bool translucencyPossible();

// Whether the compositor can blur now (Wayland: known shortly after start)
bool blurAvailable();

// Turns blur on or off; true if on. `region`: window coords; macOS blurs its bounding rect,
// Windows the whole window
bool setBlurBehind(QWidget* window, bool enable, const QRegion& region);

#ifdef Q_OS_MACOS
// The title bar as an empty 52 pt toolbar row, the traffic lights centred in it, no title; the window draws the rest
// (Qt::ExpandedClientAreaHint)
void unifiedTitleBar(QWidget* window);
// Where the traffic lights end, in window coords
int titleBarButtonsEnd(QWidget* window);
// The title bar's height over the content; 0 in full screen
int titleBarHeight(QWidget* window);
// What a double-click on the title bar does: zoom or minimise, as the user chose
void titleBarDoubleClicked(QWidget* window);
#endif

} // namespace windowEffects
