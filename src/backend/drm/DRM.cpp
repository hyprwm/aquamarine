#include <aquamarine/backend/DRM.hpp>
#include <aquamarine/backend/drm/Legacy.hpp>
#include <aquamarine/backend/drm/Atomic.hpp>
#include <hyprutils/string/VarList.hpp>
#include <chrono>
#include <thread>
#include <deque>
#include <cstring>
#include <sys/mman.h>

extern "C" {
#include <libseat.h>
#include <libudev.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdisplay-info/cvt.h>
}

#include "Props.hpp"
#include "FormatUtils.hpp"
#include "Shared.hpp"

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
#define SP CSharedPointer

Aquamarine::CDRMBackend::CDRMBackend(SP<CBackend> backend_) : backend(backend_) {
    listeners.sessionActivate = backend->session->events.changeActive.registerListener([this](std::any d) {
        if (backend->session->active) {
            // session got activated, we need to restore
            restoreAfterVT();
        }
    });
}

static udev_enumerate* enumDRMCards(udev* udev) {
    auto enumerate = udev_enumerate_new(udev);
    if (!enumerate)
        return nullptr;

    udev_enumerate_add_match_subsystem(enumerate, "drm");
    udev_enumerate_add_match_sysname(enumerate, DRM_PRIMARY_MINOR_NAME "[0-9]*");

    if (udev_enumerate_scan_devices(enumerate)) {
        udev_enumerate_unref(enumerate);
        return nullptr;
    }

    return enumerate;
}

static std::vector<SP<CSessionDevice>> scanGPUs(SP<CBackend> backend) {
    auto enumerate = enumDRMCards(backend->session->udevHandle);

    if (!enumerate) {
        backend->log(AQ_LOG_ERROR, "drm: couldn't enumerate gpus with udev");
        return {};
    }

    if (!udev_enumerate_get_list_entry(enumerate)) {
        // TODO: wait for them.
        backend->log(AQ_LOG_ERROR, "drm: No gpus in scanGPUs.");
        return {};
    }

    udev_list_entry*               entry = nullptr;
    size_t                         i     = 0;

    std::deque<SP<CSessionDevice>> devices;

    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerate)) {
        auto path   = udev_list_entry_get_name(entry);
        auto device = udev_device_new_from_syspath(backend->session->udevHandle, path);
        if (!device) {
            backend->log(AQ_LOG_WARNING, std::format("drm: Skipping device {}", path ? path : "unknown"));
            continue;
        }

        backend->log(AQ_LOG_DEBUG, std::format("drm: Enumerated device {}", path ? path : "unknown"));

        auto seat = udev_device_get_property_value(device, "ID_SEAT");
        if (!seat)
            seat = "seat0";

        if (!backend->session->seatName.empty() && backend->session->seatName != seat) {
            backend->log(AQ_LOG_WARNING, std::format("drm: Skipping device {} because seat {} doesn't match our {}", path ? path : "unknown", seat, backend->session->seatName));
            udev_device_unref(device);
            continue;
        }

        auto pciDevice = udev_device_get_parent_with_subsystem_devtype(device, "pci", nullptr);
        bool isBootVGA = false;
        if (pciDevice) {
            auto id   = udev_device_get_sysattr_value(pciDevice, "boot_vga");
            isBootVGA = id && id == std::string{"1"};
        }

        if (!udev_device_get_devnode(device)) {
            backend->log(AQ_LOG_ERROR, std::format("drm: Skipping device {}, no devnode", path ? path : "unknown"));
            udev_device_unref(device);
            continue;
        }

        auto sessionDevice = CSessionDevice::openIfKMS(backend->session, udev_device_get_devnode(device));
        if (!sessionDevice) {
            backend->log(AQ_LOG_ERROR, std::format("drm: Skipping device {}, not a KMS device", path ? path : "unknown"));
            udev_device_unref(device);
            continue;
        }

        udev_device_unref(device);

        if (isBootVGA)
            devices.push_front(sessionDevice);
        else
            devices.push_back(sessionDevice);

        ++i;
    }

    udev_enumerate_unref(enumerate);

    std::vector<SP<CSessionDevice>> vecDevices;

    auto                            explicitGpus = getenv("AQ_DRM_DEVICES");
    if (explicitGpus) {
        backend->log(AQ_LOG_DEBUG, std::format("drm: Explicit device list {}", explicitGpus));
        Hyprutils::String::CVarList explicitDevices(explicitGpus, ':', true);

        for (auto& d : explicitDevices) {
            bool found = false;
            for (auto& vd : devices) {
                if (vd->path == d) {
                    vecDevices.emplace_back(vd);
                    found = true;
                    break;
                }
            }

            if (found)
                backend->log(AQ_LOG_DEBUG, std::format("drm: Explicit device {} found", d));
            else
                backend->log(AQ_LOG_ERROR, std::format("drm: Explicit device {} not found", d));
        }
    } else {
        for (auto& d : devices) {
            vecDevices.push_back(d);
        }
    }

    return vecDevices;
}

std::vector<SP<CDRMBackend>> Aquamarine::CDRMBackend::attempt(SP<CBackend> backend) {
    if (!backend->session)
        backend->session = CSession::attempt(backend);

    if (!backend->session) {
        backend->log(AQ_LOG_ERROR, "Failed to open a session");
        return {};
    }

    if (!backend->session->active) {
        backend->log(AQ_LOG_DEBUG, "Session is not active, waiting for 5s");

        auto started = std::chrono::system_clock::now();

        while (!backend->session->active) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            backend->session->dispatchPendingEventsAsync();

            if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now() - started).count() >= 5000) {
                backend->log(AQ_LOG_DEBUG, "Session timeout reached");
                break;
            }
        }

        if (!backend->session->active) {
            backend->log(AQ_LOG_DEBUG, "Session could not be activated in time");
            return {};
        }
    }

    auto gpus = scanGPUs(backend);

    if (gpus.empty()) {
        backend->log(AQ_LOG_ERROR, "drm: Found no gpus to use, cannot continue");
        return {};
    }

    backend->log(AQ_LOG_DEBUG, std::format("drm: Found {} GPUs", gpus.size()));

    std::vector<SP<CDRMBackend>> backends;
    SP<CDRMBackend>              newPrimary;

    for (auto& gpu : gpus) {
        auto drmBackend  = SP<CDRMBackend>(new CDRMBackend(backend));
        drmBackend->self = drmBackend;

        if (!drmBackend->registerGPU(gpu, newPrimary)) {
            backend->log(AQ_LOG_ERROR, std::format("drm: Failed to register gpu {}", gpu->path));
            continue;
        } else
            backend->log(AQ_LOG_DEBUG, std::format("drm: Registered gpu {}", gpu->path));

        // TODO: consider listening for new devices
        // But if you expect me to handle gpu hotswaps you are probably insane LOL

        if (!drmBackend->checkFeatures()) {
            backend->log(AQ_LOG_ERROR, "drm: Failed checking features");
            continue;
        }

        if (!drmBackend->initResources()) {
            backend->log(AQ_LOG_ERROR, "drm: Failed initializing resources");
            continue;
        }

        backend->log(AQ_LOG_DEBUG, std::format("drm: Basic init pass for gpu {}", gpu->path));

        drmBackend->grabFormats();

        drmBackend->scanConnectors();

        if (!newPrimary) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: gpu {} becomes primary drm", gpu->path));
            newPrimary = drmBackend;
        }

        backends.emplace_back(drmBackend);
    }

    return backends;
}

