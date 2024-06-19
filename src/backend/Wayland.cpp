#include <aquamarine/backend/Wayland.hpp>
#include <wayland.hpp>
#include <xdg-shell.hpp>
#include "Shared.hpp"
#include <string.h>
#include <xf86drm.h>
#include <gbm.h>
#include <fcntl.h>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
#define SP CSharedPointer

Aquamarine::CWaylandBackend::~CWaylandBackend() {
    if (drmState.fd >= 0)
        close(drmState.fd);
}

eBackendType Aquamarine::CWaylandBackend::type() {
    return AQ_BACKEND_WAYLAND;
}

Aquamarine::CWaylandBackend::CWaylandBackend(SP<CBackend> backend_) : backend(backend_) {
    ;
}

bool Aquamarine::CWaylandBackend::start() {
    backend->log(AQ_LOG_DEBUG, "Starting the Wayland backend!");

    waylandState.display = wl_display_connect(nullptr);

    if (!waylandState.display) {
        backend->log(AQ_LOG_ERROR, "Wayland backend cannot start: wl_display_connect failed (is a wayland compositor running?)");
        return false;
    }

    waylandState.registry = makeShared<CWlRegistry>((wl_proxy*)wl_display_get_registry(waylandState.display));

    backend->log(AQ_LOG_DEBUG, std::format("Got registry at 0x{:x}", (uintptr_t)waylandState.registry->resource()));

    waylandState.registry->setGlobal([this](CWlRegistry* r, uint32_t id, const char* name, uint32_t version) {
        backend->log(AQ_LOG_TRACE, std::format(" | received global: {} (version {}) with id {}", name, version, id));

        const std::string NAME = name;

        if (NAME == "wl_seat") {
            backend->log(AQ_LOG_TRACE, std::format("  > binding to global: {} (version {}) with id {}", name, 9, id));
            waylandState.seat = makeShared<CWlSeat>((wl_proxy*)wl_registry_bind((wl_registry*)waylandState.registry->resource(), id, &wl_seat_interface, 9));
            initSeat();
        } else if (NAME == "xdg_wm_base") {
            backend->log(AQ_LOG_TRACE, std::format("  > binding to global: {} (version {}) with id {}", name, 6, id));
            waylandState.xdg = makeShared<CXdgWmBase>((wl_proxy*)wl_registry_bind((wl_registry*)waylandState.registry->resource(), id, &xdg_wm_base_interface, 6));
            initShell();
        } else if (NAME == "wl_compositor") {
            backend->log(AQ_LOG_TRACE, std::format("  > binding to global: {} (version {}) with id {}", name, 6, id));
            waylandState.compositor = makeShared<CWlCompositor>((wl_proxy*)wl_registry_bind((wl_registry*)waylandState.registry->resource(), id, &wl_compositor_interface, 6));
        } else if (NAME == "zwp_linux_dmabuf_v1") {
            backend->log(AQ_LOG_TRACE, std::format("  > binding to global: {} (version {}) with id {}", name, 5, id));
            waylandState.dmabuf =
                makeShared<CZwpLinuxDmabufV1>((wl_proxy*)wl_registry_bind((wl_registry*)waylandState.registry->resource(), id, &zwp_linux_dmabuf_v1_interface, 5));
            if (!initDmabuf()) {
                backend->log(AQ_LOG_ERROR, "Wayland backend cannot start: zwp_linux_dmabuf_v1 init failed");
                waylandState.dmabufFailed = true;
            }
        }
    });
    waylandState.registry->setGlobalRemove([this](CWlRegistry* r, uint32_t id) { ; });

    wl_display_roundtrip(waylandState.display);

    if (!waylandState.xdg || !waylandState.compositor || !waylandState.seat || !waylandState.dmabuf || waylandState.dmabufFailed) {
        backend->log(AQ_LOG_ERROR, "Wayland backend cannot start: Missing protocols");
        return false;
    }

    dispatchEvents();

    createOutput("WAYLAND1");

    return true;
}

