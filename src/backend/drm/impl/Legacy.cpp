#include <aquamarine/backend/drm/Legacy.hpp>
#include <cstring>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <sys/mman.h>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
#define SP CSharedPointer

Aquamarine::CDRMLegacyImpl::CDRMLegacyImpl(Hyprutils::Memory::CSharedPointer<CDRMBackend> backend_) : backend(backend_) {
    ;
}

bool Aquamarine::CDRMLegacyImpl::commitInternal(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, const SDRMConnectorCommitData& data) {
    const auto& STATE = connector->output->state->state();
    SP<CDRMFB>  mainFB;
    bool        enable = STATE.enabled;

    if (enable) {
        if (!data.mainFB)
            connector->backend->backend->log(AQ_LOG_WARNING, "legacy drm: No buffer, will fall back to only modeset (if present)");
        else
            mainFB = data.mainFB;
    }

    if (data.modeset) {
        connector->backend->backend->log(AQ_LOG_DEBUG, std::format("legacy drm: Modesetting CRTC {}", connector->crtc->id));

        uint32_t dpms = enable ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF;
        if (drmModeConnectorSetProperty(connector->backend->gpu->fd, connector->id, connector->props.dpms, dpms)) {
            connector->backend->backend->log(AQ_LOG_ERROR, "legacy drm: Failed to set dpms");
            return false;
        }

        std::vector<uint32_t> connectors;
        drmModeModeInfo*      mode = nullptr;
        if (enable) {
            connectors.push_back(connector->id);
            mode = (drmModeModeInfo*)&data.modeInfo;
        }

        connector->backend->backend->log(AQ_LOG_DEBUG, std::format("legacy drm: Modesetting CRTC, connectors: {}", connectors.size()));
        connector->backend->backend->log(
            AQ_LOG_DEBUG,
            std::format("legacy drm: Modesetting CRTC, mode: clock {} hdisplay {} vdisplay {} vrefresh {}", mode->clock, mode->hdisplay, mode->vdisplay, mode->vrefresh));

        if (auto ret = drmModeSetCrtc(connector->backend->gpu->fd, connector->crtc->id, mainFB ? mainFB->id : -1, 0, 0, connectors.data(), connectors.size(), mode); ret) {
            connector->backend->backend->log(AQ_LOG_ERROR, std::format("legacy drm: drmModeSetCrtc failed: {}", strerror(-ret)));
            return false;
        }
    }

    // TODO: gamma

    // TODO: Adaptive sync

    // TODO: cursor plane
    if (drmModeSetCursor(connector->backend->gpu->fd, connector->crtc->id, 0, 0, 0))
        connector->backend->backend->log(AQ_LOG_ERROR, "legacy drm: cursor null failed");

    if (!enable)
        return true;

    if (!(data.flags & DRM_MODE_PAGE_FLIP_EVENT))
        return true;

    if (int ret = drmModePageFlip(connector->backend->gpu->fd, connector->crtc->id, mainFB ? mainFB->id : -1, data.flags, &connector->pendingPageFlip); ret) {
        connector->backend->backend->log(AQ_LOG_ERROR, std::format("legacy drm: drmModePageFlip failed: {}", strerror(-ret)));
        return false;
    }

    connector->isPageFlipPending = true;

    return true;
}

bool Aquamarine::CDRMLegacyImpl::testInternal(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, const SDRMConnectorCommitData& data) {
    return true; // TODO: lol
}

bool Aquamarine::CDRMLegacyImpl::commit(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, const SDRMConnectorCommitData& data) {
    if (!testInternal(connector, data))
        return false;

    return commitInternal(connector, data);
}