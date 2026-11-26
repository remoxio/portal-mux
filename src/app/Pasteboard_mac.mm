#include "app/Pasteboard.h"

#include <QFileInfo>

#import <AppKit/AppKit.h>

long pasteboardChangeCount()
{
    return long([NSPasteboard generalPasteboard].changeCount);
}

QStringList pasteboardFiles()
{
    QStringList files;
    @autoreleasepool {
        NSArray<NSURL*>* urls = [[NSPasteboard generalPasteboard] readObjectsForClasses:@[NSURL.class]
                                                                               options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
        for (NSURL* url in urls)
            if (NSURL* path = url.filePathURL; path && QFileInfo::exists(QString::fromNSString(path.path))) // the Finder's are file reference URLs
                files << QString::fromNSString(path.path);
    }
    return files;
}
