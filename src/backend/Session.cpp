#include <aquamarine/backend/Backend.hpp>

extern "C" {
#include <libseat.h>
#include <libinput.h>
#include <libudev.h>
#include <cstring>
#include <xf86drm.h>
#include <sys/stat.h>
#include <xf86drmMode.h>
}

using namespace Aquamarine;
using namespace Hyprutils::Memory;
#define SP CSharedPointer

static const std::string AQ_UNKNOWN_DEVICE_NAME = "UNKNOWN";

// we can't really do better with libseat/libinput logs
// because they don't allow us to pass "data" or anything...
// Nobody should create multiple backends anyways really
Hyprutils::Memory::CSharedPointer<CBackend> backendInUse;

//
static Aquamarine::eBackendLogLevel logLevelFromLibseat(libseat_log_level level) {
    switch (level) {
        case LIBSEAT_LOG_LEVEL_ERROR: return AQ_LOG_ERROR;
        case LIBSEAT_LOG_LEVEL_SILENT: return AQ_LOG_TRACE;
        default: break;
    }

    return AQ_LOG_DEBUG;
}

static Aquamarine::eBackendLogLevel logLevelFromLibinput(libinput_log_priority level) {
    switch (level) {
        case LIBINPUT_LOG_PRIORITY_ERROR: return AQ_LOG_ERROR;
        default: break;
    }

    return AQ_LOG_DEBUG;
}

static void libseatLog(libseat_log_level level, const char* fmt, va_list args) {
    if (!backendInUse)
        return;

    static char string[1024];
    vsnprintf(string, sizeof(string), fmt, args);

    backendInUse->log(logLevelFromLibseat(level), std::format("[libseat] {}", string));
}

static void libinputLog(libinput*, libinput_log_priority level, const char* fmt, va_list args) {
    if (!backendInUse)
        return;

    static char string[1024];
    vsnprintf(string, sizeof(string), fmt, args);

    backendInUse->log(logLevelFromLibinput(level), std::format("[libinput] {}", string));
}

// ------------ Libseat

static void libseatEnableSeat(struct libseat* seat, void* data) {
    auto PSESSION    = (Aquamarine::CSession*)data;
    PSESSION->active = true;
    if (PSESSION->libinputHandle)
        libinput_resume(PSESSION->libinputHandle);
    PSESSION->events.changeActive.emit();
}

static void libseatDisableSeat(struct libseat* seat, void* data) {
    auto PSESSION    = (Aquamarine::CSession*)data;
    PSESSION->active = false;
    if (PSESSION->libinputHandle)
        libinput_suspend(PSESSION->libinputHandle);
    PSESSION->events.changeActive.emit();
    libseat_disable_seat(PSESSION->libseatHandle);
}

static const libseat_seat_listener libseatListener = {
    .enable_seat  = ::libseatEnableSeat,
    .disable_seat = ::libseatDisableSeat,
};

//  ------------ Libinput

static int libinputOpen(const char* path, int flags, void* data) {
    auto SESSION = (CSession*)data;

    auto dev = makeShared<CSessionDevice>(SESSION->self.lock(), path);
    if (!dev->dev)
        return -1;

    SESSION->sessionDevices.emplace_back(dev);
    return dev->fd;
}

static void libinputClose(int fd, void* data) {
    auto SESSION = (CSession*)data;

    std::erase_if(SESSION->sessionDevices, [fd](const auto& dev) {
        auto toRemove = dev->fd == fd;
        if (toRemove)
            dev->events.remove.emit();
        return toRemove;
    });
}

static const libinput_interface libinputListener = {
    .open_restricted  = ::libinputOpen,
    .close_restricted = ::libinputClose,
};

// ------------

Aquamarine::CSessionDevice::CSessionDevice(Hyprutils::Memory::CSharedPointer<CSession> session_, const std::string& path_) : session(session_), path(path_) {
    deviceID = libseat_open_device(session->libseatHandle, path.c_str(), &fd);
    if (deviceID < 0) {
        session->backend->log(AQ_LOG_ERROR, std::format("libseat: Couldn't open device at {}", path_));
        return;
    }

    struct stat stat_;
    if (fstat(fd, &stat_) < 0) {
        session->backend->log(AQ_LOG_ERROR, std::format("libseat: Couldn't stat device at {}", path_));
        deviceID = -1;
        return;
    }

    dev = stat_.st_rdev;
}

