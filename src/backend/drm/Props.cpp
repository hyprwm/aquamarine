#include <aquamarine/backend/DRM.hpp>

extern "C" {
#include <libseat.h>
#include <libudev.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
}

#include <cstring>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
#define SP CSharedPointer

struct prop_info {
    const char* name;
    size_t      index;
};

static const struct prop_info connector_info[] = {
#define INDEX(name) (offsetof(SDRMConnector::UDRMConnectorProps, name) / sizeof(uint32_t))
    {.name = "CRTC_ID", .index = INDEX(crtc_id)},
    {.name = "Colorspace", .index = INDEX(Colorspace)},
    {.name = "DPMS", .index = INDEX(dpms)},
    {.name = "EDID", .index = INDEX(edid)},
    {.name = "HDR_OUTPUT_METADATA", .index = INDEX(hdr_output_metadata)},
    {.name = "PATH", .index = INDEX(path)},
    {.name = "content type", .index = INDEX(content_type)},
    {.name = "link-status", .index = INDEX(link_status)},
    {.name = "max bpc", .index = INDEX(max_bpc)},
    {.name = "non-desktop", .index = INDEX(non_desktop)},
    {.name = "panel orientation", .index = INDEX(panel_orientation)},
    {.name = "subconnector", .index = INDEX(subconnector)},
    {.name = "vrr_capable", .index = INDEX(vrr_capable)},
#undef INDEX
};

static const struct prop_info colorspace_info[] = {
#define INDEX(name) (offsetof(SDRMConnector::UDRMConnectorColorspace, name) / sizeof(uint32_t))
    {.name = "BT2020_RGB", .index = INDEX(BT2020_RGB)},
    {.name = "BT2020_YCC", .index = INDEX(BT2020_YCC)},
    {.name = "Default", .index = INDEX(Default)},
#undef INDEX
};

static const struct prop_info crtc_info[] = {
#define INDEX(name) (offsetof(SDRMCRTC::UDRMCRTCProps, name) / sizeof(uint32_t))
    {.name = "ACTIVE", .index = INDEX(active)},           {.name = "CTM", .index = INDEX(ctm)},
    {.name = "DEGAMMA_LUT", .index = INDEX(degamma_lut)}, {.name = "DEGAMMA_LUT_SIZE", .index = INDEX(degamma_lut_size)},
    {.name = "GAMMA_LUT", .index = INDEX(gamma_lut)},     {.name = "GAMMA_LUT_SIZE", .index = INDEX(gamma_lut_size)},
    {.name = "MODE_ID", .index = INDEX(mode_id)},         {.name = "OUT_FENCE_PTR", .index = INDEX(out_fence_ptr)},
    {.name = "VRR_ENABLED", .index = INDEX(vrr_enabled)},
#undef INDEX
};

static const struct prop_info plane_info[] = {
#define INDEX(name) (offsetof(SDRMPlane::UDRMPlaneProps, name) / sizeof(uint32_t))
    {.name = "CRTC_H", .index = INDEX(crtc_h)},
    {.name = "CRTC_ID", .index = INDEX(crtc_id)},
    {.name = "CRTC_W", .index = INDEX(crtc_w)},
    {.name = "CRTC_X", .index = INDEX(crtc_x)},
    {.name = "CRTC_Y", .index = INDEX(crtc_y)},
    {.name = "FB_DAMAGE_CLIPS", .index = INDEX(fb_damage_clips)},
    {.name = "FB_ID", .index = INDEX(fb_id)},
    {.name = "HOTSPOT_X", .index = INDEX(hotspot_x)},
    {.name = "HOTSPOT_Y", .index = INDEX(hotspot_y)},
    {.name = "IN_FENCE_FD", .index = INDEX(in_fence_fd)},
    {.name = "IN_FORMATS", .index = INDEX(in_formats)},
    {.name = "SRC_H", .index = INDEX(src_h)},
    {.name = "SRC_W", .index = INDEX(src_w)},
    {.name = "SRC_X", .index = INDEX(src_x)},
    {.name = "SRC_Y", .index = INDEX(src_y)},
    {.name = "rotation", .index = INDEX(rotation)},
    {.name = "type", .index = INDEX(type)},
#undef INDEX
};

namespace Aquamarine {

    static int comparePropInfo(const void* arg1, const void* arg2) {
        const char*      key  = (const char*)arg1;
        const prop_info* elem = (prop_info*)arg2;

        return strcmp(key, elem->name);
    }

