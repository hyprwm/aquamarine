#include <aquamarine/backend/Wayland.hpp>
#include <wayland.hpp>
#include <xdg-shell.hpp>
#include "Shared.hpp"
#include "FormatUtils.hpp"
#include <string.h>
#include <xf86drm.h>
#include <gbm.h>
#include <fcntl.h>
#include <sys/mman.h>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
#define SP CSharedPointer

static std::pair<int, std::string> openExclusiveShm() {
    // Only absolute paths can be shared across different shm_open() calls
    srand(time(nullptr));
    std::string name = std::format("/aq{:x}", rand() % RAND_MAX);

    for (size_t i = 0; i < 69; ++i) {
        int fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0)
            return {fd, name};
    }

    return {-1, ""};
}

static int allocateSHMFile(size_t len) {
    auto [fd, name] = openExclusiveShm();
    if (fd < 0)
        return -1;

    shm_unlink(name.c_str());

    int ret;
    do {
        ret = ftruncate(fd, len);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

wl_shm_format shmFormatFromDRM(uint32_t drmFormat) {
    switch (drmFormat) {
        case DRM_FORMAT_XRGB8888: return WL_SHM_FORMAT_XRGB8888;
        case DRM_FORMAT_ARGB8888: return WL_SHM_FORMAT_ARGB8888;
        default: return (wl_shm_format)drmFormat;
    }

    return (wl_shm_format)drmFormat;
}

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

    waylandState.registry = makeShared<CCWlRegistry>((wl_proxy*)wl_display_get_registry(waylandState.display));

    backend->log(AQ_LOG_DEBUG, std::format("Got registry at 0x{:x}", (uintptr_t)waylandState.registry->resource()));

    waylandState.registry->setGlobal([this](CCWlRegistry* r, uint32_t id, const char* name, uint32_t version) {
        backend->log(AQ_LOG_TRACE, std::format(" | received global: {} (version {}) with id {}", name, version, id));

        const std::string NAME = name;

        if (NAME == "wl_seat") {
            backend->log(AQ_LOG_TRACE, std::format("  > binding to global: {} (version {}) with id {}", name, 9, id));
            waylandState.seat = makeShared<CCWlSeat>((wl_proxy*)wl_registry_bind((wl_registry*)waylandState.registry->resource(), id, &wl_seat_interface, 9));
            initSeat();
        } else if (NAME == "xdg_wm_base") {
            backend->log(AQ_LOG_TRACE, std::format("  > binding to global: {} (version {}) with id {}", name, 6, id));
            waylandState.xdg = makeShared<CCXdgWmBase>((wl_proxy*)wl_registry_bind((wl_registry*)waylandState.registry->resource(), id, &xdg_wm_base_interface, 6));
            initShell();
        } else if (NAME == "wl_compositor") {
            backend->log(AQ_LOG_TRACE, std::format("  > binding to global: {} (version {}) with id {}", name, 6, id));
            waylandState.compositor = makeShared<CCWlCompositor>((wl_proxy*)wl_registry_bind((wl_registry*)waylandState.registry->resource(), id, &wl_compositor_interface, 6));
        } else if (NAME == "wl_shm") {
            backend->log(AQ_LOG_TRACE, std::format("  > binding to global: {} (version {}) with id {}", name, 1, id));
            waylandState.shm = makeShared<CCWlShm>((wl_proxy*)wl_registry_bind((wl_registry*)waylandState.registry->resource(), id, &wl_shm_interface, 1));
        } else if (NAME == "zwp_linux_dmabuf_v1") {
            backend->log(AQ_LOG_TRACE, std::format("  > binding to global: {} (version {}) with id {}", name, 5, id));
            waylandState.dmabuf =
                makeShared<CCZwpLinuxDmabufV1>((wl_proxy*)wl_registry_bind((wl_registry*)waylandState.registry->resource(), id, &zwp_linux_dmabuf_v1_interface, 5));
            if (!initDmabuf()) {
                backend->log(AQ_LOG_ERROR, "Wayland backend cannot start: zwp_linux_dmabuf_v1 init failed");
                waylandState.dmabufFailed = true;
            }
        }
    });
    waylandState.registry->setGlobalRemove([this](CCWlRegistry* r, uint32_t id) { backend->log(AQ_LOG_DEBUG, std::format("Global {} removed", id)); });

    wl_display_roundtrip(waylandState.display);

    if (!waylandState.xdg || !waylandState.compositor || !waylandState.seat || !waylandState.dmabuf || waylandState.dmabufFailed || !waylandState.shm) {
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
    auto o  = outputs.emplace_back(SP<CWaylandOutput>(new CWaylandOutput(szName, self)));
    o->self = o;
    idleCallbacks.emplace_back([this, o]() { backend->events.newOutput.emit(SP<IOutput>(o)); });
}

std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>> Aquamarine::CWaylandBackend::pollFDs() {
    if (!waylandState.display)
        return {};

    return {makeShared<SPollFD>(wl_display_get_fd(waylandState.display), [this]() { dispatchEvents(); })};
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

    // dispatch frames
    if (backend->ready) {
        for (auto& f : idleCallbacks) {
            f();
        }
        idleCallbacks.clear();
    }

    return true;
}

uint32_t Aquamarine::CWaylandBackend::capabilities() {
    return AQ_BACKEND_CAPABILITY_POINTER;
}

bool Aquamarine::CWaylandBackend::setCursor(Hyprutils::Memory::CSharedPointer<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot) {
    // TODO:
    return true;
}

void Aquamarine::CWaylandBackend::onReady() {
    for (auto& o : outputs) {
        o->swapchain = CSwapchain::create(backend->allocator, self.lock());
        if (!o->swapchain) {
            backend->log(AQ_LOG_ERROR, std::format("Output {} failed: swapchain creation failed", o->name));
            continue;
        }
    }
}

Aquamarine::CWaylandKeyboard::CWaylandKeyboard(SP<CCWlKeyboard> keyboard_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_) : keyboard(keyboard_), backend(backend_) {
    if (!keyboard->resource())
        return;

    backend->backend->log(AQ_LOG_DEBUG, "New wayland keyboard wl_keyboard");

    keyboard->setKey([this](CCWlKeyboard* r, uint32_t serial, uint32_t timeMs, uint32_t key, wl_keyboard_key_state state) {
        events.key.emit(SKeyEvent{
            .timeMs  = timeMs,
            .key     = key,
            .pressed = state == WL_KEYBOARD_KEY_STATE_PRESSED,
        });
    });

    keyboard->setModifiers([this](CCWlKeyboard* r, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
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

Aquamarine::CWaylandPointer::CWaylandPointer(SP<CCWlPointer> pointer_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_) : pointer(pointer_), backend(backend_) {
    if (!pointer->resource())
        return;

    backend->backend->log(AQ_LOG_DEBUG, "New wayland pointer wl_pointer");

    pointer->setMotion([this](CCWlPointer* r, uint32_t serial, wl_fixed_t x, wl_fixed_t y) {
        const auto STATE = backend->focusedOutput->state->state();

        if (!backend->focusedOutput || (!STATE.mode && !STATE.customMode))
            return;

        const Vector2D size = STATE.customMode ? STATE.customMode->pixelSize : STATE.mode->pixelSize;

        Vector2D       local = {wl_fixed_to_double(x), wl_fixed_to_double(y)};
        local                = local / size;

        timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);

        events.warp.emit(SWarpEvent{
            .timeMs   = (uint32_t)(now.tv_sec * 1000 + now.tv_nsec / 1000000),
            .absolute = local,
        });
    });

    pointer->setEnter([this](CCWlPointer* r, uint32_t serial, wl_proxy* surface, wl_fixed_t x, wl_fixed_t y) {
        backend->lastEnterSerial = serial;

        for (auto& o : backend->outputs) {
            if (o->waylandState.surface->resource() != surface)
                continue;

            backend->focusedOutput = o;
            backend->backend->log(AQ_LOG_DEBUG, std::format("[wayland] focus changed: {}", o->name));
            o->onEnter(pointer, serial);
            break;
        }
    });

    pointer->setLeave([this](CCWlPointer* r, uint32_t serial, wl_proxy* surface) {
        for (auto& o : backend->outputs) {
            if (o->waylandState.surface->resource() != surface)
                continue;

            o->cursorState.serial = 0;
        }
    });

    pointer->setButton([this](CCWlPointer* r, uint32_t serial, uint32_t timeMs, uint32_t button, wl_pointer_button_state state) {
        events.button.emit(SButtonEvent{
            .timeMs  = timeMs,
            .button  = button,
            .pressed = state == WL_POINTER_BUTTON_STATE_PRESSED,
        });
    });

    pointer->setAxis([this](CCWlPointer* r, uint32_t timeMs, wl_pointer_axis axis, wl_fixed_t value) {
        events.axis.emit(SAxisEvent{
            .timeMs = timeMs,
            .axis   = axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? AQ_POINTER_AXIS_HORIZONTAL : AQ_POINTER_AXIS_VERTICAL,
            .delta  = wl_fixed_to_double(value),
        });
    });

    pointer->setFrame([this](CCWlPointer* r) { events.frame.emit(); });
}

Aquamarine::CWaylandPointer::~CWaylandPointer() {
    ;
}

const std::string& Aquamarine::CWaylandPointer::getName() {
    return name;
}

void Aquamarine::CWaylandBackend::initSeat() {
    waylandState.seat->setCapabilities([this](CCWlSeat* r, wl_seat_capability cap) {
        const bool HAS_KEYBOARD = ((uint32_t)cap) & WL_SEAT_CAPABILITY_KEYBOARD;
        const bool HAS_POINTER  = ((uint32_t)cap) & WL_SEAT_CAPABILITY_POINTER;

        if (HAS_KEYBOARD && keyboards.empty()) {
            auto k = keyboards.emplace_back(makeShared<CWaylandKeyboard>(makeShared<CCWlKeyboard>(waylandState.seat->sendGetKeyboard()), self));
            idleCallbacks.emplace_back([this, k]() { backend->events.newKeyboard.emit(SP<IKeyboard>(k)); });
        } else if (!HAS_KEYBOARD && !keyboards.empty())
            keyboards.clear();

        if (HAS_POINTER && pointers.empty()) {
            auto p = pointers.emplace_back(makeShared<CWaylandPointer>(makeShared<CCWlPointer>(waylandState.seat->sendGetPointer()), self));
            idleCallbacks.emplace_back([this, p]() { backend->events.newPointer.emit(SP<IPointer>(p)); });
        } else if (!HAS_POINTER && !pointers.empty())
            pointers.clear();
    });
}

void Aquamarine::CWaylandBackend::initShell() {
    waylandState.xdg->setPing([](CCXdgWmBase* r, uint32_t serial) { r->sendPong(serial); });
}

bool Aquamarine::CWaylandBackend::initDmabuf() {
    waylandState.dmabufFeedback = makeShared<CCZwpLinuxDmabufFeedbackV1>(waylandState.dmabuf->sendGetDefaultFeedback());
    if (!waylandState.dmabufFeedback) {
        backend->log(AQ_LOG_ERROR, "initDmabuf: failed to get default feedback");
        return false;
    }

    waylandState.dmabufFeedback->setDone([this](CCZwpLinuxDmabufFeedbackV1* r) {
        // no-op
        backend->log(AQ_LOG_DEBUG, "zwp_linux_dmabuf_v1: Got done");
    });

    waylandState.dmabufFeedback->setMainDevice([this](CCZwpLinuxDmabufFeedbackV1* r, wl_array* deviceArr) {
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

    waylandState.dmabufFeedback->setFormatTable([this](CCZwpLinuxDmabufFeedbackV1* r, int32_t fd, uint32_t size) {
#pragma pack(push, 1)
        struct wlDrmFormatMarshalled {
            uint32_t drmFormat;
            char     pad[4];
            uint64_t modifier;
        };
#pragma pack(pop)
        static_assert(sizeof(wlDrmFormatMarshalled) == 16);

        auto formatTable = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (formatTable == MAP_FAILED) {
            backend->log(AQ_LOG_ERROR, std::format("zwp_linux_dmabuf_v1: Failed to mmap the format table"));
            return;
        }

        const auto FORMATS = (wlDrmFormatMarshalled*)formatTable;

        for (size_t i = 0; i < size / 16; ++i) {
            auto& fmt = FORMATS[i];

            auto  modName = drmGetFormatModifierName(fmt.modifier);
            backend->log(AQ_LOG_DEBUG, std::format("zwp_linux_dmabuf_v1: Got format {} with modifier {}", fourccToName(fmt.drmFormat), modName ? modName : "UNKNOWN"));
            free(modName);

            auto it = std::find_if(dmabufFormats.begin(), dmabufFormats.end(), [&fmt](const auto& e) { return e.drmFormat == fmt.drmFormat; });
            if (it == dmabufFormats.end()) {
                dmabufFormats.emplace_back(SDRMFormat{.drmFormat = fmt.drmFormat, .modifiers = {fmt.modifier}});
                continue;
            }

            it->modifiers.emplace_back(fmt.modifier);
        }

        munmap(formatTable, size);
    });

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

std::vector<SDRMFormat> Aquamarine::CWaylandBackend::getRenderFormats() {
    return dmabufFormats;
}

std::vector<SDRMFormat> Aquamarine::CWaylandBackend::getCursorFormats() {
    return dmabufFormats;
}

Aquamarine::CWaylandOutput::CWaylandOutput(const std::string& name_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_) : backend(backend_) {
    name = name_;

    waylandState.surface = makeShared<CCWlSurface>(backend->waylandState.compositor->sendCreateSurface());

    if (!waylandState.surface->resource()) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {} failed: no surface given. Errno: {}", name, errno));
        return;
    }

    waylandState.xdgSurface = makeShared<CCXdgSurface>(backend->waylandState.xdg->sendGetXdgSurface(waylandState.surface->resource()));

    if (!waylandState.xdgSurface->resource()) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {} failed: no xdgSurface given. Errno: {}", name, errno));
        return;
    }

    waylandState.xdgSurface->setConfigure([this](CCXdgSurface* r, uint32_t serial) {
        backend->backend->log(AQ_LOG_DEBUG, std::format("Output {}: configure surface with {}", name, serial));
        r->sendAckConfigure(serial);
    });

    waylandState.xdgToplevel = makeShared<CCXdgToplevel>(waylandState.xdgSurface->sendGetToplevel());

    if (!waylandState.xdgToplevel->resource()) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {} failed: no xdgToplevel given. Errno: {}", name, errno));
        return;
    }

    waylandState.xdgToplevel->setWmCapabilities(
        [this](CCXdgToplevel* r, wl_array* arr) { backend->backend->log(AQ_LOG_DEBUG, std::format("Output {}: wm_capabilities received", name)); });

    waylandState.xdgToplevel->setConfigure([this](CCXdgToplevel* r, int32_t w, int32_t h, wl_array* arr) {
        backend->backend->log(AQ_LOG_DEBUG, std::format("Output {}: configure toplevel with {}x{}", name, w, h));
        events.state.emit(SStateEvent{.size = {w, h}});
        sendFrameAndSetCallback();
    });

    auto inputRegion = makeShared<CCWlRegion>(backend->waylandState.compositor->sendCreateRegion());
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

bool Aquamarine::CWaylandOutput::test() {
    return true; // TODO:
}

bool Aquamarine::CWaylandOutput::commit() {
    Vector2D pixelSize   = {};
    uint32_t refreshRate = 0;

    if (state->internalState.customMode)
        pixelSize = state->internalState.customMode->pixelSize;
    else if (state->internalState.mode)
        pixelSize = state->internalState.mode->pixelSize;
    else {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: pending state rejected: invalid mode", name));
        return false;
    }

    uint32_t format = state->internalState.drmFormat;

    if (format == DRM_FORMAT_INVALID) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: pending state rejected: invalid format", name));
        return false;
    }

    if (!swapchain) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: no swapchain, lying because it will soon be here", name));
        return true;
    }

    if (!swapchain->reconfigure(SSwapchainOptions{.length = 2, .size = pixelSize, .format = format})) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: pending state rejected: swapchain failed reconfiguring", name));
        return false;
    }

    if (!state->internalState.buffer) {
        // if the consumer explicitly committed a null buffer, that's a violation.
        if (state->internalState.committed & COutputState::AQ_OUTPUT_STATE_BUFFER) {
            backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: pending state rejected: no buffer", name));
            return false;
        }

        events.commit.emit();
        state->onCommit();
        return true;
    }

    auto wlBuffer = wlBufferFromBuffer(state->internalState.buffer);

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

    readyForFrameCallback = true;

    events.commit.emit();

    state->onCommit();

    return true;
}

SP<IBackendImplementation> Aquamarine::CWaylandOutput::getBackend() {
    return SP<IBackendImplementation>(backend.lock());
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
    frameScheduled = false;
    if (waylandState.frameCallback || !readyForFrameCallback)
        return;

    waylandState.frameCallback = makeShared<CCWlCallback>(waylandState.surface->sendFrame());
    waylandState.frameCallback->setDone([this](CCWlCallback* r, uint32_t ms) { onFrameDone(); });
}

void Aquamarine::CWaylandOutput::onFrameDone() {
    waylandState.frameCallback.reset();
    readyForFrameCallback = false;

    if (frameScheduledWhileWaiting)
        sendFrameAndSetCallback();
    else
        events.frame.emit();

    frameScheduledWhileWaiting = false;
}

bool Aquamarine::CWaylandOutput::setCursor(Hyprutils::Memory::CSharedPointer<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot) {
    if (!cursorState.cursorSurface)
        cursorState.cursorSurface = makeShared<CCWlSurface>(backend->waylandState.compositor->sendCreateSurface());

    if (!cursorState.cursorSurface) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: Failed to create a wl_surface for the cursor", name));
        return false;
    }

    if (!buffer) {
        cursorState.cursorBuffer.reset();
        cursorState.cursorWlBuffer.reset();
        backend->pointers.at(0)->pointer->sendSetCursor(cursorState.serial, nullptr, cursorState.hotspot.x, cursorState.hotspot.y);
        return true;
    }

    cursorState.cursorBuffer = buffer;
    cursorState.hotspot      = hotspot;

    if (buffer->shm().success) {
        auto attrs                    = buffer->shm();
        auto [pixelData, fmt, bufLen] = buffer->beginDataPtr(0);

        int fd = allocateSHMFile(bufLen);
        if (fd < 0) {
            backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: Failed to allocate a shm file", name));
            return false;
        }

        void* data = mmap(nullptr, bufLen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (data == MAP_FAILED) {
            backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: Failed to mmap the cursor pixel data", name));
            close(fd);
            return false;
        }

        memcpy(data, pixelData, bufLen);
        munmap(data, bufLen);

        auto pool = makeShared<CCWlShmPool>(backend->waylandState.shm->sendCreatePool(fd, bufLen));
        if (!pool) {
            backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: Failed to submit a wl_shm pool", name));
            close(fd);
            return false;
        }

        cursorState.cursorWlBuffer = makeShared<CCWlBuffer>(pool->sendCreateBuffer(0, attrs.size.x, attrs.size.y, attrs.stride, shmFormatFromDRM(attrs.format)));

        pool.reset();

        close(fd);
    } else if (auto attrs = buffer->dmabuf(); attrs.success) {
        auto params = makeShared<CCZwpLinuxBufferParamsV1>(backend->waylandState.dmabuf->sendCreateParams());

        for (int i = 0; i < attrs.planes; ++i) {
            params->sendAdd(attrs.fds.at(i), i, attrs.offsets.at(i), attrs.strides.at(i), attrs.modifier >> 32, attrs.modifier & 0xFFFFFFFF);
        }

        cursorState.cursorWlBuffer = makeShared<CCWlBuffer>(params->sendCreateImmed(attrs.size.x, attrs.size.y, attrs.format, (zwpLinuxBufferParamsV1Flags)0));
    } else {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: Failed to create a buffer for cursor: No known attrs (tried dmabuf / shm)", name));
        return false;
    }

    if (!cursorState.cursorWlBuffer) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {}: Failed to create a buffer for cursor", name));
        return false;
    }

    cursorState.cursorSurface->sendSetBufferScale(1);
    cursorState.cursorSurface->sendSetBufferTransform(WL_OUTPUT_TRANSFORM_NORMAL);
    cursorState.cursorSurface->sendAttach(cursorState.cursorWlBuffer.get(), 0, 0);
    cursorState.cursorSurface->sendDamage(0, 0, INT32_MAX, INT32_MAX);
    cursorState.cursorSurface->sendCommit();

    // this may fail if we are not in focus
    if (!backend->pointers.empty() && cursorState.serial)
        backend->pointers.at(0)->pointer->sendSetCursor(cursorState.serial, cursorState.cursorSurface.get(), cursorState.hotspot.x, cursorState.hotspot.y);

    return true;
}

void Aquamarine::CWaylandOutput::moveCursor(const Hyprutils::Math::Vector2D& coord) {
    return;
}

void Aquamarine::CWaylandOutput::onEnter(SP<CCWlPointer> pointer, uint32_t serial) {
    cursorState.serial = serial;

    if (!cursorState.cursorSurface)
        return;

    pointer->sendSetCursor(serial, cursorState.cursorSurface.get(), cursorState.hotspot.x, cursorState.hotspot.y);
}

Hyprutils::Math::Vector2D Aquamarine::CWaylandOutput::cursorPlaneSize() {
    return {-1, -1}; // no limit
}

void Aquamarine::CWaylandOutput::scheduleFrame() {
    if (frameScheduled)
        return;

    frameScheduled = true;

    if (waylandState.frameCallback)
        frameScheduledWhileWaiting = true;
    else
        backend->idleCallbacks.emplace_back([this]() { sendFrameAndSetCallback(); });
}

Aquamarine::CWaylandBuffer::CWaylandBuffer(SP<IBuffer> buffer_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_) : buffer(buffer_), backend(backend_) {
    auto params = makeShared<CCZwpLinuxBufferParamsV1>(backend->waylandState.dmabuf->sendCreateParams());

    if (!params) {
        backend->backend->log(AQ_LOG_ERROR, "WaylandBuffer: failed to query params");
        return;
    }

    auto attrs = buffer->dmabuf();

    for (size_t i = 0; i < attrs.planes; ++i) {
        params->sendAdd(attrs.fds.at(i), i, attrs.offsets.at(i), attrs.strides.at(i), attrs.modifier >> 32, attrs.modifier & 0xFFFFFFFF);
    }

    waylandState.buffer = makeShared<CCWlBuffer>(params->sendCreateImmed(attrs.size.x, attrs.size.y, attrs.format, (zwpLinuxBufferParamsV1Flags)0));

    waylandState.buffer->setRelease([this](CCWlBuffer* r) { pendingRelease = false; });

    params->sendDestroy();
}

Aquamarine::CWaylandBuffer::~CWaylandBuffer() {
    if (waylandState.buffer && waylandState.buffer->resource())
        waylandState.buffer->sendDestroy();
}

bool Aquamarine::CWaylandBuffer::good() {
    return waylandState.buffer && waylandState.buffer->resource();
}