Aquamarine::CDRMBackend::~CDRMBackend() {
    ;
}

void Aquamarine::CDRMBackend::log(eBackendLogLevel l, const std::string& s) {
    backend->log(l, s);
}

bool Aquamarine::CDRMBackend::sessionActive() {
    return backend->session->active;
}

void Aquamarine::CDRMBackend::restoreAfterVT() {
    backend->log(AQ_LOG_DEBUG, "drm: Restoring after VT switch");

    scanConnectors();

    backend->log(AQ_LOG_DEBUG, "drm: Rescanned connectors");

    if (!impl->reset())
        backend->log(AQ_LOG_ERROR, "drm: failed reset");

    for (auto& c : connectors) {
        if (!c->crtc)
            continue;

        SDRMConnectorCommitData data = {
            .mainFB   = nullptr,
            .modeset  = true,
            .blocking = true,
            .flags    = 0,
            .test     = false,
        };

        if (c->output->state->state().mode && c->output->state->state().mode->modeInfo.has_value())
            data.modeInfo = *c->output->state->state().mode->modeInfo;
        else
            data.calculateMode(c);

        backend->log(AQ_LOG_DEBUG,
                     std::format("drm: Restoring crtc {} with clock {} hdisplay {} vdisplay {} vrefresh {}", c->crtc->id, data.modeInfo.clock, data.modeInfo.hdisplay,
                                 data.modeInfo.vdisplay, data.modeInfo.vrefresh));

        if (!impl->commit(c, data))
            backend->log(AQ_LOG_ERROR, std::format("drm: crtc {} failed restore", c->crtc->id));
    }
}

bool Aquamarine::CDRMBackend::checkFeatures() {
    uint64_t curW = 0, curH = 0;
    if (drmGetCap(gpu->fd, DRM_CAP_CURSOR_WIDTH, &curW))
        curW = 64;
    if (drmGetCap(gpu->fd, DRM_CAP_CURSOR_HEIGHT, &curH))
        curH = 64;

    drmProps.cursorSize = Hyprutils::Math::Vector2D{(double)curW, (double)curH};

    uint64_t cap = 0;
    if (drmGetCap(gpu->fd, DRM_CAP_PRIME, &cap) || !(cap & DRM_PRIME_CAP_IMPORT)) {
        backend->log(AQ_LOG_ERROR, std::format("drm: DRM_PRIME_CAP_IMPORT unsupported"));
        return false;
    }

    if (drmGetCap(gpu->fd, DRM_CAP_CRTC_IN_VBLANK_EVENT, &cap) || !cap) {
        backend->log(AQ_LOG_ERROR, std::format("drm: DRM_CAP_CRTC_IN_VBLANK_EVENT unsupported"));
        return false;
    }

    if (drmGetCap(gpu->fd, DRM_CAP_TIMESTAMP_MONOTONIC, &cap) || !cap) {
        backend->log(AQ_LOG_ERROR, std::format("drm: DRM_PRIME_CAP_IMPORT unsupported"));
        return false;
    }

    if (drmSetClientCap(gpu->fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1)) {
        backend->log(AQ_LOG_ERROR, std::format("drm: DRM_CLIENT_CAP_UNIVERSAL_PLANES unsupported"));
        return false;
    }

    drmProps.supportsAsyncCommit     = drmGetCap(gpu->fd, DRM_CAP_ASYNC_PAGE_FLIP, &cap) == 0 && cap == 1;
    drmProps.supportsAddFb2Modifiers = drmGetCap(gpu->fd, DRM_CAP_ADDFB2_MODIFIERS, &cap) == 0 && cap == 1;

    // FIXME: move to NO_ATOMIC when this piece of shit works
    if (!envEnabled("AQ_ATOMIC")) {
        backend->log(AQ_LOG_WARNING, "drm: AQ_ATOMIC not enabled, using the legacy drm iface");
        impl = makeShared<CDRMLegacyImpl>(self.lock());
    } else if (drmSetClientCap(gpu->fd, DRM_CLIENT_CAP_ATOMIC, 1)) {
        backend->log(AQ_LOG_WARNING, "drm: failed to set DRM_CLIENT_CAP_ATOMIC, falling back to legacy");
        impl = makeShared<CDRMLegacyImpl>(self.lock());
    } else {
        backend->log(AQ_LOG_DEBUG, "drm: Atomic supported, using atomic for modesetting");
        impl                         = makeShared<CDRMAtomicImpl>(self.lock());
        drmProps.supportsAsyncCommit = drmGetCap(gpu->fd, DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP, &cap) == 0 && cap == 1;
    }

    backend->log(AQ_LOG_DEBUG, std::format("drm: drmProps.supportsAsyncCommit: {}", drmProps.supportsAsyncCommit));
    backend->log(AQ_LOG_DEBUG, std::format("drm: drmProps.supportsAddFb2Modifiers: {}", drmProps.supportsAddFb2Modifiers));

    // TODO: allow no-modifiers?

    return true;
}

