#include "app/WindowEffects.h"

#include <QRegion>
#include <QWidget>

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

// only the blur, as GPUI's: the sidebar paints its own tint
@interface PortalMuxBlurView : NSVisualEffectView
@end

static void stripTint(CALayer* layer)
{
    layer.backgroundColor = nil;
    if ([layer.className isEqualToString:@"CAChameleonLayer"]) {
        layer.hidden = YES;
        return;
    }
    NSIndexSet* saturate = [layer.filters indexesOfObjectsPassingTest:^BOOL(id filter, NSUInteger, BOOL*) {
        return [[filter description] containsString:@"Saturat"];
    }];
    if (saturate.count) {
        NSMutableArray* filters = [[layer.filters mutableCopy] autorelease];
        [filters removeObjectsAtIndexes:saturate];
        layer.filters = filters;
    }
    for (CALayer* sublayer in layer.sublayers) stripTint(sublayer);
}

@implementation PortalMuxBlurView
- (void)updateLayer
{
    [super updateLayer];
    stripTint(self.layer);
}
@end

bool windowEffects::translucencyPossible() { return true; }

bool windowEffects::blurAvailable() { return true; }

bool windowEffects::setBlurBehind(QWidget* window, bool enable, const QRegion& region)
{
    NSView* view = reinterpret_cast<NSView*>(window->winId());
    NSView* frame = view.superview;
    if (!frame) return false;
    // Qt leaves a framed window's background to AppKit; clearColor would break the shadow
    view.window.backgroundColor = [NSColor colorWithSRGBRed:0 green:0 blue:0 alpha:0.0001];
    PortalMuxBlurView* blur = nil;
    for (NSView* v in frame.subviews)
        if ([v isKindOfClass:PortalMuxBlurView.class]) blur = static_cast<PortalMuxBlurView*>(v);
    if (!enable) {
        [blur removeFromSuperview];
        return false;
    }
    if (!blur) {
        blur = [[[PortalMuxBlurView alloc] initWithFrame:NSZeroRect] autorelease];
        blur.material = NSVisualEffectMaterialUnderWindowBackground; // Sidebar: no blur in inactive windows
        blur.blendingMode = NSVisualEffectBlendingModeBehindWindow;
        blur.state = NSVisualEffectStateActive;
        blur.autoresizingMask = NSViewHeightSizable;
        [frame addSubview:blur positioned:NSWindowBelow relativeTo:view];
    }
    const QRect r = region.boundingRect(); // from the content's top
    blur.frame = NSMakeRect(NSMinX(view.frame) + r.x(), NSMaxY(view.frame) - r.y() - r.height(), r.width(), r.height());
    return true;
}
