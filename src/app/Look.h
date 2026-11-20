#pragma once

#include <QFont>
#include <QFontDatabase>

class QPainter;
class QPalette;
class QRectF;

// Where Qt's look differs from the platform's own apps
namespace look {

// Windows: WinUI's fonts; elsewhere Qt's
QFont systemFont(QFontDatabase::SystemFont kind);
// Windows: WinUI's metrics and fonts; macOS: AppKit's (Look_mac.mm)
void apply();
// a selected row as Dolphin's: the accent faint inside and a line of it around; the text keeps its colour
void paintSelection(QPainter* p, const QRectF& rect, const QPalette& palette, qreal radius);

} // namespace look
