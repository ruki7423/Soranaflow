#pragma once

#include <string>
#include <cstdint>
#include <vector>

struct AudioDevice {
    uint32_t    deviceId;
    std::string name;
    bool        isDefault;
};
