#include "FormatUtils.hpp"
#include <drm_fourcc.h>
#include <xf86drm.h>

std::string fourccToName(uint32_t drmFormat) {
    auto        fmt  = drmGetFormatName(drmFormat);
    std::string name = fmt ? fmt : "unknown";
    free(fmt);
    return name;
}
