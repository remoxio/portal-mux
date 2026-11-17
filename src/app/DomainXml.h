#pragma once

#include "core/Connection.h"

#include <QString>

#include <optional>

// A live libvirt domain as a SPICE/VNC connection; none without a TCP display
std::optional<rdc::Connection> connectionFromDomainXml(const QString& xml);
