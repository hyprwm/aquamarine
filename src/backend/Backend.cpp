#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/backend/Wayland.hpp>
#include <aquamarine/backend/Headless.hpp>
#include <aquamarine/backend/DRM.hpp>
#include <aquamarine/allocator/GBM.hpp>
#include <ranges>
#include <sys/timerfd.h>
#include <ctime>
#include <cstring>
#include <xf86drm.h>
#include <fcntl.h>
#include <unistd.h>

using namespace Hyprutils::Memory;
using namespace Aquamarine;
#define SP CSharedPointer

#define TIMESPEC_NSEC_PER_SEC 1000000000LL

static void timespecAddNs(timespec* pTimespec, int64_t delta) {
    int delta_ns_low = delta % TIMESPEC_NSEC_PER_SEC;
    int delta_s_high = delta / TIMESPEC_NSEC_PER_SEC;

    pTimespec->tv_sec += delta_s_high;

    pTimespec->tv_nsec += (long)delta_ns_low;
    if (pTimespec->tv_nsec >= TIMESPEC_NSEC_PER_SEC) {
        pTimespec->tv_nsec -= TIMESPEC_NSEC_PER_SEC;
        ++pTimespec->tv_sec;
    }
}

static const char* backendTypeToName(eBackendType type) {
    switch (type) {
        case AQ_BACKEND_DRM: return "drm";
        case AQ_BACKEND_HEADLESS: return "headless";
        case AQ_BACKEND_WAYLAND: return "wayland";
        default: break;
    }
    return "invalid";
}

Aquamarine::CBackend::CBackend() {
    ;
}

Aquamarine::SBackendImplementationOptions::SBackendImplementationOptions() : backendType(AQ_BACKEND_WAYLAND), backendRequestMode(AQ_BACKEND_REQUEST_IF_AVAILABLE) {
    ;
}

Aquamarine::SBackendOptions::SBackendOptions() : logFunction(nullptr) {
    ;
}

Hyprutils::Memory::CSharedPointer<CBackend> Aquamarine::CBackend::create(const std::vector<SBackendImplementationOptions>& backends, const SBackendOptions& options) {
    auto backend = SP<CBackend>(new CBackend());

    backend->options               = options;
    backend->implementationOptions = backends;
    backend->self                  = backend;

    if (backends.size() <= 0)
        return nullptr;

    backend->log(AQ_LOG_DEBUG, "Creating an Aquamarine backend!");

    for (auto const& b : backends) {
        if (b.backendType == AQ_BACKEND_WAYLAND) {
            auto ref = SP<CWaylandBackend>(new CWaylandBackend(backend));
            backend->implementations.emplace_back(ref);
            ref->self = ref;
        } else if (b.backendType == AQ_BACKEND_DRM) {
            auto ref = CDRMBackend::attempt(backend);
            if (ref.empty()) {
                backend->log(AQ_LOG_ERROR, "DRM Backend failed");
                continue;
            }

            for (auto const& r : ref) {
                backend->implementations.emplace_back(r);
            }
        } else if (b.backendType == AQ_BACKEND_HEADLESS) {
            auto ref = SP<CHeadlessBackend>(new CHeadlessBackend(backend));
            backend->implementations.emplace_back(ref);
            ref->self = ref;
        } else {
            backend->log(AQ_LOG_ERROR, std::format("Unknown backend id: {}", (int)b.backendType));
            continue;
        }
    }

    // create a timerfd for idle events
    backend->idle.fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);

    return backend;
}

Aquamarine::CBackend::~CBackend() {
    ;
}

