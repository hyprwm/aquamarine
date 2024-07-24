#pragma once

#include <string>
#include <cstdint>

struct SGLFormat {
    uint32_t drmFormat = 0;
    uint64_t modifier  = 0;
    bool     external  = false;
};

std::string fourccToName(uint32_t drmFormat);