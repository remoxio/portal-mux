#pragma once

#include <QList>
#include <QString>

class QSettings;
class QWidget;

// commands saved once, typed into a terminal from its toolbar
namespace snippets {

struct Snippet {
    QString name, command;
    bool run = true; // Enter after it
    QString label() const; // the name, else the command's first line
};

QList<Snippet> load(QSettings* settings);
void edit(QWidget* parent, QSettings* settings);

} // namespace snippets