bool Aquamarine::CDRMBackend::initResources() {
    auto resources = drmModeGetResources(gpu->fd);
    if (!resources) {
        backend->log(AQ_LOG_ERROR, std::format("drm: drmModeGetResources failed"));
        return false;
    }

    backend->log(AQ_LOG_DEBUG, std::format("drm: found {} CRTCs", resources->count_crtcs));

    for (size_t i = 0; i < resources->count_crtcs; ++i) {
        auto CRTC     = makeShared<SDRMCRTC>();
        CRTC->id      = resources->crtcs[i];
        CRTC->backend = self;

        auto drmCRTC = drmModeGetCrtc(gpu->fd, CRTC->id);
        if (!drmCRTC) {
            backend->log(AQ_LOG_ERROR, std::format("drm: drmModeGetCrtc for crtc {} failed", CRTC->id));
            drmModeFreeResources(resources);
            crtcs.clear();
            return false;
        }

        CRTC->legacy.gammaSize = drmCRTC->gamma_size;
        drmModeFreeCrtc(drmCRTC);

        if (!getDRMCRTCProps(gpu->fd, CRTC->id, &CRTC->props)) {
            backend->log(AQ_LOG_ERROR, std::format("drm: getDRMCRTCProps for crtc {} failed", CRTC->id));
            drmModeFreeResources(resources);
            crtcs.clear();
            return false;
        }

        crtcs.emplace_back(CRTC);
    }

    if (crtcs.size() > 32) {
        backend->log(AQ_LOG_CRITICAL, "drm: Cannot support more than 32 CRTCs");
        return false;
    }

    // initialize planes
    auto planeResources = drmModeGetPlaneResources(gpu->fd);
    if (!planeResources) {
        backend->log(AQ_LOG_ERROR, std::format("drm: drmModeGetPlaneResources failed"));
        return false;
    }

    backend->log(AQ_LOG_DEBUG, std::format("drm: found {} planes", planeResources->count_planes));

    for (uint32_t i = 0; i < planeResources->count_planes; ++i) {
        auto id    = planeResources->planes[i];
        auto plane = drmModeGetPlane(gpu->fd, id);
        if (!plane) {
            backend->log(AQ_LOG_ERROR, std::format("drm: drmModeGetPlane for plane {} failed", id));
            drmModeFreeResources(resources);
            crtcs.clear();
            planes.clear();
            return false;
        }

        auto aqPlane     = makeShared<SDRMPlane>();
        aqPlane->backend = self;
        aqPlane->self    = aqPlane;
        if (!aqPlane->init((drmModePlane*)plane)) {
            backend->log(AQ_LOG_ERROR, std::format("drm: aqPlane->init for plane {} failed", id));
            drmModeFreeResources(resources);
            crtcs.clear();
            planes.clear();
            return false;
        }

        planes.emplace_back(aqPlane);

        drmModeFreePlane(plane);
    }

    drmModeFreePlaneResources(planeResources);
    drmModeFreeResources(resources);

    return true;
}

bool Aquamarine::CDRMBackend::grabFormats() {
    // FIXME: do this properly maybe?
    return true;
}

bool Aquamarine::CDRMBackend::registerGPU(SP<CSessionDevice> gpu_, SP<CDRMBackend> primary_) {
    gpu     = gpu_;
    primary = primary_;

    auto drmName = drmGetDeviceNameFromFd2(gpu->fd);
    auto drmVer  = drmGetVersion(gpu->fd);

    gpuName = drmName;

    backend->log(AQ_LOG_DEBUG,
                 std::format("drm: Starting backend for {}, with driver {}{}", drmName ? drmName : "unknown", drmVer->name ? drmVer->name : "unknown",
                             (primary ? std::format(" with primary {}", primary->gpu->path) : "")));

    drmFreeVersion(drmVer);

    listeners.gpuChange = gpu->events.change.registerListener([this](std::any d) {
        auto E = std::any_cast<CSessionDevice::SChangeEvent>(d);
        if (E.type == CSessionDevice::AQ_SESSION_EVENT_CHANGE_HOTPLUG) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: Got a hotplug event for {}", gpuName));
            scanConnectors();
        }
    });

    listeners.gpuRemove = gpu->events.remove.registerListener(
        [this](std::any d) { backend->log(AQ_LOG_ERROR, std::format("drm: !!!!FIXME: Got a remove event for {}, this is not handled properly!!!!!", gpuName)); });

    return true;
}

eBackendType Aquamarine::CDRMBackend::type() {
    return eBackendType::AQ_BACKEND_DRM;
}

void Aquamarine::CDRMBackend::scanConnectors() {
    backend->log(AQ_LOG_DEBUG, std::format("drm: Scanning connectors for {}", gpu->path));

    auto resources = drmModeGetResources(gpu->fd);
    if (!resources) {
        backend->log(AQ_LOG_ERROR, std::format("drm: Scanning connectors for {} failed", gpu->path));
        return;
    }

    for (size_t i = 0; i < resources->count_connectors; ++i) {
        uint32_t          connectorID = resources->connectors[i];

        SP<SDRMConnector> conn;
        auto              drmConn = drmModeGetConnector(gpu->fd, connectorID);

        backend->log(AQ_LOG_DEBUG, std::format("drm: Scanning connector id {}", connectorID));

        if (!drmConn) {
            backend->log(AQ_LOG_ERROR, std::format("drm: Failed to get connector id {}", connectorID));
            continue;
        }

        auto it = std::find_if(connectors.begin(), connectors.end(), [connectorID](const auto& e) { return e->id == connectorID; });
        if (it == connectors.end()) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: Initializing connector id {}", connectorID));
            conn          = connectors.emplace_back(SP<SDRMConnector>(new SDRMConnector()));
            conn->self    = conn;
            conn->backend = self;
            conn->id      = connectorID;
            if (!conn->init(drmConn)) {
                backend->log(AQ_LOG_ERROR, std::format("drm: Connector id {} failed initializing", connectorID));
                connectors.pop_back();
                continue;
            }
        } else {
            backend->log(AQ_LOG_DEBUG, std::format("drm: Connector id {} already initialized", connectorID));
            conn = *it;
        }

        if (!conn->crtc) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: Ignoring connector {} because it has no CRTC", connectorID));
            continue;
        }

        backend->log(AQ_LOG_DEBUG, std::format("drm: Connector {} connection state: {}", connectorID, (int)drmConn->connection));

        if (conn->status == DRM_MODE_DISCONNECTED && drmConn->connection == DRM_MODE_CONNECTED) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: Connector {} connected", conn->szName));
            conn->connect(drmConn);
        } else if (conn->status == DRM_MODE_CONNECTED && drmConn->connection == DRM_MODE_DISCONNECTED) {
            backend->log(AQ_LOG_DEBUG, std::format("drm: Connector {} disconnected", conn->szName));
            conn->disconnect();
        }

        drmModeFreeConnector(drmConn);
    }

    drmModeFreeResources(resources);
}

bool Aquamarine::CDRMBackend::start() {
    impl->reset();
    return true;
}

std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>> Aquamarine::CDRMBackend::pollFDs() {
    return {makeShared<SPollFD>(gpu->fd, [this]() { dispatchEvents(); })};
}

int Aquamarine::CDRMBackend::drmFD() {
    return gpu->fd;
}

