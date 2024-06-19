#include "FormatUtils.hpp"
#include <drm_fourcc.h>
#include <xf86drm.h>

std::string fourccToName(uint32_t drmFormat) {
    auto fmt = drmGetFormatName(drmFormat);
    return fmt ? fmt : "unknown";
}