Aquamarine::CSessionDevice::~CSessionDevice() {
    if (fd < 0)
        return;
    libseat_close_device(session->libseatHandle, fd);
    close(fd);
}

bool Aquamarine::CSessionDevice::supportsKMS() {
    if (deviceID < 0)
        return false;

    bool kms = drmIsKMS(fd);

    if (kms)
        session->backend->log(AQ_LOG_DEBUG, std::format("libseat: Device {} supports kms", path));
    else
        session->backend->log(AQ_LOG_DEBUG, std::format("libseat: Device {} does not support kms", path));

    return kms;
}

SP<CSessionDevice> Aquamarine::CSessionDevice::openIfKMS(SP<CSession> session_, const std::string& path_) {
    auto dev = makeShared<CSessionDevice>(session_, path_);
    if (!dev->supportsKMS())
        return nullptr;
    return dev;
}

SP<CSession> Aquamarine::CSession::attempt(Hyprutils::Memory::CSharedPointer<CBackend> backend_) {
    if (!backend_)
        return nullptr;

    auto session     = makeShared<CSession>();
    session->backend = backend_;
    session->self    = session;
    backendInUse     = backend_;

    // ------------ Libseat

    libseat_set_log_handler(libseatLog);
    libseat_set_log_level(LIBSEAT_LOG_LEVEL_INFO);

    session->libseatHandle = libseat_open_seat(&libseatListener, session.get());

    if (!session->libseatHandle) {
        session->backend->log(AQ_LOG_ERROR, "libseat: failed to open a seat");
        return nullptr;
    }

    auto seatName = libseat_seat_name(session->libseatHandle);
    if (!seatName) {
        session->backend->log(AQ_LOG_ERROR, "libseat: failed to get seat name");
        return nullptr;
    }

    session->seatName = seatName;

    // dispatch any already pending events
    session->dispatchPendingEventsAsync();

    // ----------- Udev

    session->udevHandle = udev_new();
    if (!session->udevHandle) {
        session->backend->log(AQ_LOG_ERROR, "udev: failed to create a new context");
        return nullptr;
    }

    session->udevMonitor = udev_monitor_new_from_netlink(session->udevHandle, "udev");
    if (!session->udevMonitor) {
        session->backend->log(AQ_LOG_ERROR, "udev: failed to create a new udevMonitor");
        return nullptr;
    }

    udev_monitor_filter_add_match_subsystem_devtype(session->udevMonitor, "drm", nullptr);
    udev_monitor_enable_receiving(session->udevMonitor);

    // ----------- Libinput

    session->libinputHandle = libinput_udev_create_context(&libinputListener, session.get(), session->udevHandle);
    if (!session->libinputHandle) {
        session->backend->log(AQ_LOG_ERROR, "libinput: failed to create a new context");
        return nullptr;
    }

    if (libinput_udev_assign_seat(session->libinputHandle, session->seatName.c_str())) {
        session->backend->log(AQ_LOG_ERROR, "libinput: failed to assign a seat");
        return nullptr;
    }

    libinput_log_set_handler(session->libinputHandle, ::libinputLog);
    libinput_log_set_priority(session->libinputHandle, LIBINPUT_LOG_PRIORITY_DEBUG);

    return session;
}

Aquamarine::CSession::~CSession() {
    sessionDevices.clear();

    if (libinputHandle)
        libinput_unref(libinputHandle);
    if (libseatHandle)
        libseat_close_seat(libseatHandle);
    if (udevMonitor)
        udev_monitor_unref(udevMonitor);
    if (udevHandle)
        udev_unref(udevHandle);

    libseatHandle = nullptr;
    udevMonitor   = nullptr;
    udevHandle    = nullptr;
}

static bool isDRMCard(const char* sysname) {
    const char prefix[] = DRM_PRIMARY_MINOR_NAME;
    if (strncmp(sysname, prefix, strlen(prefix)) != 0)
        return false;

    for (size_t i = strlen(prefix); sysname[i] != '\0'; i++) {
        if (sysname[i] < '0' || sysname[i] > '9')
            return false;
    }

    return true;
}

