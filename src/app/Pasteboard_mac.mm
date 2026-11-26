#include "app/Pasteboard.h"

#import <AppKit/AppKit.h>

long pasteboardChangeCount()
{
    return long([NSPasteboard generalPasteboard].changeCount);
}
