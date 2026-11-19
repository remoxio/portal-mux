#pragma once

#include <QFont>
#include <QFontDatabase>

// Where Qt's look differs from the platform's own apps
namespace look {

// Windows: WinUI's fonts; elsewhere Qt's
QFont systemFont(QFontDatabase::SystemFont kind);
// Windows: WinUI's metrics and fonts; macOS: AppKit's (Look_mac.mm)
void apply();

} // namespace look