void Aquamarine::CSession::onReady() {
    for (auto& d : libinputDevices) {
        if (d->keyboard)
            backend->events.newKeyboard.emit(SP<IKeyboard>(d->keyboard));
        if (d->mouse)
            backend->events.newPointer.emit(SP<IPointer>(d->mouse));
        if (d->touch)
            backend->events.newTouch.emit(SP<ITouch>(d->touch));
        if (d->switchy)
            backend->events.newSwitch.emit(SP<ITouch>(d->touch));

        // FIXME: other devices.
    }
}

void Aquamarine::CSession::dispatchUdevEvents() {
    if (!udevHandle || !udevMonitor)
        return;

    auto device = udev_monitor_receive_device(udevMonitor);

    if (!device)
        return;

    auto sysname = udev_device_get_sysname(device);
    auto devnode = udev_device_get_devnode(device);
    auto action  = udev_device_get_action(device);

    backend->log(AQ_LOG_DEBUG, std::format("udev: new udev {} event for {}", action ? action : "unknown", sysname ? sysname : "unknown"));

    if (!isDRMCard(sysname) || !action || !devnode) {
        udev_device_unref(device);
        return;
    }

    if (action == std::string{"add"})
        events.addDrmCard.emit(SAddDrmCardEvent{.path = devnode});
    else if (action == std::string{"change"} || action == std::string{"remove"}) {
        dev_t deviceNum = udev_device_get_devnum(device);

        for (auto& d : sessionDevices) {
            if (d->dev != deviceNum)
                continue;

            if (action == std::string{"change"}) {
                backend->log(AQ_LOG_DEBUG, std::format("udev: DRM device {} changed", sysname ? sysname : "unknown"));

                CSessionDevice::SChangeEvent event;

                auto                         prop = udev_device_get_property_value(device, "HOTPLUG");
                if (prop && prop == std::string{"1"}) {
                    event.type = CSessionDevice::AQ_SESSION_EVENT_CHANGE_HOTPLUG;

                    prop = udev_device_get_property_value(device, "CONNECTOR");
                    if (prop)
                        event.hotplug.connectorID = std::stoull(prop);

                    prop = udev_device_get_property_value(device, "PROPERTY");
                    if (prop)
                        event.hotplug.propID = std::stoull(prop);
                } else if (prop = udev_device_get_property_value(device, "LEASE"); prop && prop == std::string{"1"}) {
                    event.type = CSessionDevice::AQ_SESSION_EVENT_CHANGE_LEASE;
                } else {
                    backend->log(AQ_LOG_DEBUG, std::format("udev: DRM device {} change event unrecognized", sysname ? sysname : "unknown"));
                    break;
                }

                d->events.change.emit(event);
            } else if (action == std::string{"remove"}) {
                backend->log(AQ_LOG_DEBUG, std::format("udev: DRM device {} removed", sysname ? sysname : "unknown"));
                d->events.remove.emit();
            }

            break;
        }
    }

    udev_device_unref(device);
    return;
}

void Aquamarine::CSession::dispatchLibinputEvents() {
    if (!libinputHandle)
        return;

    if (int ret = libinput_dispatch(libinputHandle); ret) {
        backend->log(AQ_LOG_ERROR, std::format("Couldn't dispatch libinput events: {}", strerror(-ret)));
        return;
    }

    libinput_event* event = libinput_get_event(libinputHandle);
    while (event) {
        handleLibinputEvent(event);
        libinput_event_destroy(event);
        event = libinput_get_event(libinputHandle);
    }
}

void Aquamarine::CSession::dispatchLibseatEvents() {
    if (libseat_dispatch(libseatHandle, 0) == -1)
        backend->log(AQ_LOG_ERROR, "Couldn't dispatch libseat events");
}

void Aquamarine::CSession::dispatchPendingEventsAsync() {
    dispatchLibseatEvents();
    dispatchUdevEvents();
    dispatchLibinputEvents();
}

