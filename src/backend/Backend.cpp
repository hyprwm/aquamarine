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
        bool failed = i->pollFD() < 0;
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

    sessionFDs = session->pollFDs();

    return true;
}

void Aquamarine::CBackend::log(eBackendLogLevel level, const std::string& msg) {
    if (!options.logFunction)
        return;

    options.logFunction(level, msg);
}

void Aquamarine::CBackend::enterLoop() {
    std::vector<pollfd> pollFDs;

    for (auto& i : implementations) {
        auto fd = i->pollFD();

        pollFDs.emplace_back(pollfd{.fd = fd, .events = POLLIN, .revents = 0});
    }

    std::thread pollThr([this, &pollFDs]() {
        int ret = 0;
        while (1) {
            ret = poll(pollFDs.data(), pollFDs.size(), 5000 /* 5 seconds, reasonable. It's because we might need to terminate */);
            if (ret < 0) {
                log(AQ_LOG_CRITICAL, std::format("Polling fds failed with {}", errno));
                terminate = true;
                exit(1);
            }

            for (size_t i = 0; i < pollFDs.size(); ++i) {
                if (pollFDs[i].revents & POLLHUP) {
                    log(AQ_LOG_CRITICAL, std::format("disconnected from pollfd {}", i));
                    terminate = true;
                    exit(1);
                }
            }

            if (terminate)
                break;

            if (ret != 0) {
                std::lock_guard<std::mutex> lg(m_sEventLoopInternals.loopRequestMutex);
                m_sEventLoopInternals.shouldProcess = true;
                m_sEventLoopInternals.loopSignal.notify_all();
            }
        }
    });

    while (1) {
        m_sEventLoopInternals.loopRequestMutex.unlock(); // unlock, we are ready to take events

        std::unique_lock lk(m_sEventLoopInternals.loopMutex);
        if (m_sEventLoopInternals.shouldProcess == false) // avoid a lock if a thread managed to request something already since we .unlock()ed
            m_sEventLoopInternals.loopSignal.wait_for(lk, std::chrono::seconds(5), [this] { return m_sEventLoopInternals.shouldProcess == true; }); // wait for events

        m_sEventLoopInternals.loopRequestMutex.lock(); // lock incoming events

        if (terminate)
            break;

        m_sEventLoopInternals.shouldProcess = false;

        std::lock_guard<std::mutex> lg(m_sEventLoopInternals.eventLock);

        dispatchEventsAsync();
    }
}

std::vector<int> Aquamarine::CBackend::getPollFDs() {
    std::vector<int> result;
    for (auto& i : implementations) {
        int fd = i->pollFD();
        if (fd < 0)
            continue;

        result.push_back(fd);
    }

    for (auto& sfd : sessionFDs) {
        result.push_back(sfd);
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

void Aquamarine::CBackend::dispatchEventsAsync() {
    for (auto& i : implementations) {
        i->dispatchEvents();
    }

    if (session)
        session->dispatchPendingEventsAsync();
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
