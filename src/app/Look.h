#pragma once

#include <QFont>
#include <QFontDatabase>

class QFormLayout;
class QLabel;
class QMenu;
class QPainter;
class QPalette;
class QRectF;
class QWidget;

// Where Qt's look differs from the platform's own apps
namespace look {

// Windows: WinUI's fonts; elsewhere Qt's
QFont systemFont(QFontDatabase::SystemFont kind);
// Windows: WinUI's metrics and fonts; macOS: AppKit's (Look_mac.mm)
void apply();
// a selected row as Dolphin's: the accent faint inside and a line of it around; the text keeps its colour
void paintSelection(QPainter* p, const QRectF& rect, const QPalette& palette, qreal radius);
// a form row with a hint under its field, small and dimmed; returns the hint
QLabel* hintedRow(QFormLayout* form, const QString& label, QWidget* field, const QString& hint);
#ifdef Q_OS_MACOS
// a dialog's title bar: the close button only, no line under it (Look_mac.mm)
void dialogTitleBar(QWidget* dialog);
// AppKit's Window and Help menus: tiling and the windows, the menu search
void systemMenus(QMenu* windows, QMenu* help);
#endif

} // namespace look