std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>> Aquamarine::CSession::pollFDs() {
    // clang-format off
    return {
        makeShared<SPollFD>(libseat_get_fd(libseatHandle), [this](){    dispatchLibseatEvents();  }),
        makeShared<SPollFD>(udev_monitor_get_fd(udevMonitor), [this](){ dispatchUdevEvents();     }),
        makeShared<SPollFD>(libinput_get_fd(libinputHandle), [this](){  dispatchLibinputEvents(); })
    };
    // clang-format on
}

bool Aquamarine::CSession::switchVT(uint32_t vt) {
    return libseat_switch_session(libseatHandle, vt) == 0;
}

void Aquamarine::CSession::handleLibinputEvent(libinput_event* e) {
    auto device    = libinput_event_get_device(e);
    auto eventType = libinput_event_get_type(e);
    auto data      = libinput_device_get_user_data(device);

    if (!data && eventType != LIBINPUT_EVENT_DEVICE_ADDED) {
        backend->log(AQ_LOG_ERROR, "libinput: No aq device in event and not added");
        return;
    }

    if (!data) {
        auto dev  = libinputDevices.emplace_back(makeShared<CLibinputDevice>(device, self));
        dev->self = dev;
        dev->init();
        return;
    }

    auto hlDevice = ((CLibinputDevice*)data)->self.lock();

    switch (eventType) {
        case LIBINPUT_EVENT_DEVICE_ADDED:
            /* shouldn't happen */
            break;
        case LIBINPUT_EVENT_DEVICE_REMOVED:
            std::erase_if(libinputDevices, [device](const auto& d) { return d->device == device; });
            break;

            // --------- keyboard

        case LIBINPUT_EVENT_KEYBOARD_KEY: {
            auto kbe = libinput_event_get_keyboard_event(e);
            hlDevice->keyboard->events.key.emit(IKeyboard::SKeyEvent{
                .timeMs  = (uint32_t)(libinput_event_keyboard_get_time_usec(kbe) / 1000),
                .key     = libinput_event_keyboard_get_key(kbe),
                .pressed = libinput_event_keyboard_get_key_state(kbe) == LIBINPUT_KEY_STATE_PRESSED,
            });
            break;
        }

            // --------- pointer

        case LIBINPUT_EVENT_POINTER_MOTION: {
            auto pe = libinput_event_get_pointer_event(e);
            hlDevice->mouse->events.move.emit(IPointer::SMoveEvent{
                .timeMs  = (uint32_t)(libinput_event_pointer_get_time_usec(pe) / 1000),
                .delta   = {libinput_event_pointer_get_dx(pe), libinput_event_pointer_get_dy(pe)},
                .unaccel = {libinput_event_pointer_get_dx_unaccelerated(pe), libinput_event_pointer_get_dy_unaccelerated(pe)},
            });
            hlDevice->mouse->events.frame.emit();
            break;
        }

        case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE: {
            auto pe = libinput_event_get_pointer_event(e);
            hlDevice->mouse->events.warp.emit(IPointer::SWarpEvent{
                .timeMs   = (uint32_t)(libinput_event_pointer_get_time_usec(pe) / 1000),
                .absolute = {libinput_event_pointer_get_absolute_x_transformed(pe, 1), libinput_event_pointer_get_absolute_y_transformed(pe, 1)},
            });
            hlDevice->mouse->events.frame.emit();
            break;
        }

        case LIBINPUT_EVENT_POINTER_BUTTON: {
            auto       pe        = libinput_event_get_pointer_event(e);
            const auto SEATCOUNT = libinput_event_pointer_get_seat_button_count(pe);
            const bool PRESSED   = libinput_event_pointer_get_button_state(pe) == LIBINPUT_BUTTON_STATE_PRESSED;

            if ((PRESSED && SEATCOUNT != 1) || (!PRESSED && SEATCOUNT != 0))
                break;

            hlDevice->mouse->events.button.emit(IPointer::SButtonEvent{
                .timeMs  = (uint32_t)(libinput_event_pointer_get_time_usec(pe) / 1000),
                .button  = libinput_event_pointer_get_button(pe),
                .pressed = PRESSED,
            });
            hlDevice->mouse->events.frame.emit();
            break;
        }

        case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
        case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
        case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS: {
            auto                 pe = libinput_event_get_pointer_event(e);

            IPointer::SAxisEvent aqe = {
                .timeMs = (uint32_t)(libinput_event_pointer_get_time_usec(pe) / 1000),
            };

            switch (eventType) {
                case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL: aqe.source = IPointer::AQ_POINTER_AXIS_SOURCE_WHEEL;
                case LIBINPUT_EVENT_POINTER_SCROLL_FINGER: aqe.source = IPointer::AQ_POINTER_AXIS_SOURCE_FINGER; break;
                case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS: aqe.source = IPointer::AQ_POINTER_AXIS_SOURCE_CONTINUOUS; break;
                default: break; /* unreachable */
            }

            static const std::array<libinput_pointer_axis, 2> LAXES = {
                LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL,
                LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL,
            };

            for (auto& axis : LAXES) {
                if (!libinput_event_pointer_has_axis(pe, axis))
                    continue;

                aqe.axis      = axis == LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL ? IPointer::AQ_POINTER_AXIS_VERTICAL : IPointer::AQ_POINTER_AXIS_HORIZONTAL;
                aqe.delta     = libinput_event_pointer_get_axis_value(pe, axis);
                aqe.direction = IPointer::AQ_POINTER_AXIS_RELATIVE_IDENTICAL;
                if (libinput_device_config_scroll_get_natural_scroll_enabled(device))
                    aqe.direction = IPointer::AQ_POINTER_AXIS_RELATIVE_INVERTED;

                if (aqe.source == IPointer::AQ_POINTER_AXIS_SOURCE_WHEEL)
                    aqe.discrete = libinput_event_pointer_get_scroll_value_v120(pe, axis);

                hlDevice->mouse->events.axis.emit(aqe);
            }

            hlDevice->mouse->events.frame.emit();
            break;
        }

        case LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN: {
            auto ge = libinput_event_get_gesture_event(e);
            hlDevice->mouse->events.swipeBegin.emit(IPointer::SSwipeBeginEvent{
                .timeMs  = (uint32_t)(libinput_event_gesture_get_time_usec(ge) / 1000),
                .fingers = (uint32_t)libinput_event_gesture_get_finger_count(ge),
            });
            break;
        }
        case LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE: {
            auto ge = libinput_event_get_gesture_event(e);
            hlDevice->mouse->events.swipeUpdate.emit(IPointer::SSwipeUpdateEvent{
                .timeMs  = (uint32_t)(libinput_event_gesture_get_time_usec(ge) / 1000),
                .fingers = (uint32_t)libinput_event_gesture_get_finger_count(ge),
                .delta   = {libinput_event_gesture_get_dx(ge), libinput_event_gesture_get_dy(ge)},
            });
            break;
        }
        case LIBINPUT_EVENT_GESTURE_SWIPE_END: {
            auto ge = libinput_event_get_gesture_event(e);
            hlDevice->mouse->events.swipeEnd.emit(IPointer::SSwipeEndEvent{
                .timeMs    = (uint32_t)(libinput_event_gesture_get_time_usec(ge) / 1000),
                .cancelled = (bool)libinput_event_gesture_get_cancelled(ge),
            });
            break;
        }

        case LIBINPUT_EVENT_GESTURE_PINCH_BEGIN: {
            auto ge = libinput_event_get_gesture_event(e);
            hlDevice->mouse->events.pinchBegin.emit(IPointer::SPinchBeginEvent{
                .timeMs  = (uint32_t)(libinput_event_gesture_get_time_usec(ge) / 1000),
                .fingers = (uint32_t)libinput_event_gesture_get_finger_count(ge),
            });
            break;
        }
        case LIBINPUT_EVENT_GESTURE_PINCH_UPDATE: {
            auto ge = libinput_event_get_gesture_event(e);
            hlDevice->mouse->events.pinchUpdate.emit(IPointer::SPinchUpdateEvent{
                .timeMs   = (uint32_t)(libinput_event_gesture_get_time_usec(ge) / 1000),
                .fingers  = (uint32_t)libinput_event_gesture_get_finger_count(ge),
                .delta    = {libinput_event_gesture_get_dx(ge), libinput_event_gesture_get_dy(ge)},
                .scale    = libinput_event_gesture_get_scale(ge),
                .rotation = libinput_event_gesture_get_angle_delta(ge),
            });
            break;
        }
        case LIBINPUT_EVENT_GESTURE_PINCH_END: {
            auto ge = libinput_event_get_gesture_event(e);
            hlDevice->mouse->events.pinchEnd.emit(IPointer::SPinchEndEvent{
                .timeMs    = (uint32_t)(libinput_event_gesture_get_time_usec(ge) / 1000),
                .cancelled = (bool)libinput_event_gesture_get_cancelled(ge),
            });
            break;
        }

        case LIBINPUT_EVENT_GESTURE_HOLD_BEGIN: {
            auto ge = libinput_event_get_gesture_event(e);
            hlDevice->mouse->events.holdBegin.emit(IPointer::SHoldBeginEvent{
                .timeMs  = (uint32_t)(libinput_event_gesture_get_time_usec(ge) / 1000),
                .fingers = (uint32_t)libinput_event_gesture_get_finger_count(ge),
            });
            break;
        }
        case LIBINPUT_EVENT_GESTURE_HOLD_END: {
            auto ge = libinput_event_get_gesture_event(e);
            hlDevice->mouse->events.holdEnd.emit(IPointer::SHoldEndEvent{
                .timeMs    = (uint32_t)(libinput_event_gesture_get_time_usec(ge) / 1000),
                .cancelled = (bool)libinput_event_gesture_get_cancelled(ge),
            });
            break;
        }

            // --------- touch

        case LIBINPUT_EVENT_TOUCH_DOWN: {
            auto te = libinput_event_get_touch_event(e);
            hlDevice->touch->events.down.emit(ITouch::SDownEvent{
                .timeMs  = (uint32_t)(libinput_event_touch_get_time_usec(te) / 1000),
                .touchID = libinput_event_touch_get_seat_slot(te),
            });
            break;
        }
        case LIBINPUT_EVENT_TOUCH_UP: {
            auto te = libinput_event_get_touch_event(e);
            hlDevice->touch->events.up.emit(ITouch::SUpEvent{
                .timeMs  = (uint32_t)(libinput_event_touch_get_time_usec(te) / 1000),
                .touchID = libinput_event_touch_get_seat_slot(te),
            });
            break;
        }
        case LIBINPUT_EVENT_TOUCH_MOTION: {
            auto te = libinput_event_get_touch_event(e);
            hlDevice->touch->events.move.emit(ITouch::SMotionEvent{
                .timeMs  = (uint32_t)(libinput_event_touch_get_time_usec(te) / 1000),
                .touchID = libinput_event_touch_get_seat_slot(te),
                .pos     = {libinput_event_touch_get_x_transformed(te, 1), libinput_event_touch_get_y_transformed(te, 1)},
            });
            break;
        }
        case LIBINPUT_EVENT_TOUCH_CANCEL: {
            auto te = libinput_event_get_touch_event(e);
            hlDevice->touch->events.cancel.emit(ITouch::SCancelEvent{
                .timeMs  = (uint32_t)(libinput_event_touch_get_time_usec(te) / 1000),
                .touchID = libinput_event_touch_get_seat_slot(te),
            });
            break;
        }
        case LIBINPUT_EVENT_TOUCH_FRAME: {
            auto te = libinput_event_get_touch_event(e);
            hlDevice->touch->events.frame.emit();
            break;
        }

            // --------- switch

        case LIBINPUT_EVENT_SWITCH_TOGGLE: {
            auto       se = libinput_event_get_switch_event(e);

            const bool ENABLED = libinput_event_switch_get_switch_state(se) == LIBINPUT_SWITCH_STATE_ON;

            if (ENABLED == hlDevice->switchy->state)
                return;

            switch (libinput_event_switch_get_switch(se)) {
                case LIBINPUT_SWITCH_LID: hlDevice->switchy->type = ISwitch::AQ_SWITCH_TYPE_LID; break;
                case LIBINPUT_SWITCH_TABLET_MODE: hlDevice->switchy->type = ISwitch::AQ_SWITCH_TYPE_TABLET_MODE; break;
            }

            hlDevice->switchy->events.fire.emit(ISwitch::SFireEvent{
                .timeMs = (uint32_t)(libinput_event_switch_get_time_usec(se) / 1000),
                .type   = hlDevice->switchy->type,
                .enable = ENABLED,
            });
            break;
        }

            // --------- tbalet



            // FIXME: other events

        default: break;
    }
}

