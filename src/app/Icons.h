#pragma once

#include <QColor>
#include <QIcon>

// Sidebar icons (third_party/icons)
namespace icons {

QIcon material(const char* name);
QIcon brand(const char* name);
QIcon lucide(const char* name, QColor color = {}); // e.g. "play"; no colour: the palette's

} // namespace icons