static void handlePF(int fd, unsigned seq, unsigned tv_sec, unsigned tv_usec, unsigned crtc_id, void* data) {
    auto pageFlip = (SDRMPageFlip*)data;

    if (!pageFlip->connector)
        return;

    pageFlip->connector->isPageFlipPending = false;

    const auto& BACKEND = pageFlip->connector->backend;

    BACKEND->log(AQ_LOG_TRACE, std::format("drm: pf event seq {} sec {} usec {} crtc {}", seq, tv_sec, tv_usec, crtc_id));

    if (pageFlip->connector->status != DRM_MODE_CONNECTED || !pageFlip->connector->crtc) {
        BACKEND->log(AQ_LOG_DEBUG, "drm: Ignoring a pf event from a disabled crtc / connector");
        return;
    }

    pageFlip->connector->onPresent();

    uint32_t flags = IOutput::AQ_OUTPUT_PRESENT_VSYNC | IOutput::AQ_OUTPUT_PRESENT_HW_CLOCK | IOutput::AQ_OUTPUT_PRESENT_HW_COMPLETION | IOutput::AQ_OUTPUT_PRESENT_ZEROCOPY;

    timespec presented = {.tv_sec = tv_sec, .tv_nsec = tv_usec * 1000};

    pageFlip->connector->output->events.present.emit(IOutput::SPresentEvent{
        .presented = BACKEND->sessionActive(),
        .when      = &presented,
        .seq       = seq,
        .refresh   = (int)(pageFlip->connector->refresh ? (1000000000000LL / pageFlip->connector->refresh) : 0),
        .flags     = flags,
    });

    if (BACKEND->sessionActive() && !pageFlip->connector->frameEventScheduled)
        pageFlip->connector->output->events.frame.emit();
}

bool Aquamarine::CDRMBackend::dispatchEvents() {
    drmEventContext event = {
        .version            = 3,
        .page_flip_handler2 = ::handlePF,
    };

    if (drmHandleEvent(gpu->fd, &event) != 0)
        backend->log(AQ_LOG_ERROR, std::format("drm: Failed to handle event on fd {}", gpu->fd));

    return true;
}

uint32_t Aquamarine::CDRMBackend::capabilities() {
    return eBackendCapabilities::AQ_BACKEND_CAPABILITY_POINTER;
}

bool Aquamarine::CDRMBackend::setCursor(SP<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot) {
    return false;
}

void Aquamarine::CDRMBackend::onReady() {
    backend->log(AQ_LOG_DEBUG, std::format("drm: Connectors size2 {}", connectors.size()));

    for (auto& c : connectors) {
        backend->log(AQ_LOG_DEBUG, std::format("drm: onReady: connector {}", c->id));
        if (!c->output)
            continue;

        backend->log(AQ_LOG_DEBUG, std::format("drm: onReady: connector {} has output name {}", c->id, c->output->name));

        // swapchain has to be created here because allocator is absent in connect if not ready
        c->output->swapchain = CSwapchain::create(backend->allocator, self.lock());
        c->output->swapchain->reconfigure(SSwapchainOptions{.length = 0, .scanout = true}); // mark the swapchain for scanout
        c->output->needsFrame = true;

        backend->events.newOutput.emit(SP<IOutput>(c->output));
    }
}

std::vector<SDRMFormat> Aquamarine::CDRMBackend::getRenderFormats() {
    for (auto& p : planes) {
        if (p->type != DRM_PLANE_TYPE_PRIMARY)
            continue;

        if (primary) {
            backend->log(AQ_LOG_TRACE, std::format("drm: getRenderFormats on secondary {}", gpu->path));

            // this is a secondary GPU renderer. In order to receive buffers,
            // we'll force linear modifiers.
            // TODO: don't. Find a common maybe?
            auto fmts = p->formats;
            for (auto& fmt : fmts) {
                fmt.modifiers = {DRM_FORMAT_MOD_LINEAR};
            }
            return fmts;
        }

        return p->formats;
    }

    return {};
}

std::vector<SDRMFormat> Aquamarine::CDRMBackend::getCursorFormats() {
    for (auto& p : planes) {
        if (p->type != DRM_PLANE_TYPE_CURSOR)
            continue;

        if (primary) {
            backend->log(AQ_LOG_TRACE, std::format("drm: getCursorFormats on secondary {}", gpu->path));

            // this is a secondary GPU renderer. In order to receive buffers,
            // we'll force linear modifiers.
            // TODO: don't. Find a common maybe?
            auto fmts = p->formats;
            for (auto& fmt : fmts) {
                fmt.modifiers = {DRM_FORMAT_MOD_LINEAR};
            }
            return fmts;
        }

        return p->formats;
    }

    return {};
}

bool Aquamarine::CDRMBackend::createOutput(const std::string&) {
    return false;
}

bool Aquamarine::SDRMPlane::init(drmModePlane* plane) {
    id = plane->plane_id;

    if (!getDRMPlaneProps(backend->gpu->fd, id, &props))
        return false;

    if (!getDRMProp(backend->gpu->fd, id, props.type, &type))
        return false;

    initialID = id;

    backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Plane {} has type {}", id, (int)type));

    backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Plane {} has {} formats", id, plane->count_formats));

    for (size_t i = 0; i < plane->count_formats; ++i) {
        if (type != DRM_PLANE_TYPE_CURSOR)
            formats.emplace_back(SDRMFormat{.drmFormat = plane->formats[i], .modifiers = {DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID}});
        else
            formats.emplace_back(SDRMFormat{.drmFormat = plane->formats[i], .modifiers = {DRM_FORMAT_MOD_LINEAR}});

        backend->backend->log(AQ_LOG_TRACE, std::format("drm: | Format {}", fourccToName(plane->formats[i])));
    }

    if (props.in_formats && backend->drmProps.supportsAddFb2Modifiers) {
        backend->backend->log(AQ_LOG_DEBUG, "drm: Plane: checking for modifiers");

        uint64_t blobID = 0;
        if (!getDRMProp(backend->gpu->fd, id, props.in_formats, &blobID)) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Plane: No blob id");
            return false;
        }

        auto blob = drmModeGetPropertyBlob(backend->gpu->fd, blobID);
        if (!blob) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Plane: No property");
            return false;
        }

        drmModeFormatModifierIterator iter = {0};
        while (drmModeFormatModifierBlobIterNext(blob, &iter)) {
            auto it = std::find_if(formats.begin(), formats.end(), [iter](const auto& e) { return e.drmFormat == iter.fmt; });

            backend->backend->log(AQ_LOG_TRACE, std::format("drm: | Modifier {} with format {}", iter.mod, fourccToName(iter.fmt)));

            if (it == formats.end())
                formats.emplace_back(SDRMFormat{.drmFormat = iter.fmt, .modifiers = {iter.mod}});
            else
                it->modifiers.emplace_back(iter.mod);
        }

        drmModeFreePropertyBlob(blob);
    }

    for (size_t i = 0; i < backend->crtcs.size(); ++i) {
        uint32_t crtcBit = (1 << i);
        if (!(plane->possible_crtcs & crtcBit))
            continue;

        auto CRTC = backend->crtcs.at(i);
        if (type == DRM_PLANE_TYPE_PRIMARY && !CRTC->primary) {
            CRTC->primary = self.lock();
            break;
        }

        if (type == DRM_PLANE_TYPE_CURSOR && !CRTC->cursor) {
            CRTC->cursor = self.lock();
            break;
        }
    }

    return true;
}

