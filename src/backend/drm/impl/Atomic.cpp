#include <aquamarine/backend/drm/Atomic.hpp>
#include <cstdint>
#include <cstring>
#include <drm_mode.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <sys/mman.h>
#include <sstream>
#include "Shared.hpp"
#include "aquamarine/output/Output.hpp"

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
#define SP CSharedPointer

Aquamarine::CDRMAtomicRequest::CDRMAtomicRequest(Hyprutils::Memory::CWeakPointer<CDRMBackend> backend_) : backend(backend_), req(drmModeAtomicAlloc()) {
    if (!req)
        failed = true;
}

Aquamarine::CDRMAtomicRequest::~CDRMAtomicRequest() {
    if (req)
        drmModeAtomicFree(req);
}

void Aquamarine::CDRMAtomicRequest::add(uint32_t id, uint32_t prop, uint64_t val) {
    if (failed)
        return;

    TRACE(backend->log(AQ_LOG_TRACE, std::format("atomic drm request: adding id {} prop {} with value {}", id, prop, val)));

    if (id == 0 || prop == 0) {
        backend->log(AQ_LOG_ERROR, "atomic drm request: failed to add prop: id / prop == 0");
        return;
    }

    if (drmModeAtomicAddProperty(req, id, prop, val) < 0) {
        backend->log(AQ_LOG_ERROR, "atomic drm request: failed to add prop");
        failed = true;
    }
}

void Aquamarine::CDRMAtomicRequest::planeProps(Hyprutils::Memory::CSharedPointer<SDRMPlane> plane, Hyprutils::Memory::CSharedPointer<CDRMFB> fb, uint32_t crtc,
                                               Hyprutils::Math::Vector2D pos) {

    if (failed)
        return;

    if (!fb || !crtc) {
        // Disable the plane
        TRACE(backend->log(AQ_LOG_TRACE, std::format("atomic planeProps: disabling plane {}", plane->id)));
        add(plane->id, plane->props.fb_id, 0);
        add(plane->id, plane->props.crtc_id, 0);
        add(plane->id, plane->props.crtc_x, (uint64_t)pos.x);
        add(plane->id, plane->props.crtc_y, (uint64_t)pos.y);
        return;
    }

    TRACE(
        backend->log(AQ_LOG_TRACE,
                     std::format("atomic planeProps: prop blobs: src_x {}, src_y {}, src_w {}, src_h {}, crtc_w {}, crtc_h {}, fb_id {}, crtc_id {}", plane->props.src_x,
                                 plane->props.src_y, plane->props.src_w, plane->props.src_h, plane->props.crtc_w, plane->props.crtc_h, plane->props.fb_id, plane->props.crtc_id)));

    // src_ are 16.16 fixed point (lol)
    add(plane->id, plane->props.src_x, 0);
    add(plane->id, plane->props.src_y, 0);
    add(plane->id, plane->props.src_w, ((uint64_t)fb->buffer->size.x) << 16);
    add(plane->id, plane->props.src_h, ((uint64_t)fb->buffer->size.y) << 16);
    add(plane->id, plane->props.crtc_w, (uint32_t)fb->buffer->size.x);
    add(plane->id, plane->props.crtc_h, (uint32_t)fb->buffer->size.y);
    add(plane->id, plane->props.fb_id, fb->id);
    add(plane->id, plane->props.crtc_id, crtc);
    planePropsPos(plane, pos);
}

void Aquamarine::CDRMAtomicRequest::planePropsPos(Hyprutils::Memory::CSharedPointer<SDRMPlane> plane, Hyprutils::Math::Vector2D pos) {

    if (failed)
        return;

    TRACE(backend->log(AQ_LOG_TRACE, std::format("atomic planeProps: pos blobs: crtc_x {}, crtc_y {}", plane->props.crtc_x, plane->props.crtc_y)));

    add(plane->id, plane->props.crtc_x, (uint64_t)pos.x);
    add(plane->id, plane->props.crtc_y, (uint64_t)pos.y);
}

