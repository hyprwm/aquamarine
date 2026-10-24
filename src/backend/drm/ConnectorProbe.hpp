#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <xf86drmMode.h>

// NOLINTNEXTLINE(readability-identifier-naming): Keep the existing library namespace.
namespace Aquamarine {
    // Forced probes can exceed RLIMIT_RTTIME in one ioctl before a SIGXCPU handler can run.
    // Keep only the query on a SCHED_OTHER worker.
    // Callers wait for result and update their own backend state.
    //
    // Keep fd alive until query() returns and free results with drmModeFreeConnector.
    // Finish all calls before destruction. Running query cannot safely be cancelled.
    class [[gnu::visibility("hidden")]] CConnectorProbe {
      public:
        using FQuery = std::function<drmModeConnector*(int, uint32_t)>;

        explicit CConnectorProbe(FQuery query = drmModeGetConnector);
        ~CConnectorProbe() = default;

        CConnectorProbe(const CConnectorProbe&)            = delete;
        CConnectorProbe& operator=(const CConnectorProbe&) = delete;
        CConnectorProbe(CConnectorProbe&&)                 = delete;
        CConnectorProbe&  operator=(CConnectorProbe&&)     = delete;

        drmModeConnector* query(int fd, uint32_t connectorID);

      private:
        struct SRequest {
            int      fd          = -1;
            uint32_t connectorID = 0;
        };

        struct SResult {
            drmModeConnector* connector = nullptr;
            int               error     = 0;
        };

        void                        run(std::stop_token stop);

        const FQuery                m_query;
        std::mutex                  m_submissionMutex;
        std::mutex                  m_mutex;
        std::condition_variable_any m_condition;
        std::optional<SRequest>     m_request;
        std::optional<SResult>      m_result;
        bool                        m_started    = false;
        int                         m_startError = 0;

        // Keep last: jthread stops and joins before the state it uses is destroyed.
        // Stop token wakes idle wait.
        std::jthread m_thread;
    };

    // Process-wide synchronous worker, shared by all DRM backends.
    // Never fall back to probing on an RT caller.
    [[gnu::visibility("hidden")]] drmModeConnector* probeDRMConnector(int fd, uint32_t connectorID);
}
