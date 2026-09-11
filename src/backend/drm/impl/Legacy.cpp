#include "aquamarine/output/Output.hpp"
#include <aquamarine/backend/drm/Legacy.hpp>
#include <cstring>
#include <format>
#include <vector>
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

bool Aquamarine::CDRMLegacyImpl::moveCursor(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, bool skipSchedule) {
    if (!connector->output->cursorVisible || !connector->output->state->state().enabled || !connector->crtc || !connector->crtc->cursor)
        return true;

    if (!skipSchedule)
        connector->output->scheduleFrame(IOutput::AQ_SCHEDULE_CURSOR_MOVE);

    return true;
}

bool Aquamarine::CDRMLegacyImpl::commitInternal(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data) {
    const auto& STATE = data.outputState;
    SP<CDRMFB>  mainFB;
    bool        enable = data.enabled;

    if (enable) {
        if (!data.mainFB)
            connector->backend->backend->log(AQ_LOG_WARNING, "legacy drm: No buffer, will fall back to only modeset (if present)");
        else
            mainFB = data.mainFB;
    }

    if (data.modeset) {
        connector->backend->backend->log(AQ_LOG_DEBUG, std::format("legacy drm: Modesetting CRTC {}", connector->crtc->id));

        uint32_t dpms = enable ? DRM_MODE_DPMS_ON : DRM_MODE_DPMS_OFF;
        if (drmModeConnectorSetProperty(connector->backend->gpu->fd, connector->id, connector->props.values.dpms, dpms)) {
            connector->backend->backend->log(AQ_LOG_ERROR, "legacy drm: Failed to set dpms");
            return false;
        }

        std::vector<uint32_t> connectors;
        drmModeModeInfo*      mode = nullptr;
        if (enable) {
            connectors.push_back(connector->id);
            mode = &data.modeInfo;
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

    if (data.committed & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_ADAPTIVE_SYNC) {
        if (STATE.adaptiveSync && !connector->canDoVrr) {
            connector->backend->backend->log(AQ_LOG_ERROR, std::format("legacy drm: connector {} can't do vrr", connector->id));
            return false;
        }

        if (connector->crtc->props.values.vrr_enabled) {
            if (auto ret =
                    drmModeObjectSetProperty(backend->gpu->fd, connector->crtc->id, DRM_MODE_OBJECT_CRTC, connector->crtc->props.values.vrr_enabled, (uint64_t)STATE.adaptiveSync);
                ret) {
                connector->backend->backend->log(AQ_LOG_ERROR, std::format("legacy drm: drmModeObjectSetProperty: vrr -> {} failed: {}", STATE.adaptiveSync, strerror(-ret)));
                return false;
            }
        }

        connector->output->vrrActive = STATE.adaptiveSync;
        connector->backend->backend->log(AQ_LOG_DEBUG, std::format("legacy drm: connector {} vrr -> {}", connector->id, STATE.adaptiveSync));
    }

    // Legacy exposes a single gamma ramp per crtc and no degamma. Unlike atomic
    // there is no blob to zero, so an empty lut is written as an identity ramp.
    // Re-send on modeset too, so a previous compositor's ramp cannot bleed into
    // our session (same reasoning as the atomic path, see #127).
    if (enable && connector->crtc && (data.modeset || (data.committed & COutputState::AQ_OUTPUT_STATE_GAMMA_LUT))) {
        size_t gammaSize = 0;
        if (auto crtcInfo = drmModeGetCrtc(connector->backend->gpu->fd, connector->crtc->id); crtcInfo) {
            gammaSize = crtcInfo->gamma_size;
            drmModeFreeCrtc(crtcInfo);
        }

        if (!gammaSize && (data.committed & COutputState::AQ_OUTPUT_STATE_GAMMA_LUT))
            connector->backend->backend->log(AQ_LOG_ERROR, "legacy drm: can't commit gamma: crtc reports no gamma ramp");
        else if (gammaSize) {
            std::vector<uint16_t> r(gammaSize), g(gammaSize), b(gammaSize);
            bool                  ok = true;

            if (STATE.gammaLut.empty()) {
                for (size_t i = 0; i < gammaSize; ++i)
                    r.at(i) = g.at(i) = b.at(i) = gammaSize > 1 ? (uint16_t)((i * 0xFFFF) / (gammaSize - 1)) : 0xFFFF;
            } else if (STATE.gammaLut.size() != gammaSize * 3) {
                connector->backend->backend->log(
                    AQ_LOG_ERROR, std::format("legacy drm: can't commit gamma: lut has {} entries, crtc wants {}", STATE.gammaLut.size() / 3, gammaSize));
                ok = false;
            } else {
                for (size_t i = 0; i < gammaSize; ++i) { // [r,g,b]+
                    r.at(i) = STATE.gammaLut.at(i * 3 + 0);
                    g.at(i) = STATE.gammaLut.at(i * 3 + 1);
                    b.at(i) = STATE.gammaLut.at(i * 3 + 2);
                }
            }

            // A failed gamma commit must not fail the whole commit: that would
            // take the output down over a colour tweak.
            if (ok) {
                if (auto ret = drmModeCrtcSetGamma(connector->backend->gpu->fd, connector->crtc->id, gammaSize, r.data(), g.data(), b.data()); ret)
                    connector->backend->backend->log(AQ_LOG_ERROR, std::format("legacy drm: drmModeCrtcSetGamma failed: {}", strerror(-ret)));
                else
                    connector->backend->backend->log(AQ_LOG_DEBUG, std::format("legacy drm: gamma ramp of {} entries set on crtc {}", gammaSize, connector->crtc->id));
            }
        }
    }

    if (data.cursorFB && connector->crtc->cursor && data.cursorVisible && enable &&
        (data.committed & COutputState::AQ_OUTPUT_STATE_CURSOR_SHAPE || data.committed & COutputState::AQ_OUTPUT_STATE_CURSOR_POS)) {
        uint32_t boHandle = 0;
        auto     attrs    = data.cursorFB->buffer->dmabuf();

        if (int ret = drmPrimeFDToHandle(connector->backend->gpu->fd, attrs.fds.at(0), &boHandle); ret) {
            connector->backend->backend->log(AQ_LOG_ERROR, std::format("legacy drm: drmPrimeFDToHandle failed: {}", strerror(-ret)));
            return false;
        }

        connector->backend->backend->log(AQ_LOG_DEBUG,
                                         std::format("legacy drm: cursor fb: {} with bo handle {} from fd {}, size {}", connector->backend->gpu->fd, boHandle,
                                                     data.cursorFB->buffer->dmabuf().fds.at(0), data.cursorFB->buffer->size));

        struct drm_mode_cursor2 request = {
            .flags   = DRM_MODE_CURSOR_BO | DRM_MODE_CURSOR_MOVE,
            .crtc_id = connector->crtc->id,
            .x       = (int32_t)data.cursorPos.x,
            .y       = (int32_t)data.cursorPos.y,
            .width   = (uint32_t)data.cursorFB->buffer->size.x,
            .height  = (uint32_t)data.cursorFB->buffer->size.y,
            .handle  = boHandle,
            .hot_x   = (int32_t)data.cursorHotspot.x,
            .hot_y   = (int32_t)data.cursorHotspot.y,
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

    const auto FLIPID = connector->crtc->armPageFlip(connector, data.flags & DRM_MODE_PAGE_FLIP_ASYNC);

    if (int ret = drmModePageFlip(connector->backend->gpu->fd, connector->crtc->id, mainFB ? mainFB->id : -1, data.flags, rc<void*>(FLIPID)); ret) {
        connector->backend->backend->log(AQ_LOG_ERROR, std::format("legacy drm: drmModePageFlip failed: {}", strerror(-ret)));
        connector->crtc->disarmPageFlip();
        return false;
    }

    connector->sched.onFrameSubmitted();

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

bool Aquamarine::CDRMLegacyImpl::reset() {
    bool ok = true;
    for (auto const& connector : backend->connectors) {
        if (!connector->crtc)
            continue;

        if (int ret = drmModeSetCrtc(backend->gpu->fd, connector->crtc->id, 0, 0, 0, nullptr, 0, nullptr); ret) {
            connector->backend->backend->log(AQ_LOG_ERROR, std::format("legacy drm: reset failed: {}", strerror(-ret)));
            ok = false;
        }
    }

    return ok;
}
