#include <aquamarine/backend/Backend.hpp>

extern "C" {
#include <libseat.h>
#include <libudev.h>
#include <cstring>
#include <xf86drm.h>
#include <sys/stat.h>
#include <xf86drmMode.h>
}

using namespace Aquamarine;
using namespace Hyprutils::Memory;
#define SP CSharedPointer

// we can't really do better with libseat logs
// because they don't allow us to pass "data" or anything...
// Nobody should create multiple backends anyways really
Hyprutils::Memory::CSharedPointer<CBackend> backendInUse;

static Aquamarine::eBackendLogLevel         logLevelFromLibseat(enum libseat_log_level level) {
    switch (level) {
        case LIBSEAT_LOG_LEVEL_ERROR: return AQ_LOG_ERROR;
        case LIBSEAT_LOG_LEVEL_SILENT: return AQ_LOG_TRACE;
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

static void libseatEnableSeat(struct libseat* seat, void* data) {
    auto PSESSION    = (Aquamarine::CSession*)data;
    PSESSION->active = true;
    PSESSION->events.changeActive.emit();
}

static void libseatDisableSeat(struct libseat* seat, void* data) {
    auto PSESSION    = (Aquamarine::CSession*)data;
    PSESSION->active = false;
    PSESSION->events.changeActive.emit();
    libseat_disable_seat(PSESSION->libseatHandle);
}

static const libseat_seat_listener libseatListener = {
    .enable_seat  = libseatEnableSeat,
    .disable_seat = libseatDisableSeat,
};

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

    return session;
}

Aquamarine::CSession::~CSession() {
    sessionDevices.clear();

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

void Aquamarine::CSession::dispatchUdevEvents() {
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

void Aquamarine::CSession::dispatchPendingEventsAsync() {
    if (libseat_dispatch(libseatHandle, 0) == -1)
        backend->log(AQ_LOG_ERROR, "Couldn't dispatch libseat events");

    dispatchUdevEvents();
}

std::vector<int> Aquamarine::CSession::pollFDs() {
    if (!libseatHandle || !udevMonitor || !udevHandle)
        return {};

    return {libseat_get_fd(libseatHandle), udev_monitor_get_fd(udevMonitor)};
}
