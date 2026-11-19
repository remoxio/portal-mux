#pragma once

#include "core/Connection.h"

#include <QByteArray>

#include <vector>

// KRDC bookmarks, history and krdcrc settings as connections; no KWallet passwords
std::vector<rdc::Connection> importKrdc(const QByteArray& bookmarksXml, const QByteArray& krdcrc);