void Aquamarine::CDRMAtomicRequest::setConnector(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector) {
    conn = connector;
}

void Aquamarine::CDRMAtomicRequest::addConnector(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data) {
    const auto& STATE  = connector->output->state->state();
    const bool  enable = STATE.enabled && data.mainFB;

    TRACE(backend->log(AQ_LOG_TRACE,
                       std::format("atomic addConnector blobs: mode_id {}, active {}, crtc_id {}, link_status {}, content_type {}", connector->crtc->props.mode_id,
                                   connector->crtc->props.active, connector->props.crtc_id, connector->props.link_status, connector->props.content_type)));

    TRACE(backend->log(AQ_LOG_TRACE, std::format("atomic addConnector values: CRTC {}, mode {}", enable ? connector->crtc->id : 0, data.atomic.modeBlob)));

    conn = connector;
    drmModeModeInfo* currentMode = connector->getCurrentMode();
    bool modeDiffers = true;
    if (currentMode) {
        modeDiffers = memcmp(currentMode, &data.modeInfo, sizeof(drmModeModeInfo)) != 0;
        free(currentMode);
    }

    if (modeDiffers) {
        addConnectorModeset(connector, data);
    }
    addConnectorCursor(connector, data);

    add(connector->id, connector->props.crtc_id, enable ? connector->crtc->id : 0);

    if (enable && connector->props.content_type)
        add(connector->id, connector->props.content_type, STATE.contentType);

    add(connector->crtc->id, connector->crtc->props.active, enable);

    if (enable) {
        if (connector->output->supportsExplicit && STATE.committed & COutputState::AQ_OUTPUT_STATE_EXPLICIT_OUT_FENCE)
            add(connector->crtc->id, connector->crtc->props.out_fence_ptr, (uintptr_t)&STATE.explicitOutFence);

        if (connector->crtc->props.gamma_lut && data.atomic.gammad)
            add(connector->crtc->id, connector->crtc->props.gamma_lut, data.atomic.gammaLut);

        if (connector->crtc->props.degamma_lut && data.atomic.degammad)
            add(connector->crtc->id, connector->crtc->props.degamma_lut, data.atomic.degammaLut);

        if (connector->crtc->props.ctm && data.atomic.ctmd)
            add(connector->crtc->id, connector->crtc->props.ctm, data.atomic.ctmBlob);

        if (connector->crtc->props.vrr_enabled)
            add(connector->crtc->id, connector->crtc->props.vrr_enabled, (uint64_t)STATE.adaptiveSync);

        planeProps(connector->crtc->primary, data.mainFB, connector->crtc->id, {});

        if (connector->output->supportsExplicit && STATE.explicitInFence >= 0)
            add(connector->crtc->primary->id, connector->crtc->primary->props.in_fence_fd, STATE.explicitInFence);

        if (connector->crtc->primary->props.fb_damage_clips)
            add(connector->crtc->primary->id, connector->crtc->primary->props.fb_damage_clips, data.atomic.fbDamage);
    } else {
        planeProps(connector->crtc->primary, nullptr, 0, {});
    }
}

// HW capabilites aren't checked. Should be handled by the drivers (and highly unlikely to get a format outside of bpc range)
// https://drmdb.emersion.fr/properties/3233857728/max%20bpc
static uint8_t getMaxBPC(uint32_t drmFormat) {
    switch (drmFormat) {
        case DRM_FORMAT_XRGB8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_RGBX8888:
        case DRM_FORMAT_BGRX8888:

        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_RGBA8888:
        case DRM_FORMAT_BGRA8888: return 8;

        case DRM_FORMAT_XRGB2101010:
        case DRM_FORMAT_XBGR2101010:
        case DRM_FORMAT_RGBX1010102:
        case DRM_FORMAT_BGRX1010102:

        case DRM_FORMAT_ARGB2101010:
        case DRM_FORMAT_ABGR2101010:
        case DRM_FORMAT_RGBA1010102:
        case DRM_FORMAT_BGRA1010102: return 10;

        case DRM_FORMAT_XRGB16161616:
        case DRM_FORMAT_XBGR16161616:

        case DRM_FORMAT_ARGB16161616:
        case DRM_FORMAT_ABGR16161616: return 16;

        // FIXME? handle non-rgb formats and some weird stuff like DRM_FORMAT_AXBXGXRX106106106106
        default: return 8;
    }
}

