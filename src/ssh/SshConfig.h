#pragma once

#include "core/Connection.h"

#include <string>
#include <vector>

namespace rdc {

// ~/.ssh/config's hosts as connections, resolved as OpenSSH does (Match skipped)
inline constexpr char kSshConfigIdPrefix[] = "ssh-config:";
std::vector<Connection> readSshConfig(const std::string& path);

} // namespace rdc