bool Aquamarine::CBackend::start() {
    log(AQ_LOG_DEBUG, "Starting the Aquamarine backend!");

    int  started = 0;

    auto optionsForType = [this](eBackendType type) -> SBackendImplementationOptions {
        for (auto const& o : implementationOptions) {
            if (o.backendType == type)
                return o;
        }
        return SBackendImplementationOptions{};
    };

    for (size_t i = 0; i < implementations.size(); ++i) {
        const bool ok = implementations.at(i)->start();

        if (!ok) {
            log(AQ_LOG_ERROR, std::format("Requested backend ({}) could not start, enabling fallbacks", backendTypeToName(implementations.at(i)->type())));
            if (optionsForType(implementations.at(i)->type()).backendRequestMode == AQ_BACKEND_REQUEST_MANDATORY) {
                log(AQ_LOG_CRITICAL, std::format("Requested backend ({}) could not start and it's mandatory, cannot continue!", backendTypeToName(implementations.at(i)->type())));
                implementations.clear();
                return false;
            }
        } else
            started++;
    }

    if (implementations.empty() || started <= 0) {
        log(AQ_LOG_CRITICAL, "No backend could be opened. Make sure there was a correct backend passed to CBackend, and that your environment supports at least one of them.");
        return false;
    }

    // erase failed impls
    std::erase_if(implementations, [this](const auto& i) {
        bool failed = i->pollFDs().empty();
        if (failed)
            log(AQ_LOG_ERROR, std::format("Implementation {} failed, erasing.", backendTypeToName(i->type())));
        return failed;
    });

    // TODO: obviously change this when (if) we add different allocators.
    for (auto const& b : implementations) {
        if (b->drmFD() >= 0) {
            auto fd = reopenDRMNode(b->drmFD());
            if (fd < 0) {
                // this is critical, we cannot create an allocator properly
                log(AQ_LOG_CRITICAL, "Failed to create an allocator (reopenDRMNode failed)");
                return false;
            }
            primaryAllocator = CGBMAllocator::create(fd, self);
            break;
        }
    }

    if (!primaryAllocator) {
        log(AQ_LOG_CRITICAL, "Cannot open backend: no allocator available");
        return false;
    }

    ready = true;
    for (auto const& b : implementations) {
        b->onReady();
    }

    if (session)
        session->onReady();

    sessionFDs = session ? session->pollFDs() : std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>>{};

    return true;
}

void Aquamarine::CBackend::log(eBackendLogLevel level, const std::string& msg) {
    if (!options.logFunction)
        return;

    options.logFunction(level, msg);
}

std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>> Aquamarine::CBackend::getPollFDs() {
    std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>> result;
    for (auto const& i : implementations) {
        auto pollfds = i->pollFDs();
        for (auto const& p : pollfds) {
            log(AQ_LOG_DEBUG, std::format("backend: poll fd {} for implementation {}", p->fd, backendTypeToName(i->type())));
            result.emplace_back(p);
        }
    }

    for (auto const& sfd : sessionFDs) {
        log(AQ_LOG_DEBUG, std::format("backend: poll fd {} for session", sfd->fd));
        result.emplace_back(sfd);
    }

    log(AQ_LOG_DEBUG, std::format("backend: poll fd {} for idle", idle.fd));
    result.emplace_back(makeShared<SPollFD>(idle.fd, [this]() { dispatchIdle(); }));

    return result;
}

int Aquamarine::CBackend::drmFD() {
    for (auto const& i : implementations) {
        int fd = i->drmFD();
        if (fd < 0)
            continue;

        return fd;
    }
    return -1;
}

bool Aquamarine::CBackend::hasSession() {
    return session;
}

std::vector<SDRMFormat> Aquamarine::CBackend::getPrimaryRenderFormats() {
    for (auto const& b : implementations) {
        if (b->type() != AQ_BACKEND_DRM && b->type() != AQ_BACKEND_WAYLAND)
            continue;

        return b->getRenderFormats();
    }

    for (auto const& b : implementations) {
        return b->getRenderFormats();
    }

    return {};
}

const std::vector<SP<IBackendImplementation>>& Aquamarine::CBackend::getImplementations() {
    return implementations;
}

void Aquamarine::CBackend::addIdleEvent(SP<std::function<void(void)>> fn) {
    auto r = idle.pending.emplace_back(fn);

    updateIdleTimer();
}

void Aquamarine::CBackend::updateIdleTimer() {
    uint64_t ADD_NS = idle.pending.empty() ? TIMESPEC_NSEC_PER_SEC * 240ULL /* 240s, 4 mins */ : 0;

    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    timespecAddNs(&now, ADD_NS);

    itimerspec ts = {.it_value = now};

    if (timerfd_settime(idle.fd, TFD_TIMER_ABSTIME, &ts, nullptr))
        log(AQ_LOG_ERROR, std::format("backend: failed to arm timerfd: {}", strerror(errno)));
}

