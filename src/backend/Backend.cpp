#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/backend/Wayland.hpp>
#include <aquamarine/backend/DRM.hpp>
#include <aquamarine/allocator/GBM.hpp>
#include <sys/poll.h>
#include <thread>
#include <chrono>

using namespace Hyprutils::Memory;
using namespace Aquamarine;
#define SP CSharedPointer

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

Aquamarine::SBackendImplementationOptions::SBackendImplementationOptions() {
    backendType        = AQ_BACKEND_WAYLAND;
    backendRequestMode = AQ_BACKEND_REQUEST_IF_AVAILABLE;
}

Aquamarine::SBackendOptions::SBackendOptions() {
    logFunction = nullptr;
}

Hyprutils::Memory::CSharedPointer<CBackend> Aquamarine::CBackend::create(const std::vector<SBackendImplementationOptions>& backends, const SBackendOptions& options) {
    auto backend = SP<CBackend>(new CBackend());

    backend->options               = options;
    backend->implementationOptions = backends;
    backend->self                  = backend;

    if (backends.size() <= 0)
        return nullptr;

    backend->log(AQ_LOG_DEBUG, "Creating an Aquamarine backend!");

    for (auto& b : backends) {
        if (b.backendType == AQ_BACKEND_WAYLAND) {
            auto ref = SP<CWaylandBackend>(new CWaylandBackend(backend));
            backend->implementations.emplace_back(ref);
            ref->self = ref;
        } else if (b.backendType == AQ_BACKEND_DRM) {
            auto ref = CDRMBackend::attempt(backend);
            if (!ref) {
                backend->log(AQ_LOG_ERROR, "DRM Backend failed");
                continue;
            }
            backend->implementations.emplace_back(ref);
            ref->self = ref;
        } else {
            backend->log(AQ_LOG_ERROR, std::format("Unknown backend id: {}", (int)b.backendType));
            continue;
        }
    }

    return backend;
}

Aquamarine::CBackend::~CBackend() {
    ;
}

bool Aquamarine::CBackend::start() {
    log(AQ_LOG_DEBUG, "Starting the Aquamarine backend!");

    bool fallback = false;
    int  started  = 0;

    for (size_t i = 0; i < implementations.size(); ++i) {
        const bool ok = implementations.at(i)->start();

        if (!ok) {
            log(AQ_LOG_ERROR, std::format("Requested backend ({}) could not start, enabling fallbacks", backendTypeToName(implementationOptions.at(i).backendType)));
            fallback = true;
            if (implementationOptions.at(i).backendRequestMode == AQ_BACKEND_REQUEST_MANDATORY) {
                log(AQ_LOG_CRITICAL,
                    std::format("Requested backend ({}) could not start and it's mandatory, cannot continue!", backendTypeToName(implementationOptions.at(i).backendType)));
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
    for (auto& b : implementations) {
        if (b->drmFD() >= 0) {
            allocator = CGBMAllocator::create(b->drmFD(), self);
            break;
        }
    }

    if (!allocator)
        return false;

    ready = true;
    for (auto& b : implementations) {
        b->onReady();
    }

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
    for (auto& i : implementations) {
        auto pollfds = i->pollFDs();
        for (auto& p : pollfds) {
            result.emplace_back(p);
        }
    }

    for (auto& sfd : sessionFDs) {
        result.emplace_back(sfd);
    }

    return result;
}

int Aquamarine::CBackend::drmFD() {
    for (auto& i : implementations) {
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
    for (auto& b : implementations) {
        if (b->type() != AQ_BACKEND_DRM && b->type() != AQ_BACKEND_WAYLAND)
            continue;

        return b->getRenderFormats();
    }

    for (auto& b : implementations) {
        return b->getRenderFormats();
    }

    return {};
}

const std::vector<Hyprutils::Memory::CSharedPointer<IBackendImplementation>>& Aquamarine::CBackend::getImplementations() {
    return implementations;
}
