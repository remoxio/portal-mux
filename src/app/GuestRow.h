#pragma once

#include <QColor>
#include <QString>

#include <vector>

class QPainter;
class QStyleOptionViewItem;
class QSize;

// A VM, container or node row: icon with its state, bold name, detail line, CPU graph
struct GuestRow {
    const char* icon = "device-desktop"; // Tabler
    bool active = false;                 // the icon in the text colour, else dimmed
    const char* glyph = nullptr;
    QColor glyphColor;
    QString name, detail;
    std::vector<float> cpu; // percent, the newest last
};

QSize guestRowSize(const QStyleOptionViewItem& option);
void paintGuestRow(QPainter* p, const QStyleOptionViewItem& option, const GuestRow& row); // option: initialized, panel drawn
