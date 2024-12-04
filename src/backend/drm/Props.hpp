#pragma once

#include <aquamarine/backend/DRM.hpp>

namespace Aquamarine {
    bool  getDRMConnectorProps(int fd, uint32_t id, SDRMConnector::UDRMConnectorProps* out);
    bool  getDRMConnectorColorspace(int fd, uint32_t id, SDRMConnector::UDRMConnectorColorspace* out);
    bool  getDRMCRTCProps(int fd, uint32_t id, SDRMCRTC::UDRMCRTCProps* out);
    bool  getDRMPlaneProps(int fd, uint32_t id, SDRMPlane::UDRMPlaneProps* out);
    bool  getDRMProp(int fd, uint32_t obj, uint32_t prop, uint64_t* ret);
    void* getDRMPropBlob(int fd, uint32_t obj, uint32_t prop, size_t* ret_len);
    char* getDRMPropEnum(int fd, uint32_t obj, uint32_t prop_id);
    bool  introspectDRMPropRange(int fd, uint32_t prop_id, uint64_t* min, uint64_t* max);
};