int Aquamarine::CWaylandBackend::drmFD() {
    return drmState.fd;
}

void Aquamarine::CWaylandBackend::createOutput(const std::string& szName) {
    auto o = outputs.emplace_back(SP<CWaylandOutput>(new CWaylandOutput(szName, self)));
    backend->events.newOutput.emit(SP<IOutput>(o));
}

int Aquamarine::CWaylandBackend::pollFD() {
    if (!waylandState.display)
        return -1;

    return wl_display_get_fd(waylandState.display);
}

bool Aquamarine::CWaylandBackend::dispatchEvents() {
    wl_display_flush(waylandState.display);

    if (wl_display_prepare_read(waylandState.display) == 0) {
        wl_display_read_events(waylandState.display);
        wl_display_dispatch_pending(waylandState.display);
    } else
        wl_display_dispatch(waylandState.display);

    int ret = 0;
    do {
        ret = wl_display_dispatch_pending(waylandState.display);
        wl_display_flush(waylandState.display);
    } while (ret > 0);

    return true;
}

Aquamarine::CWaylandKeyboard::CWaylandKeyboard(SP<CWlKeyboard> keyboard_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_) : keyboard(keyboard_), backend(backend_) {
    if (!keyboard->resource())
        return;

    backend->backend->log(AQ_LOG_DEBUG, "New wayland keyboard wl_keyboard");

    keyboard->setKey([this](CWlKeyboard* r, uint32_t serial, uint32_t timeMs, uint32_t key, wl_keyboard_key_state state) {
        events.key.emit(SKeyEvent{
            .timeMs  = timeMs,
            .key     = key,
            .pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED,
        });
    });

    keyboard->setModifiers([this](CWlKeyboard* r, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
        events.modifiers.emit(SModifiersEvent{
            .depressed = depressed,
            .latched   = latched,
            .locked    = locked,
            .group     = group,
        });
    });
}

Aquamarine::CWaylandKeyboard::~CWaylandKeyboard() {
    ;
}

const std::string& Aquamarine::CWaylandKeyboard::getName() {
    return name;
}

Aquamarine::CWaylandPointer::CWaylandPointer(SP<CWlPointer> pointer_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_) : pointer(pointer_), backend(backend_) {
    if (!pointer->resource())
        return;

    backend->backend->log(AQ_LOG_DEBUG, "New wayland pointer wl_pointer");

    pointer->setMotion([this](CWlPointer* r, uint32_t serial, wl_fixed_t x, wl_fixed_t y) {
        if (!backend->focusedOutput || (!backend->focusedOutput->state->mode && !backend->focusedOutput->state->customMode.has_value()))
            return;

        const Vector2D size =
            backend->focusedOutput->state->customMode.has_value() ? backend->focusedOutput->state->customMode->pixelSize : backend->focusedOutput->state->mode->pixelSize;

        Vector2D local = {wl_fixed_to_double(x), wl_fixed_to_double(y)};
        local          = local / size;

        events.warp.emit(SWarpEvent{
            .absolute = local,
        });
    });

    pointer->setEnter([this](CWlPointer* r, uint32_t serial, wl_proxy* surface, wl_fixed_t x, wl_fixed_t y) {
        backend->lastEnterSerial = serial;

        for (auto& o : backend->outputs) {
            if (o->waylandState.surface->resource() != surface)
                continue;

            backend->focusedOutput = o;
            backend->backend->log(AQ_LOG_DEBUG, std::format("[wayland] focus changed: {}", o->name));
            break;
        }
    });

    pointer->setButton([this](CWlPointer* r, uint32_t serial, uint32_t timeMs, uint32_t button, wl_pointer_button_state state) {
        events.button.emit(SButtonEvent{
            .timeMs  = timeMs,
            .button  = button,
            .pressed = state == WL_POINTER_BUTTON_STATE_PRESSED,
        });
    });

    pointer->setAxis([this](CWlPointer* r, uint32_t timeMs, wl_pointer_axis axis, wl_fixed_t value) {
        events.axis.emit(SAxisEvent{
            .timeMs = timeMs,
            .axis   = axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? AQ_POINTER_AXIS_HORIZONTAL : AQ_POINTER_AXIS_VERTICAL,
            .value  = wl_fixed_to_double(value),
        });
    });

    pointer->setFrame([this](CWlPointer* r) { events.frame.emit(); });
}

