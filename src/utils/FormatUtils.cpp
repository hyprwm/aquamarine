#include "FormatUtils.hpp"
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <cstdlib>

std::string fourccToName(uint32_t drmFormat) {
    auto        fmt  = drmGetFormatName(drmFormat);
    std::string name = fmt ? fmt : "unknown";
    free(fmt);
    return name;
}

std::string drmModifierToName(uint64_t drmModifier) {
    auto        n    = drmGetFormatModifierName(drmModifier);
    std::string name = n;
    free(n); // NOLINT(cppcoreguidelines-no-malloc,-warnings-as-errors)
    return name;
}
