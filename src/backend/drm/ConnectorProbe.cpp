#include "ConnectorProbe.hpp"

#include <cerrno>
#include <new>
#include <pthread.h>
#include <sched.h>
#include <system_error>
#include <utility>
#include <hyprutils/memory/UniquePtr.hpp>

#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace Hyprutils::Memory;

static int useNormalScheduling() {
    const sched_param PARAM = {
        .sched_priority = 0,
    };
#ifdef __linux__
    // Keep RESET_ON_FORK because clearing it needs CAP_SYS_NICE.
    // Use per-thread syscalls because musl's sched_setscheduler returns ENOSYS.
    const auto POLICY = syscall(SYS_sched_getscheduler, 0);
    if (POLICY < 0)
        return errno;

    // Do not require permission to change a policy that is already correct.
    if ((POLICY & ~SCHED_RESET_ON_FORK) == SCHED_OTHER)
        return 0;

    if (syscall(SYS_sched_setscheduler, 0, SCHED_OTHER | (POLICY & SCHED_RESET_ON_FORK), &PARAM) < 0)
        return errno;

    return 0;
#else
    return pthread_setschedparam(pthread_self(), SCHED_OTHER, &PARAM);
#endif
}

Aquamarine::CConnectorProbe::CConnectorProbe(FQuery query) : m_query(std::move(query)), m_thread([this](std::stop_token stop) { run(stop); }) {
    // Accept queries only after worker has confirmed SCHED_OTHER.
    std::unique_lock lock(m_mutex);
    m_condition.wait(lock, [this] { return m_started; });
    if (m_startError)
        throw std::system_error(m_startError, std::generic_category(), "Connector probe worker scheduling");
}

drmModeConnector* Aquamarine::CConnectorProbe::query(int fd, uint32_t connectorID) {
    // Hold submission lock through wait so another caller cannot replace this request or consume its result.
    SResult result;
    {
        std::scoped_lock submitLock(m_submissionMutex);
        std::unique_lock lock(m_mutex);
        m_result.reset();
        m_request = SRequest{
            .fd          = fd,
            .connectorID = connectorID,
        };
        m_condition.notify_all();
        m_condition.wait(lock, [this] { return m_result.has_value(); });
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access): wait's predicate holds with m_mutex locked.
        result = m_result.value();
    }

    // errno is thread-local, preserve caller's value on success.
    if (!result.connector)
        errno = result.error;
    return result.connector;
}

void Aquamarine::CConnectorProbe::run(std::stop_token stop) {
    const int START_ERROR = useNormalScheduling();
    {
        std::scoped_lock lock(m_mutex);
        m_startError = START_ERROR;
        m_started    = true;
    }
    m_condition.notify_all();
    if (START_ERROR)
        return;

    while (true) {
        SRequest request;
        {
            std::unique_lock lock(m_mutex);
            if (!m_condition.wait(lock, stop, [this] { return m_request.has_value(); }))
                return;
            // NOLINTNEXTLINE(bugprone-unchecked-optional-access): wait returned true with m_mutex locked.
            request = m_request.value();
            m_request.reset();
        }

        SResult result;
        errno            = 0;
        result.connector = m_query(request.fd, request.connectorID);
        result.error     = result.connector ? 0 : (errno ? errno : EIO);

        {
            std::scoped_lock lock(m_mutex);
            m_result = result;
        }
        m_condition.notify_all();
    }
}

namespace Aquamarine::NConnectorProbe {
    // Queries are synchronous and hold no backend references.
    // Sharing one worker also covers GPU discovery before a DRM backend exists,
    //   without changing public backend layout or creating a thread per probe.
    static CUniquePointer<CConnectorProbe>& worker() {
        static auto instance = makeUnique<CConnectorProbe>();
        return instance;
    }
}

drmModeConnector* Aquamarine::probeDRMConnector(int fd, uint32_t connectorID) {
    try {
        return NConnectorProbe::worker()->query(fd, connectorID);
    } catch (const std::system_error& e) { errno = e.code().value(); } catch (const std::bad_alloc&) {
        errno = ENOMEM;
    }
    return nullptr;
}