Aquamarine::CWaylandPointer::~CWaylandPointer() {
    ;
}

const std::string& Aquamarine::CWaylandPointer::getName() {
    return name;
}

void Aquamarine::CWaylandBackend::initSeat() {
    waylandState.seat->setCapabilities([this](CWlSeat* r, wl_seat_capability cap) {
        const bool HAS_KEYBOARD = ((uint32_t)cap) & WL_SEAT_CAPABILITY_KEYBOARD;
        const bool HAS_POINTER  = ((uint32_t)cap) & WL_SEAT_CAPABILITY_POINTER;

        if (HAS_KEYBOARD && keyboards.empty()) {
            auto k = keyboards.emplace_back(makeShared<CWaylandKeyboard>(makeShared<CWlKeyboard>(waylandState.seat->sendGetKeyboard()), self));
            backend->events.newKeyboard.emit(SP<IKeyboard>(k));
        } else if (!HAS_KEYBOARD && !keyboards.empty())
            keyboards.clear();

        if (HAS_POINTER && pointers.empty()) {
            auto p = pointers.emplace_back(makeShared<CWaylandPointer>(makeShared<CWlPointer>(waylandState.seat->sendGetPointer()), self));
            backend->events.newPointer.emit(SP<IPointer>(p));
        } else if (!HAS_POINTER && !pointers.empty())
            pointers.clear();
    });
}

void Aquamarine::CWaylandBackend::initShell() {
    waylandState.xdg->setPing([](CXdgWmBase* r, uint32_t serial) { r->sendPong(serial); });
}

