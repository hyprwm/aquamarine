#include <aquamarine/backend/Wayland.hpp>
#include <wayland.hpp>
#include <xdg-shell.hpp>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
#define SP CSharedPointer

Aquamarine::CWaylandBackend::~CWaylandBackend() {
    ;
}
eBackendType Aquamarine::CWaylandBackend::type() {
    return AQ_BACKEND_WAYLAND;
}

Aquamarine::CWaylandBackend::CWaylandBackend(Hyprutils::Memory::CSharedPointer<CBackend> backend_) : backend(backend_) {
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
        }
    });
    waylandState.registry->setGlobalRemove([this](CWlRegistry* r, uint32_t id) { ; });

    wl_display_roundtrip(waylandState.display);

    if (!waylandState.xdg || !waylandState.compositor || !waylandState.seat) {
        backend->log(AQ_LOG_ERROR, "Wayland backend cannot start: Missing protocols");
        return false;
    }

    dispatchEvents();

    createOutput("WAYLAND1");

    return true;
}

void Aquamarine::CWaylandBackend::createOutput(const std::string& szName) {
    outputs.emplace_back(SP<CWaylandOutput>(new CWaylandOutput(szName, self)));
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

Aquamarine::CWaylandKeyboard::CWaylandKeyboard(Hyprutils::Memory::CSharedPointer<CWlKeyboard> keyboard_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_) :
    keyboard(keyboard_), backend(backend_) {
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
        events.key.emit(SModifiersEvent{
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

Aquamarine::CWaylandPointer::CWaylandPointer(Hyprutils::Memory::CSharedPointer<CWlPointer> pointer_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_) :
    pointer(pointer_), backend(backend_) {
    if (!pointer->resource())
        return;

    backend->backend->log(AQ_LOG_DEBUG, "New wayland pointer wl_pointer");

    pointer->setMotion([this](CWlPointer* r, uint32_t serial, wl_fixed_t x, wl_fixed_t y) {
        if (!backend->focusedOutput || !backend->focusedOutput->state->mode)
            return;

        Vector2D local = {wl_fixed_to_double(x), wl_fixed_to_double(y)};
        local          = local / backend->focusedOutput->state->mode->pixelSize;

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

        if (HAS_KEYBOARD && keyboards.empty())
            keyboards.emplace_back(makeShared<CWaylandKeyboard>(SP<CWlKeyboard>(waylandState.seat->sendGetKeyboard()), self));
        else if (!HAS_KEYBOARD && !keyboards.empty())
            keyboards.clear();

        if (HAS_POINTER && pointers.empty())
            keyboards.emplace_back(makeShared<CWaylandKeyboard>(SP<CWlKeyboard>(waylandState.seat->sendGetKeyboard()), self));
        else if (!HAS_POINTER && !keyboards.empty())
            keyboards.clear();
    });
}

void Aquamarine::CWaylandBackend::initShell() {
    waylandState.xdg->setPing([](CXdgWmBase* r, uint32_t serial) { r->sendPong(serial); });
}

Aquamarine::CWaylandOutput::CWaylandOutput(const std::string& name_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_) : name(name_), backend(backend_) {
    errno = 0;

    waylandState.surface = SP<CWlSurface>(backend->waylandState.compositor->sendCreateSurface());

    if (!waylandState.surface->resource()) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {} failed: no surface given. Errno: {}", name, errno));
        return;
    }

    waylandState.xdgSurface = SP<CXdgSurface>(backend->waylandState.xdg->sendGetXdgSurface(waylandState.surface->resource()));

    if (!waylandState.xdgSurface->resource()) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {} failed: no xdgSurface given. Errno: {}", name, errno));
        return;
    }

    waylandState.xdgSurface->setConfigure([this](CXdgSurface* r, uint32_t serial) {
        backend->backend->log(AQ_LOG_DEBUG, std::format("Output {}: configure surface with {}", name, serial));
        r->sendAckConfigure(serial);
    });

    waylandState.xdgToplevel = SP<CXdgToplevel>(waylandState.xdgSurface->sendGetToplevel());

    if (!waylandState.xdgToplevel->resource()) {
        backend->backend->log(AQ_LOG_ERROR, std::format("Output {} failed: no xdgToplevel given. Errno: {}", name, errno));
        return;
    }

    waylandState.xdgToplevel->setWmCapabilities(
        [this](CXdgToplevel* r, wl_array* arr) { backend->backend->log(AQ_LOG_DEBUG, std::format("Output {}: wm_capabilities received", name)); });

    waylandState.xdgToplevel->setConfigure([this](CXdgToplevel* r, int32_t w, int32_t h, wl_array* arr) {
        backend->backend->log(AQ_LOG_DEBUG, std::format("Output {}: configure toplevel with {}x{}", name, w, h));
        events.state.emit(SStateEvent{.size = {w, h}});
    });

    waylandState.surface->sendAttach(nullptr, 0, 0);
    waylandState.surface->sendCommit();

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

void Aquamarine::CWaylandOutput::commit() {
    ;
}