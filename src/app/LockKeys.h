#pragma once

#include <cstdint>
#include <optional>

// rdc::LockKey bits of this computer; nullopt if unknown.
std::optional<uint8_t> localLockKeys();
