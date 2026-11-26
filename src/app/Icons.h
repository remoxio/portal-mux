#pragma once

#include <QColor>
#include <QIcon>
#include <QPalette>

// Sidebar icons (third_party/icons)
namespace icons {

QIcon material(const char* name);
QIcon brand(const char* name);
QIcon lucide(const char* name, QColor color = {}); // e.g. "play"; no colour: the palette's
QIcon lucide(const char* name, QPalette::ColorRole role); // that colour of the painted widget's palette, e.g. a dimmed one

} // namespace icons
