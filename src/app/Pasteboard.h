#pragma once

#include <QStringList>

// macOS: the general pasteboard's change count; tells copies made outside Qt (Pasteboard_mac.mm)
long pasteboardChangeCount();
// its files that exist, as paths: Qt doesn't see the Finder's
QStringList pasteboardFiles();
