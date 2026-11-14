#pragma once

#include <string>
#include <cstdint>
#include <drm_fourcc.h>

std::string fourccToName(uint32_t drmFormat);
std::string drmModifierToName(uint64_t drmModifier);

// Alpha variant of an opaque 8 bpc format, or DRM_FORMAT_INVALID. Used when a scanout plane offers only the alpha variant.
inline uint32_t opaqueToAlphaFormat(uint32_t fmt) {
    if (fmt == DRM_FORMAT_XRGB8888)
        return DRM_FORMAT_ARGB8888;
    if (fmt == DRM_FORMAT_XBGR8888)
        return DRM_FORMAT_ABGR8888;
    return DRM_FORMAT_INVALID;
}
