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

bool Aquamarine::CDRMLegacyImpl::moveCursor(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector) {
    if (!connector->output->cursorVisible || !connector->output->state->state().enabled || !connector->crtc || !connector->crtc->cursor)
        return true;

    connector->output->needsFrame = true;
    connector->output->scheduleFrame();

    return true;
}

bool Aquamarine::CDRMLegacyImpl::commitInternal(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data) {
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

        if (mode) {
            connector->backend->backend->log(
                AQ_LOG_DEBUG,
                std::format("legacy drm: Modesetting CRTC, mode: clock {} hdisplay {} vdisplay {} vrefresh {}", mode->clock, mode->hdisplay, mode->vdisplay, mode->vrefresh));
        } else
            connector->backend->backend->log(AQ_LOG_DEBUG, "legacy drm: Modesetting CRTC, mode null");

        if (auto ret = drmModeSetCrtc(connector->backend->gpu->fd, connector->crtc->id, mainFB ? mainFB->id : -1, 0, 0, connectors.data(), connectors.size(), mode); ret) {
            connector->backend->backend->log(AQ_LOG_ERROR, std::format("legacy drm: drmModeSetCrtc failed: {}", strerror(-ret)));
            return false;
        }
    }

    if (STATE.committed & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_ADAPTIVE_SYNC) {
        if (STATE.adaptiveSync && !connector->canDoVrr) {
            connector->backend->backend->log(AQ_LOG_ERROR, std::format("legacy drm: connector {} can't do vrr", connector->id));
            return false;
        }

        if (connector->crtc->props.vrr_enabled) {
            if (auto ret = drmModeObjectSetProperty(backend->gpu->fd, connector->crtc->id, DRM_MODE_OBJECT_CRTC, connector->crtc->props.vrr_enabled, (uint64_t)STATE.adaptiveSync);
                ret) {
                connector->backend->backend->log(AQ_LOG_ERROR, std::format("legacy drm: drmModeObjectSetProperty: vrr -> {} failed: {}", STATE.adaptiveSync, strerror(-ret)));
                return false;
            }
        }

        connector->output->vrrActive = STATE.adaptiveSync;
        connector->backend->backend->log(AQ_LOG_DEBUG, std::format("legacy drm: connector {} vrr -> {}", connector->id, STATE.adaptiveSync));
    }

    // TODO: gamma

    if (data.cursorFB && connector->crtc->cursor && connector->output->cursorVisible && enable) {
        uint32_t boHandle = 0;
        auto     attrs    = data.cursorFB->buffer->dmabuf();

        if (int ret = drmPrimeFDToHandle(connector->backend->gpu->fd, attrs.fds.at(0), &boHandle); ret) {
            connector->backend->backend->log(AQ_LOG_ERROR, std::format("legacy drm: drmPrimeFDToHandle failed: {}", strerror(-ret)));
            return false;
        }

        connector->backend->backend->log(AQ_LOG_DEBUG,
                                         std::format("legacy drm: cursor fb: {} with bo handle {} from fd {}, size {}", connector->backend->gpu->fd, boHandle,
                                                     data.cursorFB->buffer->dmabuf().fds.at(0), data.cursorFB->buffer->size));

        Vector2D                cursorPos = connector->output->cursorPos;

        struct drm_mode_cursor2 request = {
            .flags   = DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE,
            .crtc_id = connector->crtc->id,
            .x       = (int32_t)cursorPos.x,
            .y       = (int32_t)cursorPos.y,
            .width   = (uint32_t)data.cursorFB->buffer->size.x,
            .height  = (uint32_t)data.cursorFB->buffer->size.y,
            .handle  = boHandle,
            .hot_x   = (int32_t)connector->output->cursorHotspot.x,
            .hot_y   = (int32_t)connector->output->cursorHotspot.y,
        };

        int ret = drmIoctl(connector->backend->gpu->fd, DRM_IOCTL_MODE_CURSOR2, &request);

        if (boHandle && drmCloseBufferHandle(connector->backend->gpu->fd, boHandle))
            connector->backend->backend->log(AQ_LOG_ERROR, "legacy drm: drmCloseBufferHandle in cursor failed");

        if (ret) {
            connector->backend->backend->log(AQ_LOG_ERROR, std::format("legacy drm: cursor drmIoctl failed: {}", strerror(errno)));
            return false;
        }
    } else if (drmModeSetCursor(connector->backend->gpu->fd, connector->crtc->id, 0, 0, 0))
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

bool Aquamarine::CDRMLegacyImpl::testInternal(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data) {
    return true; // TODO: lol
}

bool Aquamarine::CDRMLegacyImpl::commit(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data) {
    if (!testInternal(connector, data))
        return false;

    return commitInternal(connector, data);
}

bool Aquamarine::CDRMLegacyImpl::reset(SP<SDRMConnector> connector) {
    if (!connector->crtc)
        return true;

    if (int ret = drmModeSetCrtc(backend->gpu->fd, connector->crtc->id, 0, 0, 0, nullptr, 0, nullptr); ret) {
        connector->backend->backend->log(AQ_LOG_ERROR, std::format("legacy drm: reset failed: {}", strerror(-ret)));
        return false;
    }

    return true;
}