Aquamarine::CLibinputDevice::CLibinputDevice(libinput_device* device_, Hyprutils::Memory::CWeakPointer<CSession> session_) : device(device_), session(session_) {
    ;
}

void Aquamarine::CLibinputDevice::init() {
    const auto VENDOR  = libinput_device_get_id_vendor(device);
    const auto PRODUCT = libinput_device_get_id_product(device);
    const auto NAME    = libinput_device_get_name(device);

    session->backend->log(AQ_LOG_DEBUG, std::format("libinput: New device {}: {}-{}", NAME ? NAME : "Unknown", VENDOR, PRODUCT));

    name = NAME;

    libinput_device_ref(device);
    libinput_device_set_user_data(device, this);

    if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_KEYBOARD)) {
        keyboard = makeShared<CLibinputKeyboard>(self.lock());
        if (session->backend->ready)
            session->backend->events.newKeyboard.emit(SP<IKeyboard>(keyboard));
    }

    if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_POINTER)) {
        mouse = makeShared<CLibinputMouse>(self.lock());
        if (session->backend->ready)
            session->backend->events.newPointer.emit(SP<IPointer>(mouse));
    }

    if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_TOUCH)) {
        touch = makeShared<CLibinputTouch>(self.lock());
        if (session->backend->ready)
            session->backend->events.newTouch.emit(SP<ITouch>(touch));
    }

    if (libinput_device_has_capability(device, LIBINPUT_DEVICE_CAP_SWITCH)) {
        switchy = makeShared<CLibinputSwitch>(self.lock());
        if (session->backend->ready)
            session->backend->events.newSwitch.emit(SP<ISwitch>(switchy));
    }

    // FIXME: other devices
}

