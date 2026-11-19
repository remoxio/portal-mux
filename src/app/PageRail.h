#pragma once

class QListWidget;
class QWidget;

// A list as spark-commander's settings rail: a row per item with its icon. macOS: on a rounded panel, the selection a
// row of the accent; Windows: unframed, a faint selection with the accent's pill; elsewhere a panel, the selection the
// accent. Returns `list` in its panel.
QWidget* pageRail(QListWidget* list);