void Aquamarine::CDRMAtomicRequest::addConnectorModeset(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data) {
    if (!data.modeset)
        return;

    const auto& STATE  = connector->output->state->state();
    const bool  enable = STATE.enabled && data.mainFB;

    add(connector->crtc->id, connector->crtc->props.mode_id, data.atomic.modeBlob);
    data.atomic.blobbed = true;

    if (!enable)
        return;

    if (connector->props.link_status)
        add(connector->id, connector->props.link_status, DRM_MODE_LINK_STATUS_GOOD);

    if (connector->props.max_bpc && connector->maxBpcBounds.at(1))
        add(connector->id, connector->props.max_bpc, getMaxBPC(data.mainFB->buffer->dmabuf().format));

    if (connector->props.Colorspace && connector->colorspace.BT2020_RGB)
        add(connector->id, connector->props.Colorspace, STATE.wideColorGamut ? connector->colorspace.BT2020_RGB : connector->colorspace.Default);

    if (connector->props.hdr_output_metadata && data.atomic.hdrd)
        add(connector->id, connector->props.hdr_output_metadata, data.atomic.hdrBlob);
}

void Aquamarine::CDRMAtomicRequest::addConnectorCursor(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data) {
    if (!connector->crtc->cursor)
        return;

    const auto& STATE  = connector->output->state->state();
    const bool  enable = STATE.enabled && data.mainFB;

    if (enable) {
        if (STATE.committed & COutputState::AQ_OUTPUT_STATE_CURSOR_SHAPE || STATE.committed & COutputState::AQ_OUTPUT_STATE_CURSOR_POS) {
            TRACE(backend->log(AQ_LOG_TRACE, STATE.committed & COutputState::AQ_OUTPUT_STATE_CURSOR_SHAPE ? "atomic addConnector cursor shape" : "atomic addConnector cursor pos"));
            if (STATE.committed & COutputState::AQ_OUTPUT_STATE_CURSOR_SHAPE) {
                if (!connector->output->cursorVisible)
                    planeProps(connector->crtc->cursor, nullptr, 0, {});
                else
                    planeProps(connector->crtc->cursor, data.cursorFB, connector->crtc->id, connector->output->cursorPos - connector->output->cursorHotspot);
            } else if (connector->output->cursorVisible)
                planePropsPos(connector->crtc->cursor, connector->output->cursorPos - connector->output->cursorHotspot);
        }
    } else
        planeProps(connector->crtc->cursor, nullptr, 0, {});
}

