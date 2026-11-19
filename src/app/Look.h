#pragma once

#include <QFont>
#include <QFontDatabase>

// Where Qt's look differs from the platform's own apps
namespace look {

// Windows: WinUI's fonts; elsewhere Qt's
QFont systemFont(QFontDatabase::SystemFont kind);
// Windows: WinUI's sizes, spacing and fonts; macOS: AppKit's tab view, field sizes and spacing
void apply();

} // namespace look