bool Aquamarine::CWaylandBackend::initDmabuf() {
    waylandState.dmabufFeedback = makeShared<CZwpLinuxDmabufFeedbackV1>(waylandState.dmabuf->sendGetDefaultFeedback());
    if (!waylandState.dmabufFeedback) {
        backend->log(AQ_LOG_ERROR, "initDmabuf: failed to get default feedback");
        return false;
    }

    waylandState.dmabufFeedback->setDone([this](CZwpLinuxDmabufFeedbackV1* r) {
        // no-op
        backend->log(AQ_LOG_DEBUG, "zwp_linux_dmabuf_v1: Got done");
    });

    waylandState.dmabufFeedback->setMainDevice([this](CZwpLinuxDmabufFeedbackV1* r, wl_array* deviceArr) {
        backend->log(AQ_LOG_DEBUG, "zwp_linux_dmabuf_v1: Got main device");

        dev_t device;
        ASSERT(deviceArr->size == sizeof(device));
        memcpy(&device, deviceArr->data, sizeof(device));

        drmDevice* drmDev;
        if (drmGetDeviceFromDevId(device, /* flags */ 0, &drmDev) != 0) {
            backend->log(AQ_LOG_ERROR, "zwp_linux_dmabuf_v1: drmGetDeviceFromDevId failed");
            return;
        }

        const char* name = nullptr;
        if (drmDev->available_nodes & (1 << DRM_NODE_RENDER))
            name = drmDev->nodes[DRM_NODE_RENDER];
        else {
            // Likely a split display/render setup. Pick the primary node and hope
            // Mesa will open the right render node under-the-hood.
            ASSERT(drmDev->available_nodes & (1 << DRM_NODE_PRIMARY));
            name = drmDev->nodes[DRM_NODE_PRIMARY];
            backend->log(AQ_LOG_WARNING, "zwp_linux_dmabuf_v1: DRM device has no render node, using primary.");
        }

        if (!name) {
            backend->log(AQ_LOG_ERROR, "zwp_linux_dmabuf_v1: no node name");
            return;
        }

        drmState.nodeName = name;

        drmFreeDevice(&drmDev);

        backend->log(AQ_LOG_DEBUG, std::format("zwp_linux_dmabuf_v1: Got node {}", drmState.nodeName));
    });

    // TODO: format table and tranche

    wl_display_roundtrip(waylandState.display);

    if (!drmState.nodeName.empty()) {
        drmState.fd = open(drmState.nodeName.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (drmState.fd < 0) {
            backend->log(AQ_LOG_ERROR, std::format("zwp_linux_dmabuf_v1: Failed to open node {}", drmState.nodeName));
            return false;
        }

        backend->log(AQ_LOG_DEBUG, std::format("zwp_linux_dmabuf_v1: opened node {} with fd {}", drmState.nodeName, drmState.fd));
    }

    return true;
}

Aquamarine::CWaylandOutput::CWaylandOutput(const std::string& name_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_) : name(name_), backend(backend_) {
    errno = 0;

    waylandState.surface = makeShared<CWlSurface>(backend->waylandState.compositor->sendCreateSurface());

    if (!waylandState.surface->resource()) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {} failed: no surface given. Errno: {}", name, errno));
        return;
    }

    waylandState.xdgSurface = makeShared<CXdgSurface>(backend->waylandState.xdg->sendGetXdgSurface(waylandState.surface->resource()));

    if (!waylandState.xdgSurface->resource()) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {} failed: no xdgSurface given. Errno: {}", name, errno));
        return;
    }

    waylandState.xdgSurface->setConfigure([this](CXdgSurface* r, uint32_t serial) {
        backend->backend->log(AQ_LOG_DEBUG, std::format("Output {}: configure surface with {}", name, serial));
        r->sendAckConfigure(serial);
    });

    waylandState.xdgToplevel = makeShared<CXdgToplevel>(waylandState.xdgSurface->sendGetToplevel());

    if (!waylandState.xdgToplevel->resource()) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {} failed: no xdgToplevel given. Errno: {}", name, errno));
        return;
    }

    waylandState.xdgToplevel->setWmCapabilities(
        [this](CXdgToplevel* r, wl_array* arr) { backend->backend->log(AQ_LOG_DEBUG, std::format("Output {}: wm_capabilities received", name)); });

    waylandState.xdgToplevel->setConfigure([this](CXdgToplevel* r, int32_t w, int32_t h, wl_array* arr) {
        // we only create the swapchain here because in the main func we still don't have an allocator
        if (!swapchain) {
            swapchain = makeShared<CSwapchain>(backend->backend->allocator);
            if (!swapchain) {
                backend->backend->log(AQ_LOG_ERROR, std::format("Output {} failed: swapchain creation failed", name));
                return;
            }
        }

        backend->backend->log(AQ_LOG_DEBUG, std::format("Output {}: configure toplevel with {}x{}", name, w, h));
        events.state.emit(SStateEvent{.size = {w, h}});
        sendFrameAndSetCallback();
    });

    auto inputRegion = makeShared<CWlRegion>(backend->waylandState.compositor->sendCreateRegion());
    inputRegion->sendAdd(0, 0, INT32_MAX, INT32_MAX);

    waylandState.surface->sendSetInputRegion(inputRegion.get());
    waylandState.surface->sendAttach(nullptr, 0, 0);
    waylandState.surface->sendCommit();

    inputRegion->sendDestroy();

    backend->backend->log(AQ_LOG_DEBUG, std::format("Output {}: initialized", name));
}

Aquamarine::CWaylandOutput::~CWaylandOutput() {
    if (waylandState.xdgToplevel)
        waylandState.xdgToplevel->sendDestroy();
    if (waylandState.xdgSurface)
        waylandState.xdgSurface->sendDestroy();
    if (waylandState.surface)
        waylandState.surface->sendDestroy();
}

