#pragma once

#include "core/Connection.h"

#include <QByteArray>
#include <QString>

#include <vector>

// Other apps' saved connections, without passwords (theirs are encrypted or in their keychains)
std::vector<rdc::Connection> importRemmina(const QByteArray& file);                     // one .remmina file
std::vector<rdc::Connection> importPutty(const QString& name, const QByteArray& file);  // a ~/.putty/sessions file, its name as stored
std::vector<rdc::Connection> importMobaXterm(const QByteArray& file);                   // .mxtsessions or MobaXterm.ini
std::vector<rdc::Connection> importMremoteng(const QByteArray& xml, QString* error);    // confCons.xml, not fully encrypted