    static bool scanProperties(int fd, uint32_t id, uint32_t type, uint32_t* result, const prop_info* info, size_t info_len) {
        drmModeObjectProperties* props = drmModeObjectGetProperties(fd, id, type);
        if (!props)
            return false;

        for (uint32_t i = 0; i < props->count_props; ++i) {
            drmModePropertyRes* prop = drmModeGetProperty(fd, props->props[i]);
            if (!prop)
                continue;

            const prop_info* p = (prop_info*)bsearch(prop->name, info, info_len, sizeof(info[0]), comparePropInfo);
            if (p)
                result[p->index] = prop->prop_id;

            drmModeFreeProperty(prop);
        }

        drmModeFreeObjectProperties(props);
        return true;
    }

    static bool scanPropertyEnum(int fd, uint32_t propertyId, uint32_t* result, const prop_info* info, size_t info_len) {
        drmModePropertyRes* prop = drmModeGetProperty(fd, propertyId);
        if (!prop)
            return false;

        for (int i = 0; i < prop->count_enums; ++i) {
            const prop_info* p = (prop_info*)bsearch(prop->enums[i].name, info, info_len, sizeof(info[0]), comparePropInfo);
            if (p)
                result[p->index] = prop->enums[i].value;
        }

        drmModeFreeProperty(prop);

        return true;
    }

    bool getDRMConnectorProps(int fd, uint32_t id, SDRMConnector::UDRMConnectorProps* out) {
        return scanProperties(fd, id, DRM_MODE_OBJECT_CONNECTOR, out->props, connector_info, sizeof(connector_info) / sizeof(connector_info[0]));
    }

    bool getDRMConnectorColorspace(int fd, uint32_t id, SDRMConnector::UDRMConnectorColorspace* out) {
        return scanPropertyEnum(fd, id, out->props, colorspace_info, sizeof(colorspace_info) / sizeof(colorspace_info[0]));
    }

    bool getDRMCRTCProps(int fd, uint32_t id, SDRMCRTC::UDRMCRTCProps* out) {
        return scanProperties(fd, id, DRM_MODE_OBJECT_CRTC, out->props, crtc_info, sizeof(crtc_info) / sizeof(crtc_info[0]));
    }

    bool getDRMPlaneProps(int fd, uint32_t id, SDRMPlane::UDRMPlaneProps* out) {
        return scanProperties(fd, id, DRM_MODE_OBJECT_PLANE, out->props, plane_info, sizeof(plane_info) / sizeof(plane_info[0]));
    }

    bool getDRMProp(int fd, uint32_t obj, uint32_t prop, uint64_t* ret) {
        drmModeObjectProperties* props = drmModeObjectGetProperties(fd, obj, DRM_MODE_OBJECT_ANY);
        if (!props)
            return false;

        bool found = false;

        for (uint32_t i = 0; i < props->count_props; ++i) {
            if (props->props[i] == prop) {
                *ret  = props->prop_values[i];
                found = true;
                break;
            }
        }

        drmModeFreeObjectProperties(props);
        return found;
    }

    void* getDRMPropBlob(int fd, uint32_t obj, uint32_t prop, size_t* ret_len) {
        uint64_t blob_id;
        if (!getDRMProp(fd, obj, prop, &blob_id))
            return nullptr;

        drmModePropertyBlobRes* blob = drmModeGetPropertyBlob(fd, blob_id);
        if (!blob)
            return nullptr;

        void* ptr = malloc(blob->length);
        if (!ptr) {
            drmModeFreePropertyBlob(blob);
            return nullptr;
        }

        memcpy(ptr, blob->data, blob->length);
        *ret_len = blob->length;

        drmModeFreePropertyBlob(blob);
        return ptr;
    }

    char* getDRMPropEnum(int fd, uint32_t obj, uint32_t prop_id) {
        uint64_t value;
        if (!getDRMProp(fd, obj, prop_id, &value))
            return nullptr;

        drmModePropertyRes* prop = drmModeGetProperty(fd, prop_id);
        if (!prop)
            return nullptr;

        char* str = nullptr;
        for (int i = 0; i < prop->count_enums; i++) {
            if (prop->enums[i].value == value) {
                str = strdup(prop->enums[i].name);
                break;
            }
        }

        drmModeFreeProperty(prop);

        return str;
    }

    bool introspectDRMPropRange(int fd, uint32_t prop_id, uint64_t* min, uint64_t* max) {
        drmModePropertyRes* prop = drmModeGetProperty(fd, prop_id);
        if (!prop)
            return false;

        if (drmModeGetPropertyType(prop) != DRM_MODE_PROP_RANGE) {
            drmModeFreeProperty(prop);
            return false;
        }

        if (prop->count_values != 2)
            abort();

        if (min != nullptr)
            *min = prop->values[0];
        if (max != nullptr)
            *max = prop->values[1];

        drmModeFreeProperty(prop);
        return true;
    }

};