SP<SDRMCRTC> Aquamarine::SDRMConnector::getCurrentCRTC(const drmModeConnector* connector) {
    uint32_t crtcID = 0;
    if (props.crtc_id) {
        backend->backend->log(AQ_LOG_TRACE, "drm: Using crtc_id for finding crtc");
        uint64_t value = 0;
        if (!getDRMProp(backend->gpu->fd, id, props.crtc_id, &value)) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Failed to get CRTC_ID");
            return nullptr;
        }
        crtcID = static_cast<uint32_t>(value);
    } else if (connector->encoder_id) {
        backend->backend->log(AQ_LOG_TRACE, "drm: Using encoder_id for finding crtc");
        auto encoder = drmModeGetEncoder(backend->gpu->fd, connector->encoder_id);
        if (!encoder) {
            backend->backend->log(AQ_LOG_ERROR, "drm: drmModeGetEncoder failed");
            return nullptr;
        }
        crtcID = encoder->crtc_id;
        drmModeFreeEncoder(encoder);
    } else {
        backend->backend->log(AQ_LOG_ERROR, "drm: Connector has neither crtc_id nor encoder_id");
        return nullptr;
    }

    if (crtcID == 0) {
        backend->backend->log(AQ_LOG_ERROR, "drm: getCurrentCRTC: No CRTC 0");
        return nullptr;
    }

    auto it = std::find_if(backend->crtcs.begin(), backend->crtcs.end(), [crtcID](const auto& e) { return e->id == crtcID; });

    if (it == backend->crtcs.end()) {
        backend->backend->log(AQ_LOG_ERROR, std::format("drm: Failed to find a CRTC with ID {}", crtcID));
        return nullptr;
    }

    return *it;
}

bool Aquamarine::SDRMConnector::init(drmModeConnector* connector) {
    pendingPageFlip.connector = self.lock();

    if (!getDRMConnectorProps(backend->gpu->fd, id, &props))
        return false;

    auto name = drmModeGetConnectorTypeName(connector->connector_type);
    if (!name)
        name = "ERROR";

    szName = std::format("{}-{}", name, connector->connector_type_id);
    backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Connector gets name {}", szName));

    possibleCrtcs = drmModeConnectorGetPossibleCrtcs(backend->gpu->fd, connector);
    if (!possibleCrtcs)
        backend->backend->log(AQ_LOG_ERROR, "drm: No CRTCs possible");

    crtc = getCurrentCRTC(connector);

    return true;
}

Aquamarine::SDRMConnector::~SDRMConnector() {
    disconnect();
}

static int32_t calculateRefresh(const drmModeModeInfo& mode) {
    int32_t refresh = (mode.clock * 1000000LL / mode.htotal + mode.vtotal / 2) / mode.vtotal;

    if (mode.flags & DRM_MODE_FLAG_INTERLACE)
        refresh *= 2;

    if (mode.flags & DRM_MODE_FLAG_DBLSCAN)
        refresh /= 2;

    if (mode.vscan > 1)
        refresh /= mode.vscan;

    return refresh;
}

drmModeModeInfo* Aquamarine::SDRMConnector::getCurrentMode() {
    if (!crtc)
        return nullptr;

    if (crtc->props.mode_id) {
        size_t size = 0;
        return (drmModeModeInfo*)getDRMPropBlob(backend->gpu->fd, crtc->id, crtc->props.mode_id, &size);
        ;
    }

    auto drmCrtc = drmModeGetCrtc(backend->gpu->fd, crtc->id);
    if (!drmCrtc)
        return nullptr;
    if (!drmCrtc->mode_valid) {
        drmModeFreeCrtc(drmCrtc);
        return nullptr;
    }

    drmModeModeInfo* modeInfo = (drmModeModeInfo*)malloc(sizeof(drmModeModeInfo));
    if (!modeInfo) {
        drmModeFreeCrtc(drmCrtc);
        return nullptr;
    }

    *modeInfo = drmCrtc->mode;
    drmModeFreeCrtc(drmCrtc);

    return modeInfo;
}

void Aquamarine::SDRMConnector::parseEDID(std::vector<uint8_t> data) {
    // TODO: libdisplay-info prolly
}

