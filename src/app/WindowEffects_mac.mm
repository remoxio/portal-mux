#include "app/WindowEffects.h"

#include <QRegion>
#include <QWidget>

#import <AppKit/AppKit.h>

// shortcut: written without a Mac to test on; verify on macOS before relying on it.

bool windowEffects::translucencyPossible() { return true; }

bool windowEffects::blurAvailable() { return true; }

bool windowEffects::setBlurBehind(QWidget* window, bool enable, const QRegion&)
{
    NSView* view = reinterpret_cast<NSView*>(window->winId());
    NSView* frame = view.superview;
    if (!frame) return false;
    NSString* const tag = @"portal-mux.blur";
    for (NSView* v in frame.subviews) {
        if (![v.identifier isEqualToString:tag]) continue;
        v.hidden = !enable; // added already; it follows the window size by itself
        return enable;
    }
    if (!enable) return false;
    NSVisualEffectView* effect = [[NSVisualEffectView alloc] initWithFrame:view.frame];
    effect.identifier = tag;
    effect.material = NSVisualEffectMaterialSidebar;
    effect.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    effect.state = NSVisualEffectStateFollowsWindowActiveState;
    effect.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [frame addSubview:effect positioned:NSWindowBelow relativeTo:view]; // below Qt's view: shows where it is translucent
    [effect release];
    view.window.opaque = NO;
    view.window.backgroundColor = NSColor.clearColor;
    return true;
}
