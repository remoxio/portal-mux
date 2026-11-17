#pragma once

#include "core/Connection.h"

#include <QByteArray>

#include <vector>

// KRDC bookmarks (XBEL) and per-host settings (krdcrc) as connections.
// Unbookmarked history goes to "KRDC history"; KWallet passwords aren't imported.
std::vector<rdc::Connection> importKrdc(const QByteArray& bookmarksXml, const QByteArray& krdcrc);