bool Aquamarine::CDRMAtomicRequest::commit(uint32_t flagssss) {
    static auto flagsToStr = [](uint32_t flags) {
        std::ostringstream result;
        if (flags & DRM_MODE_ATOMIC_ALLOW_MODESET)
            result << "ATOMIC_ALLOW_MODESET ";
        if (flags & DRM_MODE_ATOMIC_NONBLOCK)
            result << "ATOMIC_NONBLOCK ";
        if (flags & DRM_MODE_ATOMIC_TEST_ONLY)
            result << "ATOMIC_TEST_ONLY ";
        if (flags & DRM_MODE_PAGE_FLIP_EVENT)
            result << "PAGE_FLIP_EVENT ";
        if (flags & DRM_MODE_PAGE_FLIP_ASYNC)
            result << "PAGE_FLIP_ASYNC ";
        if (flags & (~DRM_MODE_ATOMIC_FLAGS))
            result << " + invalid...";
        return result.str();
    };

    if (failed) {
        backend->log((flagssss & DRM_MODE_ATOMIC_TEST_ONLY) ? AQ_LOG_DEBUG : AQ_LOG_ERROR, std::format("atomic drm request: failed to commit, failed flag set to true"));
        return false;
    }

    if (auto ret = drmModeAtomicCommit(backend->gpu->fd, req, flagssss, conn ? &conn->pendingPageFlip : nullptr); ret) {
        backend->log((flagssss & DRM_MODE_ATOMIC_TEST_ONLY) ? AQ_LOG_DEBUG : AQ_LOG_ERROR,
                     std::format("atomic drm request: failed to commit: {}, flags: {}", strerror(ret == -1 ? errno : -ret), flagsToStr(flagssss)));
        return false;
    }

    return true;
}

void Aquamarine::CDRMAtomicRequest::destroyBlob(uint32_t id) {
    if (!id)
        return;

    if (drmModeDestroyPropertyBlob(backend->gpu->fd, id))
        backend->log(AQ_LOG_ERROR, "atomic drm request: failed to destroy a blob");
}

void Aquamarine::CDRMAtomicRequest::commitBlob(uint32_t* current, uint32_t next) {
    if (*current == next)
        return;
    destroyBlob(*current);
    *current = next;
}

void Aquamarine::CDRMAtomicRequest::rollbackBlob(uint32_t* current, uint32_t next) {
    if (*current == next)
        return;
    destroyBlob(next);
}

void Aquamarine::CDRMAtomicRequest::rollback(SDRMConnectorCommitData& data) {
    if (!conn)
        return;

    conn->crtc->atomic.ownModeID = true;
    if (data.atomic.blobbed)
        rollbackBlob(&conn->crtc->atomic.modeID, data.atomic.modeBlob);
    rollbackBlob(&conn->crtc->atomic.gammaLut, data.atomic.gammaLut);
    rollbackBlob(&conn->crtc->atomic.ctm, data.atomic.ctmBlob);
    destroyBlob(data.atomic.fbDamage);
}

void Aquamarine::CDRMAtomicRequest::apply(SDRMConnectorCommitData& data) {
    if (!conn)
        return;

    if (!conn->crtc->atomic.ownModeID)
        conn->crtc->atomic.modeID = 0;

    conn->crtc->atomic.ownModeID = true;
    if (data.atomic.blobbed)
        commitBlob(&conn->crtc->atomic.modeID, data.atomic.modeBlob);
    commitBlob(&conn->crtc->atomic.gammaLut, data.atomic.gammaLut);
    commitBlob(&conn->crtc->atomic.ctm, data.atomic.ctmBlob);
    destroyBlob(data.atomic.fbDamage);
}

Aquamarine::CDRMAtomicImpl::CDRMAtomicImpl(Hyprutils::Memory::CSharedPointer<CDRMBackend> backend_) : backend(backend_) {
    ;
}