Aquamarine::CLibinputDevice::~CLibinputDevice() {
    libinput_device_unref(device);
}

Aquamarine::CLibinputKeyboard::CLibinputKeyboard(SP<CLibinputDevice> dev) : device(dev) {}

libinput_device* Aquamarine::CLibinputKeyboard::getLibinputHandle() {
    if (!device)
        return nullptr;
    return device->device;
}

const std::string& Aquamarine::CLibinputKeyboard::getName() {
    if (!device)
        return AQ_UNKNOWN_DEVICE_NAME;
    return device->name;
}

void Aquamarine::CLibinputKeyboard::updateLEDs(uint32_t leds) {
    ; // FIXME:
}

Aquamarine::CLibinputMouse::CLibinputMouse(Hyprutils::Memory::CSharedPointer<CLibinputDevice> dev) : device(dev) {
    ;
}

libinput_device* Aquamarine::CLibinputMouse::getLibinputHandle() {
    if (!device)
        return nullptr;
    return device->device;
}

const std::string& Aquamarine::CLibinputMouse::getName() {
    if (!device)
        return AQ_UNKNOWN_DEVICE_NAME;
    return device->name;
}

Aquamarine::CLibinputTouch::CLibinputTouch(Hyprutils::Memory::CSharedPointer<CLibinputDevice> dev) : device(dev) {
    double w = 0, h = 0;
    libinput_device_get_size(dev->device, &w, &h);
    physicalSize = {w, h};
}

libinput_device* Aquamarine::CLibinputTouch::getLibinputHandle() {
    if (!device)
        return nullptr;
    return device->device;
}

const std::string& Aquamarine::CLibinputTouch::getName() {
    if (!device)
        return AQ_UNKNOWN_DEVICE_NAME;
    return device->name;
}

Aquamarine::CLibinputSwitch::CLibinputSwitch(Hyprutils::Memory::CSharedPointer<CLibinputDevice> dev) : device(dev) {
    ;
}

libinput_device* Aquamarine::CLibinputSwitch::getLibinputHandle() {
    if (!device)
        return nullptr;
    return device->device;
}

const std::string& Aquamarine::CLibinputSwitch::getName() {
    if (!device)
        return AQ_UNKNOWN_DEVICE_NAME;
    return device->name;
}