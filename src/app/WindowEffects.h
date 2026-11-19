#pragma once

#include <QtGlobal>

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

#ifdef Q_OS_MACOS
// An empty 52 pt toolbar row with the traffic lights, no title
void unifiedTitleBar(QWidget* window);
// Where the traffic lights end, in window coords
int titleBarButtonsEnd(QWidget* window);
// The title bar's height over the content; 0 in full screen
int titleBarHeight(QWidget* window);
// What a double-click on the title bar does: zoom or minimise, as the user chose
void titleBarDoubleClicked(QWidget* window);
#endif

} // namespace windowEffects