bool Aquamarine::CWaylandOutput::commit() {
    Vector2D pixelSize   = {};
    uint32_t refreshRate = 0;

    if (state->customMode)
        pixelSize = state->customMode->pixelSize;
    else if (state->mode)
        pixelSize = state->mode->pixelSize;
    else {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: pending state rejected: invalid mode", name));
        return false;
    }

    uint32_t format = state->drmFormat;

    if (format == DRM_FORMAT_INVALID) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: pending state rejected: invalid format", name));
        return false;
    }

    if (!swapchain->reconfigure(SSwapchainOptions{.length = 2, .size = pixelSize, .format = format})) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: pending state rejected: swapchain failed reconfiguring", name));
        return false;
    }

    if (!state->buffer) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: pending state rejected: no buffer", name));
        return false;
    }

    auto wlBuffer = wlBufferFromBuffer(state->buffer);

    if (!wlBuffer) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: pending state rejected: no wlBuffer??", name));
        return false;
    }

    if (wlBuffer->pendingRelease)
        backend->backend->log(AQ_LOG_WARNING, std::format("Output {}: pending state has a non-released buffer??", name));

    wlBuffer->pendingRelease = true;

    waylandState.surface->sendAttach(wlBuffer->waylandState.buffer.get(), 0, 0);
    waylandState.surface->sendDamageBuffer(0, 0, INT32_MAX, INT32_MAX);
    waylandState.surface->sendCommit();

    return true;
}

SP<CWaylandBuffer> Aquamarine::CWaylandOutput::wlBufferFromBuffer(SP<IBuffer> buffer) {
    std::erase_if(backendState.buffers, [this](const auto& el) { return el.first.expired() || !swapchain->contains(el.first.lock()); });

    for (auto& [k, v] : backendState.buffers) {
        if (k != buffer)
            continue;

        return v;
    }

    // create a new one
    auto wlBuffer = makeShared<CWaylandBuffer>(buffer, backend);

    if (!wlBuffer->good())
        return nullptr;

    backendState.buffers.emplace_back(std::make_pair<>(buffer, wlBuffer));

    return wlBuffer;
}

void Aquamarine::CWaylandOutput::sendFrameAndSetCallback() {
    events.frame.emit();
    if (waylandState.frameCallback)
        return;

    waylandState.frameCallback = makeShared<CWlCallback>(waylandState.surface->sendFrame());
    waylandState.frameCallback->setDone([this](CWlCallback* r, uint32_t ms) {
        events.frame.emit();
        waylandState.frameCallback.reset();
    });
}

Aquamarine::CWaylandBuffer::CWaylandBuffer(SP<IBuffer> buffer_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_) : buffer(buffer_), backend(backend_) {
    auto params = makeShared<CZwpLinuxBufferParamsV1>(backend->waylandState.dmabuf->sendCreateParams());

    if (!params) {
        backend->backend->log(AQ_LOG_ERROR, "WaylandBuffer: failed to query params");
        return;
    }

    auto attrs = buffer->dmabuf();

    for (size_t i = 0; i < attrs.planes; ++i) {
        params->sendAdd(attrs.fds.at(i), i, attrs.offsets.at(i), attrs.strides.at(i), attrs.modifier >> 32, attrs.modifier & 0xFFFFFFFF);
    }

    waylandState.buffer = makeShared<CWlBuffer>(params->sendCreateImmed(attrs.size.x, attrs.size.y, attrs.format, (zwpLinuxBufferParamsV1Flags)0));

    waylandState.buffer->setRelease([this](CWlBuffer* r) { pendingRelease = false; });

    params->sendDestroy();
}

Aquamarine::CWaylandBuffer::~CWaylandBuffer() {
    if (waylandState.buffer && waylandState.buffer->resource())
        waylandState.buffer->sendDestroy();
}

bool Aquamarine::CWaylandBuffer::good() {
    return waylandState.buffer && waylandState.buffer->resource();
}