bool Aquamarine::CDRMAtomicImpl::prepareConnector(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data) {
    const auto& STATE  = connector->output->state->state();
    const bool  enable = STATE.enabled;
    const auto& MODE   = STATE.mode ? STATE.mode : STATE.customMode;

    if (data.modeset) {
        if (!enable)
            data.atomic.modeBlob = 0;
        else {
            if (drmModeCreatePropertyBlob(connector->backend->gpu->fd, &data.modeInfo, sizeof(drmModeModeInfo), &data.atomic.modeBlob)) {
                connector->backend->backend->log(AQ_LOG_ERROR, "atomic drm: failed to create a modeset blob");
                return false;
            }

            TRACE(connector->backend->log(AQ_LOG_TRACE,
                                          std::format("Connector blob id {}: clock {}, {}x{}, vrefresh {}, name: {}", data.atomic.modeBlob, data.modeInfo.clock,
                                                      data.modeInfo.hdisplay, data.modeInfo.vdisplay, data.modeInfo.vrefresh, data.modeInfo.name)));
        }
    }

    auto prepareGammaBlob = [connector](uint32_t prop, const std::vector<uint16_t>& gammaLut, uint32_t* blobId) -> bool {
        if (!prop) // TODO: allow this with legacy gamma, perhaps.
            connector->backend->backend->log(AQ_LOG_ERROR, "atomic drm: failed to commit gamma: no gamma_lut prop");
        else if (gammaLut.empty()) {
            blobId = nullptr;
            return true;
        } else {
            std::vector<drm_color_lut> lut;
            lut.resize(gammaLut.size() / 3); // [r,g,b]+

            for (size_t i = 0; i < lut.size(); ++i) {
                lut.at(i).red      = gammaLut.at(i * 3 + 0);
                lut.at(i).green    = gammaLut.at(i * 3 + 1);
                lut.at(i).blue     = gammaLut.at(i * 3 + 2);
                lut.at(i).reserved = 0;
            }

            if (drmModeCreatePropertyBlob(connector->backend->gpu->fd, lut.data(), lut.size() * sizeof(drm_color_lut), blobId)) {
                connector->backend->backend->log(AQ_LOG_ERROR, "atomic drm: failed to create a gamma blob");
                *blobId = 0;
            } else
                return true;
        }

        return false;
    };

    if (STATE.committed & COutputState::AQ_OUTPUT_STATE_GAMMA_LUT)
        data.atomic.gammad = prepareGammaBlob(connector->crtc->props.gamma_lut, STATE.gammaLut, &data.atomic.gammaLut);

    if (STATE.committed & COutputState::AQ_OUTPUT_STATE_DEGAMMA_LUT)
        data.atomic.degammad = prepareGammaBlob(connector->crtc->props.degamma_lut, STATE.degammaLut, &data.atomic.degammaLut);

    if ((STATE.committed & COutputState::AQ_OUTPUT_STATE_CTM) && data.ctm.has_value()) {
        if (!connector->crtc->props.ctm)
            connector->backend->backend->log(AQ_LOG_ERROR, "atomic drm: failed to commit ctm: no ctm prop support");
        else {
            static auto doubleToS3132Fixed = [](const double val) -> uint64_t {
                const uint64_t result = std::abs(val) * (1ULL << 32);
                if (val < 0)
                    return result | 1ULL << 63;
                return result;
            };

            drm_color_ctm ctm = {0};
            for (size_t i = 0; i < 9; ++i) {
                ctm.matrix[i] = doubleToS3132Fixed(data.ctm->getMatrix()[i]);
            }

            if (drmModeCreatePropertyBlob(connector->backend->gpu->fd, &ctm, sizeof(drm_color_ctm), &data.atomic.ctmBlob)) {
                connector->backend->backend->log(AQ_LOG_ERROR, "atomic drm: failed to create a ctm blob");
                data.atomic.ctmBlob = 0;
            } else
                data.atomic.ctmd = true;
        }
    }

    if ((STATE.committed & COutputState::AQ_OUTPUT_STATE_HDR) && data.hdrMetadata.has_value()) {
        if (!connector->props.hdr_output_metadata)
            connector->backend->backend->log(AQ_LOG_ERROR, "atomic drm: failed to commit hdr metadata: no HDR_OUTPUT_METADATA prop support");
        else {
            if (!data.hdrMetadata->hdmi_metadata_type1.eotf) {
                data.atomic.hdrBlob = 0;
                data.atomic.hdrd    = true;
            } else if (drmModeCreatePropertyBlob(connector->backend->gpu->fd, &data.hdrMetadata.value(), sizeof(hdr_output_metadata), &data.atomic.hdrBlob)) {
                connector->backend->backend->log(AQ_LOG_ERROR, "atomic drm: failed to create a hdr metadata blob");
                data.atomic.hdrBlob = 0;
                data.atomic.hdrd    = false;
            } else {
                data.atomic.hdrd = true;
                TRACE(connector->backend->backend->log(
                    AQ_LOG_TRACE,
                    std::format("atomic drm: setting hdr min {}, max {}, avg {}, content {}, primaries {},{} {},{} {},{} {},{}",
                                data.hdrMetadata->hdmi_metadata_type1.min_display_mastering_luminance, data.hdrMetadata->hdmi_metadata_type1.max_display_mastering_luminance,
                                data.hdrMetadata->hdmi_metadata_type1.max_fall, data.hdrMetadata->hdmi_metadata_type1.max_cll,
                                data.hdrMetadata->hdmi_metadata_type1.display_primaries[0].x, data.hdrMetadata->hdmi_metadata_type1.display_primaries[0].y,
                                data.hdrMetadata->hdmi_metadata_type1.display_primaries[1].x, data.hdrMetadata->hdmi_metadata_type1.display_primaries[1].y,
                                data.hdrMetadata->hdmi_metadata_type1.display_primaries[2].x, data.hdrMetadata->hdmi_metadata_type1.display_primaries[2].y,
                                data.hdrMetadata->hdmi_metadata_type1.display_primaries[0].x, data.hdrMetadata->hdmi_metadata_type1.white_point.x,
                                data.hdrMetadata->hdmi_metadata_type1.white_point.y)));
            }
        }
    }

    if ((STATE.committed & COutputState::AQ_OUTPUT_STATE_DAMAGE) && connector->crtc->primary->props.fb_damage_clips && MODE) {
        if (STATE.damage.empty())
            data.atomic.fbDamage = 0;
        else {
            TRACE(connector->backend->backend->log(AQ_LOG_TRACE, std::format("atomic drm: clipping damage to pixel size {}", MODE->pixelSize)));
            std::vector<pixman_box32_t> rects = STATE.damage.copy().intersect(CBox{{}, MODE->pixelSize}).getRects();
            if (drmModeCreatePropertyBlob(connector->backend->gpu->fd, rects.data(), sizeof(pixman_box32_t) * rects.size(), &data.atomic.fbDamage)) {
                connector->backend->backend->log(AQ_LOG_ERROR, "atomic drm: failed to create a damage blob");
                return false;
            }
        }
    }

    return true;
}

