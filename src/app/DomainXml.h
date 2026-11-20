#pragma once

#include "app/VmSettings.h"
#include "core/Connection.h"

#include <QString>

#include <optional>

// A live libvirt domain as a SPICE/VNC connection; none without a TCP display
std::optional<rdc::Connection> connectionFromDomainXml(const QString& xml);

// Its persistent definition's processors, memory and first CD/DVD drive (host fields left empty)
VmSettings domainSettings(const QString& xml);
// The definition with those that differ from it changed; empty if it can't be read
QString withSettings(const QString& xml, const VmSettings& settings);
// Its first CD/DVD drive's <disk> holding this disc (none: empty), for virDomainUpdateDeviceFlags
QString cdromXml(const QString& xml, const QString& iso);
