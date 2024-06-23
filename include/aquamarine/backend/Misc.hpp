#pragma once

#include <cstdint>
#include <vector>

namespace Aquamarine {
    struct SDRMFormat {
        uint32_t              drmFormat = 0; /* DRM_FORMAT_INVALID */
        std::vector<uint64_t> modifiers;
    };
};