void Aquamarine::SDRMConnector::connect(drmModeConnector* connector) {
    if (output) {
        backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Not connecting connector {} because it's already connected", szName));
        return;
    }

    backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Connecting connector {}, CRTC ID {}", szName, crtc ? crtc->id : -1));

    output            = SP<CDRMOutput>(new CDRMOutput(szName, backend, self.lock()));
    output->self      = output;
    output->connector = self.lock();

    backend->backend->log(AQ_LOG_DEBUG, "drm: Dumping detected modes:");

    auto currentModeInfo = getCurrentMode();

    for (int i = 0; i < connector->count_modes; ++i) {
        auto& drmMode = connector->modes[i];

        if (drmMode.flags & DRM_MODE_FLAG_INTERLACE) {
            backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Skipping mode {} because it's interlaced", i));
            continue;
        }

        if (i == 1)
            fallbackModeInfo = drmMode;

        auto aqMode         = makeShared<SOutputMode>();
        aqMode->pixelSize   = {drmMode.hdisplay, drmMode.vdisplay};
        aqMode->refreshRate = calculateRefresh(drmMode);
        aqMode->preferred   = (drmMode.type & DRM_MODE_TYPE_PREFERRED);
        aqMode->modeInfo    = drmMode;

        output->modes.emplace_back(aqMode);

        if (currentModeInfo && std::memcmp(&drmMode, currentModeInfo, sizeof(drmModeModeInfo))) {
            output->state->setMode(aqMode);

            //uint64_t modeID = 0;
            // getDRMProp(backend->gpu->fd, crtc->id, crtc->props.mode_id, &modeID);

            crtc->refresh = calculateRefresh(drmMode);
        }

        backend->backend->log(AQ_LOG_DEBUG,
                              std::format("drm: Mode {}: {}x{}@{:.2f}Hz {}", i, (int)aqMode->pixelSize.x, (int)aqMode->pixelSize.y, aqMode->refreshRate / 1000.0,
                                          aqMode->preferred ? " (preferred)" : ""));
    }

    output->physicalSize = {(double)connector->mmWidth, (double)connector->mmHeight};

    backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Physical size {} (mm)", output->physicalSize));

    switch (connector->subpixel) {
        case DRM_MODE_SUBPIXEL_NONE: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_NONE; break;
        case DRM_MODE_SUBPIXEL_UNKNOWN: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_UNKNOWN; break;
        case DRM_MODE_SUBPIXEL_HORIZONTAL_RGB: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_HORIZONTAL_RGB; break;
        case DRM_MODE_SUBPIXEL_HORIZONTAL_BGR: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_HORIZONTAL_BGR; break;
        case DRM_MODE_SUBPIXEL_VERTICAL_RGB: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_VERTICAL_RGB; break;
        case DRM_MODE_SUBPIXEL_VERTICAL_BGR: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_VERTICAL_BGR; break;
        default: output->subpixel = eSubpixelMode::AQ_SUBPIXEL_UNKNOWN;
    }

    uint64_t prop = 0;
    if (getDRMProp(backend->gpu->fd, id, props.non_desktop, &prop)) {
        if (prop == 1)
            backend->backend->log(AQ_LOG_DEBUG, "drm: Non-desktop connector");
        output->nonDesktop = prop;
    }

    canDoVrr           = props.vrr_capable && crtc->props.vrr_enabled && !getDRMProp(backend->gpu->fd, id, props.vrr_capable, &prop) && prop;
    output->vrrCapable = canDoVrr;

    maxBpcBounds.fill(0);

    if (props.max_bpc && !introspectDRMPropRange(backend->gpu->fd, props.max_bpc, maxBpcBounds.data(), &maxBpcBounds[1]))
        backend->backend->log(AQ_LOG_ERROR, "drm: Failed to check max_bpc");

    size_t               edidLen  = 0;
    uint8_t*             edidData = (uint8_t*)getDRMPropBlob(backend->gpu->fd, id, props.edid, &edidLen);

    std::vector<uint8_t> edid{edidData, edidData + edidLen};
    parseEDID(edid);

    free(edidData);
    edid = {};

    // TODO: subconnectors

    output->make        = make;
    output->model       = model;
    output->serial      = serial;
    output->description = std::format("{} {} {} ({})", make, model, serial, szName);
    output->needsFrame  = true;

    backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Description {}", output->description));

    status = DRM_MODE_CONNECTED;

    if (!backend->backend->ready)
        return;

    output->swapchain = CSwapchain::create(backend->backend->allocator, backend->self.lock());
    backend->backend->events.newOutput.emit(output);
    output->scheduleFrame();
}

void Aquamarine::SDRMConnector::disconnect() {
    if (!output) {
        backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Not disconnecting connector {} because it's already disconnected", szName));
        return;
    }

    output->events.destroy.emit();
    output.reset();

    status = DRM_MODE_DISCONNECTED;
}

bool Aquamarine::SDRMConnector::commitState(SDRMConnectorCommitData& data) {
    const bool ok = backend->impl->commit(self.lock(), data);

    if (ok && !data.test)
        applyCommit(data);
    else
        rollbackCommit(data);

    return ok;
}

void Aquamarine::SDRMConnector::applyCommit(const SDRMConnectorCommitData& data) {
    crtc->primary->back = data.mainFB;
    if (crtc->cursor && data.cursorFB)
        crtc->cursor->back = data.cursorFB;

    pendingCursorFB.reset();

    if (output->state->state().committed & COutputState::AQ_OUTPUT_STATE_MODE)
        refresh = calculateRefresh(data.modeInfo);
}

void Aquamarine::SDRMConnector::rollbackCommit(const SDRMConnectorCommitData& data) {
    // cursors are applied regardless.
    if (crtc->cursor && data.cursorFB)
        crtc->cursor->back = data.cursorFB;

    crtc->pendingCursor.reset();
}

void Aquamarine::SDRMConnector::onPresent() {
    crtc->primary->last  = crtc->primary->front;
    crtc->primary->front = crtc->primary->back;
    if (crtc->cursor && crtc->cursor->back /* Don't shift if it hasn't updated */) {
        crtc->cursor->last  = crtc->cursor->front;
        crtc->cursor->front = crtc->cursor->back;
    }
}

Aquamarine::CDRMOutput::~CDRMOutput() {
    backend->backend->removeIdleEvent(frameIdle);
    connector->isPageFlipPending   = false;
    connector->frameEventScheduled = false;
}

bool Aquamarine::CDRMOutput::commit() {
    return commitState();
}

bool Aquamarine::CDRMOutput::test() {
    return commitState(true);
}

void Aquamarine::CDRMOutput::setCursorVisible(bool visible) {
    cursorVisible = visible;
    needsFrame    = true;
    scheduleFrame();
}