bool Aquamarine::CDRMAtomicImpl::commit(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector, SDRMConnectorCommitData& data) {
    if (!prepareConnector(connector, data))
        return false;

    CDRMAtomicRequest request(backend);

    request.addConnector(connector, data);

    uint32_t flags = data.flags;
    if (data.test)
        flags |= DRM_MODE_ATOMIC_TEST_ONLY;
    if (data.modeset)
        flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
    if (!data.blocking && !data.test)
        flags |= DRM_MODE_ATOMIC_NONBLOCK;

    const bool ok = request.commit(flags);

    if (ok) {
        request.apply(data);
        if (!data.test && data.mainFB && connector->output->state->state().enabled && (flags & DRM_MODE_PAGE_FLIP_EVENT))
            connector->isPageFlipPending = true;
    } else
        request.rollback(data);

    return ok;
}

bool Aquamarine::CDRMAtomicImpl::reset() {
    return true;
}

bool Aquamarine::CDRMAtomicImpl::moveCursor(SP<SDRMConnector> connector, bool skipSchedule) {
    if (!connector->output->cursorVisible || !connector->output->state->state().enabled || !connector->crtc || !connector->crtc->cursor)
        return true;

    if (!skipSchedule) {
        TRACE(connector->backend->log(AQ_LOG_TRACE, "atomic moveCursor"));
        connector->output->scheduleFrame(IOutput::AQ_SCHEDULE_CURSOR_MOVE);
    }

    return true;
}
