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
    {"CRTC_ID", INDEX(crtc_id)},
    {"DPMS", INDEX(dpms)},
    {"EDID", INDEX(edid)},
    {"PATH", INDEX(path)},
    {"content type", INDEX(content_type)},
    {"link-status", INDEX(link_status)},
    {"max bpc", INDEX(max_bpc)},
    {"non-desktop", INDEX(non_desktop)},
    {"panel orientation", INDEX(panel_orientation)},
    {"subconnector", INDEX(subconnector)},
    {"vrr_capable", INDEX(vrr_capable)},
#undef INDEX
};

static const struct prop_info crtc_info[] = {
#define INDEX(name) (offsetof(SDRMCRTC::UDRMCRTCProps, name) / sizeof(uint32_t))
    {"ACTIVE", INDEX(active)},   {"GAMMA_LUT", INDEX(gamma_lut)},         {"GAMMA_LUT_SIZE", INDEX(gamma_lut_size)},
    {"MODE_ID", INDEX(mode_id)}, {"OUT_FENCE_PTR", INDEX(out_fence_ptr)}, {"VRR_ENABLED", INDEX(vrr_enabled)},
    {"CTM", INDEX(ctm)},
#undef INDEX
};

static const struct prop_info plane_info[] = {
#define INDEX(name) (offsetof(SDRMPlane::UDRMPlaneProps, name) / sizeof(uint32_t))
    {"CRTC_H", INDEX(crtc_h)},
    {"CRTC_ID", INDEX(crtc_id)},
    {"CRTC_W", INDEX(crtc_w)},
    {"CRTC_X", INDEX(crtc_x)},
    {"CRTC_Y", INDEX(crtc_y)},
    {"FB_DAMAGE_CLIPS", INDEX(fb_damage_clips)},
    {"FB_ID", INDEX(fb_id)},
    {"HOTSPOT_X", INDEX(hotspot_x)},
    {"HOTSPOT_Y", INDEX(hotspot_y)},
    {"IN_FENCE_FD", INDEX(in_fence_fd)},
    {"IN_FORMATS", INDEX(in_formats)},
    {"SRC_H", INDEX(src_h)},
    {"SRC_W", INDEX(src_w)},
    {"SRC_X", INDEX(src_x)},
    {"SRC_Y", INDEX(src_y)},
    {"rotation", INDEX(rotation)},
    {"type", INDEX(type)},
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

    bool getDRMConnectorProps(int fd, uint32_t id, SDRMConnector::UDRMConnectorProps* out) {
        return scanProperties(fd, id, DRM_MODE_OBJECT_CONNECTOR, out->props, connector_info, sizeof(connector_info) / sizeof(connector_info[0]));
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