bool Aquamarine::CDRMOutput::commitState(bool onlyTest) {
    if (!backend->backend->session->active) {
        backend->backend->log(AQ_LOG_ERROR, "drm: Session inactive");
        return false;
    }

    if (!connector->crtc) {
        backend->backend->log(AQ_LOG_ERROR, "drm: No CRTC attached to output");
        return false;
    }

    const auto&    STATE     = state->state();
    const uint32_t COMMITTED = STATE.committed;

    if ((COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_ENABLED) && STATE.enabled) {
        if (!STATE.mode && STATE.customMode) {
            backend->backend->log(AQ_LOG_ERROR, "drm: No mode on enable commit");
            return false;
        }
    }

    if (STATE.adaptiveSync && !connector->canDoVrr) {
        backend->backend->log(AQ_LOG_ERROR, "drm: No Adaptive sync support for output");
        return false;
    }

    if (STATE.presentationMode == AQ_OUTPUT_PRESENTATION_IMMEDIATE && !backend->drmProps.supportsAsyncCommit) {
        backend->backend->log(AQ_LOG_ERROR, "drm: No Immediate presentation support in the backend");
        return false;
    }

    if ((COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_BUFFER) && !STATE.buffer) {
        backend->backend->log(AQ_LOG_ERROR, "drm: No buffer committed");
        return false;
    }

    if ((COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_BUFFER) && STATE.buffer->attachments.has(AQ_ATTACHMENT_DRM_KMS_UNIMPORTABLE)) {
        backend->backend->log(AQ_LOG_TRACE, "drm: Cannot commit a KMS-unimportable buffer.");
        return false;
    }

    // If we are changing the rendering format, we may need to reconfigure the output (aka modeset)
    // which may result in some glitches
    const bool NEEDS_RECONFIG = COMMITTED &
        (COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_ENABLED | COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_FORMAT |
         COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_MODE);

    const bool BLOCKING = NEEDS_RECONFIG || !(COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_BUFFER);

    const auto MODE = STATE.mode ? STATE.mode : STATE.customMode;

    uint32_t   flags = 0;

    if (!onlyTest) {
        if (NEEDS_RECONFIG) {
            if (STATE.enabled)
                backend->backend->log(AQ_LOG_DEBUG,
                                      std::format("drm: Modesetting {} with {}x{}@{:.2f}Hz", name, (int)MODE->pixelSize.x, (int)MODE->pixelSize.y, MODE->refreshRate / 1000.F));
            else
                backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Disabling output {}", name));
        }

        if ((NEEDS_RECONFIG || (COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_BUFFER)) && connector->isPageFlipPending) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Cannot commit when a page-flip is awaiting");
            return false;
        }

        if (STATE.enabled && (COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_BUFFER))
            flags |= DRM_MODE_PAGE_FLIP_EVENT;
        if (STATE.presentationMode == AQ_OUTPUT_PRESENTATION_IMMEDIATE && (COMMITTED & COutputState::eOutputStateProperties::AQ_OUTPUT_STATE_BUFFER))
            flags |= DRM_MODE_PAGE_FLIP_ASYNC;
    }

    SDRMConnectorCommitData data;

    if (STATE.buffer) {
        backend->backend->log(AQ_LOG_TRACE, "drm: Committed a buffer, updating state");

        SP<CDRMFB> drmFB;
        auto       buf   = STATE.buffer;
        bool       isNew = false;

        drmFB = CDRMFB::create(buf, backend, &isNew); // will return attachment if present

        if (!drmFB) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Buffer failed to import to KMS");
            return false;
        }

        if (!isNew && backend->primary) {
            // this is not a new buffer, and we are not on a primary GPU, which means
            // this buffer lives on the primary. We need to re-import it to update
            // the contents that have possibly (probably) changed
            drmFB->reimport();
        }

        data.mainFB = drmFB;
    }

    if (connector->crtc->pendingCursor)
        data.cursorFB = connector->crtc->pendingCursor;

    if (data.cursorFB) {
        // verify cursor format. This might be wrong on NVIDIA where linear buffers
        // fail to be created from gbm
        // TODO: add an API to detect this and request drm_dumb linear buffers. Or do something,
        // idk
        if (data.cursorFB->buffer->dmabuf().modifier == DRM_FORMAT_MOD_INVALID) {
            backend->backend->log(AQ_LOG_TRACE, "drm: Dropping invalid buffer for cursor plane");
            data.cursorFB = nullptr;
        }
    }

    data.blocking = BLOCKING;
    data.modeset  = NEEDS_RECONFIG || lastCommitNoBuffer;
    data.flags    = flags;
    data.test     = onlyTest;
    if (MODE->modeInfo.has_value())
        data.modeInfo = *MODE->modeInfo;
    else
        data.calculateMode(connector);

    bool ok = connector->commitState(data);

    if (onlyTest)
        return ok;

    events.commit.emit();
    state->onCommit();

    lastCommitNoBuffer = !data.mainFB;

    return ok;
}

SP<IBackendImplementation> Aquamarine::CDRMOutput::getBackend() {
    return backend.lock();
}

bool Aquamarine::CDRMOutput::setCursor(SP<IBuffer> buffer, const Vector2D& hotspot) {
    if (!buffer)
        setCursorVisible(false);
    else {
        cursorHotspot = hotspot;
        auto fb       = CDRMFB::create(buffer, backend);
        if (!fb) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Cursor buffer failed to import to KMS");
            return false;
        }

        backend->backend->log(AQ_LOG_DEBUG, std::format("drm: Cursor buffer imported into KMS with id {}", fb->id));

        connector->crtc->pendingCursor = fb;

        cursorVisible = true;
    }

    needsFrame = true;
    scheduleFrame();
    return true;
}

void Aquamarine::CDRMOutput::moveCursor(const Vector2D& coord) {
    cursorPos     = coord;
    cursorVisible = true;
    backend->impl->moveCursor(connector);
}

void Aquamarine::CDRMOutput::scheduleFrame() {
    if (connector->isPageFlipPending || connector->frameEventScheduled)
        return;

    connector->frameEventScheduled = true;

    backend->backend->addIdleEvent(frameIdle);
}

Vector2D Aquamarine::CDRMOutput::cursorPlaneSize() {
    return backend->drmProps.cursorSize;
}

Aquamarine::CDRMOutput::CDRMOutput(const std::string& name_, Hyprutils::Memory::CWeakPointer<CDRMBackend> backend_, SP<SDRMConnector> connector_) :
    backend(backend_), connector(connector_) {
    name = name_;

    frameIdle = makeShared<std::function<void(void)>>([this]() {
        connector->frameEventScheduled = false;
        if (connector->isPageFlipPending)
            return;
        events.frame.emit();
    });
}

SP<CDRMFB> Aquamarine::CDRMFB::create(SP<IBuffer> buffer_, Hyprutils::Memory::CWeakPointer<CDRMBackend> backend_, bool* isNew) {

    SP<CDRMFB> fb;

    if (isNew)
        *isNew = true;

    if (buffer_->attachments.has(AQ_ATTACHMENT_DRM_BUFFER)) {
        auto at = (CDRMBufferAttachment*)buffer_->attachments.get(AQ_ATTACHMENT_DRM_BUFFER).get();
        fb      = at->fb;
        backend_->log(AQ_LOG_TRACE, std::format("drm: CDRMFB: buffer has drmfb attachment with fb {:x}", (uintptr_t)fb.get()));
    }

    if (fb) {
        if (isNew)
            *isNew = false;
        return fb;
    }

    fb = SP<CDRMFB>(new CDRMFB(buffer_, backend_));

    if (!fb->id)
        return nullptr;

    buffer_->attachments.add(makeShared<CDRMBufferAttachment>(fb));

    return fb;
}

Aquamarine::CDRMFB::CDRMFB(SP<IBuffer> buffer_, Hyprutils::Memory::CWeakPointer<CDRMBackend> backend_) : buffer(buffer_), backend(backend_) {
    import();
}

