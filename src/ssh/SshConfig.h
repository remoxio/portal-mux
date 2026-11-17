#pragma once

#include "core/Connection.h"

#include <string>
#include <vector>

namespace rdc {

// Hosts of ~/.ssh/config as SSH connections, resolved the OpenSSH way (first value wins,
// wildcard Host blocks contribute, Include followed, Match skipped).
inline constexpr char kSshConfigIdPrefix[] = "ssh-config:";
std::vector<Connection> readSshConfig(const std::string& path);

} // namespace rdc