void Aquamarine::CBackend::removeIdleEvent(SP<std::function<void(void)>> pfn) {
    std::erase(idle.pending, pfn);
}

void Aquamarine::CBackend::dispatchIdle() {
    auto cpy = idle.pending;
    idle.pending.clear();

    for (auto const& i : cpy) {
        if (i && *i)
            (*i)();
    }

    updateIdleTimer();
}

void Aquamarine::CBackend::onNewGpu(std::string path) {
    const auto primary    = std::ranges::find_if(implementations, [](SP<IBackendImplementation> value) { return value->type() == Aquamarine::AQ_BACKEND_DRM; });
    const auto primaryDrm = primary != implementations.end() ? ((Aquamarine::CDRMBackend*)(*primary).get())->self.lock() : nullptr;

    auto       ref = CDRMBackend::fromGpu(path, self.lock(), primaryDrm);
    if (!ref) {
        log(AQ_LOG_ERROR, std::format("DRM Backend failed for device {}", path));
        return;
    }
    if (!ref->start()) {
        log(AQ_LOG_ERROR, std::format("Couldn't start DRM Backend for device {}", path));
        return;
    }

    implementations.emplace_back(ref);
    events.pollFDsChanged.emit();

    ref->onReady();        // Renderer created here
    ref->recheckOutputs(); // Now we can recheck outputs
}

// Yoinked from wlroots, render/allocator/allocator.c
// Ref-counting reasons, see https://gitlab.freedesktop.org/mesa/drm/-/merge_requests/110
int Aquamarine::CBackend::reopenDRMNode(int drmFD, bool allowRenderNode) {

    if (drmIsMaster(drmFD)) {
        // Only recent kernels support empty leases
        uint32_t lesseeID = 0;
        int      leaseFD  = drmModeCreateLease(drmFD, nullptr, 0, O_CLOEXEC, &lesseeID);
        if (leaseFD >= 0) {
            return leaseFD;
        } else if (leaseFD != -EINVAL && leaseFD != -EOPNOTSUPP) {
            log(AQ_LOG_ERROR, "reopenDRMNode: drmModeCreateLease failed");
            return -1;
        }
        log(AQ_LOG_DEBUG, "reopenDRMNode: drmModeCreateLease failed, falling back to open");
    }

    char* name = nullptr;
    if (allowRenderNode)
        name = drmGetRenderDeviceNameFromFd(drmFD);

    if (!name) {
        // primary node or no name
        name = drmGetDeviceNameFromFd2(drmFD);

        if (!name) {
            log(AQ_LOG_ERROR, "reopenDRMNode: drmGetDeviceNameFromFd2 failed");
            return -1;
        }
    }

    log(AQ_LOG_DEBUG, std::format("reopenDRMNode: opening node {}", name));

    int newFD = open(name, O_RDWR | O_CLOEXEC);
    if (newFD < 0) {
        log(AQ_LOG_ERROR, std::format("reopenDRMNode: failed to open node {}", name));
        free(name);
        return -1;
    }

    free(name);

    // We need to authenticate if we are using a DRM primary node and are the master
    if (drmIsMaster(drmFD) && drmGetNodeTypeFromFd(newFD) == DRM_NODE_PRIMARY) {
        drm_magic_t magic;
        if (int ret = drmGetMagic(newFD, &magic); ret < 0) {
            log(AQ_LOG_ERROR, std::format("reopenDRMNode: drmGetMagic failed: {}", strerror(-ret)));
            close(newFD);
            return -1;
        }

        if (int ret = drmAuthMagic(drmFD, magic); ret < 0) {
            log(AQ_LOG_ERROR, std::format("reopenDRMNode: drmAuthMagic failed: {}", strerror(-ret)));
            close(newFD);
            return -1;
        }
    }

    return newFD;
}

std::vector<SDRMFormat> Aquamarine::IBackendImplementation::getRenderableFormats() {
    return {};
}
