#pragma once

#include "core/Connection.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rdc {

// One table per protocol drives the dialog, the JSON file and the backends.
enum class PropertyKind { Text, Secret, Int, Bool, Choice, File, Directory };

struct Choice {
    std::string value, label;
};

struct PropertyDef {
    std::string key, label, tab;
    PropertyKind kind;
    std::string defaultValue;
    std::vector<Choice> choices = {};
    int min = 0, max = 0;
    std::string tip = {};
};

const std::vector<PropertyDef>& properties(Protocol protocol);
const PropertyDef* findProperty(Protocol protocol, std::string_view key);

struct KeyboardLayout {
    const char* xkb;
    const char* name;
    uint32_t klid;    // Windows layout id
};
const std::vector<KeyboardLayout>& keyboardLayouts();

} // namespace rdc