void Aquamarine::CDRMFB::import() {
    auto attrs = buffer->dmabuf();
    if (!attrs.success) {
        backend->backend->log(AQ_LOG_ERROR, "drm: Buffer submitted has no dmabuf");
        return;
    }

    if (buffer->attachments.has(AQ_ATTACHMENT_DRM_KMS_UNIMPORTABLE)) {
        backend->backend->log(AQ_LOG_ERROR, "drm: Buffer submitted is unimportable");
        return;
    }

    // TODO: check format

    for (int i = 0; i < attrs.planes; ++i) {
        int ret = drmPrimeFDToHandle(backend->gpu->fd, attrs.fds.at(i), &boHandles[i]);
        if (ret) {
            backend->backend->log(AQ_LOG_ERROR, "drm: drmPrimeFDToHandle failed");
            drop();
            return;
        }

        backend->backend->log(AQ_LOG_TRACE, std::format("drm: CDRMFB: plane {} has fd {}, got handle {}", i, attrs.fds.at(i), boHandles.at(i)));
    }

    id = submitBuffer();
    if (!id) {
        backend->backend->log(AQ_LOG_ERROR, "drm: Failed to submit a buffer to KMS");
        buffer->attachments.add(makeShared<CDRMBufferUnimportable>());
        drop();
        return;
    }

    backend->backend->log(AQ_LOG_TRACE, std::format("drm: new buffer {}", id));

    // FIXME: why does this implode when it doesnt on wlroots or kwin?
    // closeHandles();
}

void Aquamarine::CDRMFB::reimport() {
    drop();
    dropped       = false;
    handlesClosed = false;
    boHandles     = {0, 0, 0, 0};

    import();
}

Aquamarine::CDRMFB::~CDRMFB() {
    drop();
}

void Aquamarine::CDRMFB::closeHandles() {
    if (handlesClosed)
        return;

    handlesClosed = true;

    std::vector<uint32_t> closed;

    for (size_t i = 0; i < 4; ++i) {
        if (boHandles.at(i) == 0)
            continue;

        bool exists = false;
        for (size_t j = 0; j < i; ++j) {
            if (boHandles.at(i) == boHandles.at(j)) {
                exists = true;
                break;
            }
        }
        if (exists)
            continue;

        if (drmCloseBufferHandle(backend->gpu->fd, boHandles.at(i)))
            backend->backend->log(AQ_LOG_ERROR, "drm: drmCloseBufferHandle failed");
    }
}

void Aquamarine::CDRMFB::drop() {
    if (dropped)
        return;

    dropped = true;

    if (!id)
        return;

    closeHandles();

    backend->backend->log(AQ_LOG_TRACE, std::format("drm: dropping buffer {}", id));

    int ret = drmModeCloseFB(backend->gpu->fd, id);
    if (ret == -EINVAL)
        ret = drmModeRmFB(backend->gpu->fd, id);

    if (ret)
        backend->backend->log(AQ_LOG_ERROR, std::format("drm: Failed to close a buffer: {}", strerror(-ret)));
}

uint32_t Aquamarine::CDRMFB::submitBuffer() {
    auto                    attrs = buffer->dmabuf();
    uint32_t                newID = 0;
    std::array<uint64_t, 4> mods  = {0, 0, 0, 0};
    for (size_t i = 0; i < attrs.planes; ++i) {
        mods[i] = attrs.modifier;
    }

    if (backend->drmProps.supportsAddFb2Modifiers && attrs.modifier != DRM_FORMAT_MOD_INVALID) {
        backend->backend->log(AQ_LOG_TRACE,
                              std::format("drm: Using drmModeAddFB2WithModifiers to import buffer into KMS: Size {} with format {} and mod {}", attrs.size,
                                          fourccToName(attrs.format), attrs.modifier));
        if (drmModeAddFB2WithModifiers(backend->gpu->fd, attrs.size.x, attrs.size.y, attrs.format, boHandles.data(), attrs.strides.data(), attrs.offsets.data(), mods.data(),
                                       &newID, DRM_MODE_FB_MODIFIERS)) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Failed to submit a buffer with drmModeAddFB2WithModifiers");
            return 0;
        }
    } else {
        if (attrs.modifier != DRM_FORMAT_MOD_INVALID && attrs.modifier != DRM_FORMAT_MOD_LINEAR) {
            backend->backend->log(AQ_LOG_ERROR, "drm: drmModeAddFB2WithModifiers unsupported and buffer has explicit modifiers");
            return 0;
        }

        backend->backend->log(
            AQ_LOG_TRACE,
            std::format("drm: Using drmModeAddFB2 to import buffer into KMS: Size {} with format {} and mod {}", attrs.size, fourccToName(attrs.format), attrs.modifier));

        if (drmModeAddFB2(backend->gpu->fd, attrs.size.x, attrs.size.y, attrs.format, boHandles.data(), attrs.strides.data(), attrs.offsets.data(), &newID, 0)) {
            backend->backend->log(AQ_LOG_ERROR, "drm: Failed to submit a buffer with drmModeAddFB2");
            return 0;
        }
    }

    return newID;
}

void Aquamarine::SDRMConnectorCommitData::calculateMode(Hyprutils::Memory::CSharedPointer<SDRMConnector> connector) {
    const auto&    STATE = connector->output->state->state();
    const auto     MODE  = STATE.mode ? STATE.mode : STATE.customMode;

    di_cvt_options options = {
        .red_blank_ver = DI_CVT_REDUCED_BLANKING_NONE,
        .h_pixels      = (int)MODE->pixelSize.x,
        .v_lines       = (int)MODE->pixelSize.y,
        .ip_freq_rqd   = MODE->refreshRate ? MODE->refreshRate / 1000.0 : 60.0,
    };
    di_cvt_timing timing;

    di_cvt_compute(&timing, &options);

    uint16_t hsync_start = (int)MODE->pixelSize.y + timing.h_front_porch;
    uint16_t vsync_start = timing.v_lines_rnd + timing.v_front_porch;
    uint16_t hsync_end   = hsync_start + timing.h_sync;
    uint16_t vsync_end   = vsync_start + timing.v_sync;

    modeInfo = (drmModeModeInfo){
        .clock       = (uint32_t)std::round(timing.act_pixel_freq * 1000),
        .hdisplay    = (uint16_t)MODE->pixelSize.y,
        .hsync_start = hsync_start,
        .hsync_end   = hsync_end,
        .htotal      = (uint16_t)(hsync_end + timing.h_back_porch),
        .vdisplay    = (uint16_t)timing.v_lines_rnd,
        .vsync_start = vsync_start,
        .vsync_end   = vsync_end,
        .vtotal      = (uint16_t)(vsync_end + timing.v_back_porch),
        .vrefresh    = (uint32_t)std::round(timing.act_frame_rate),
        .flags       = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_PVSYNC,
    };
    snprintf(modeInfo.name, sizeof(modeInfo.name), "%dx%d", (int)MODE->pixelSize.x, (int)MODE->pixelSize.y);
}

Aquamarine::CDRMBufferAttachment::CDRMBufferAttachment(SP<CDRMFB> fb_) : fb(fb_) {
    ;
}
