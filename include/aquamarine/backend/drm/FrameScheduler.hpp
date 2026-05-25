#pragma once

#include <cstdint>
#include <ctime>

namespace Aquamarine {
    // Per-connector page-flip / frame scheduling state for the DRM backend.
    //
    // Wraps the flip-flags that previously lived directly on SDRMConnector
    // (isPageFlipPending, pageFlipPendingAtMs, frameEventScheduled, isFrameRunning)
    // behind named methods, so a single owner holds the page-flip/frame lifecycle.
    class CFrameScheduler {
      public:
        // a page-flip was submitted to the kernel with a completion event requested.
        void onFlipSubmitted() {
            m_pending     = true;
            m_pendingAtMs = bootMs();
        }

        // the kernel delivered the page-flip completion event.
        void onFlipComplete() {
            m_pending = false;
        }

        // the in-flight flip (if any) is void and no completion event will arrive;
        // reset the flip/frame bookkeeping.
        void invalidate() {
            m_pending        = false;
            m_frameRunning   = false;
            m_frameScheduled = false;
        }

        // is a page-flip in flight on this connector's CRTC?
        bool flipInFlight() const {
            return m_pending;
        }

        // may a new frame be scheduled? (does not consider output enabled state)
        bool canSchedule() const {
            return !m_pending && !m_frameRunning && !m_frameScheduled;
        }

        // State accessors. setFrameScheduled / frameScheduled() drive the idle-frame
        // loop; setFrameRunning / frameRunning() guard event emission in handlePF.
        bool frameScheduled() const {
            return m_frameScheduled;
        }
        void setFrameScheduled(bool v) {
            m_frameScheduled = v;
        }
        bool frameRunning() const {
            return m_frameRunning;
        }
        void setFrameRunning(bool v) {
            m_frameRunning = v;
        }
        uint64_t pendingAtMs() const {
            return m_pendingAtMs;
        }

      private:
        static uint64_t bootMs() {
            struct timespec ts;
            clock_gettime(CLOCK_BOOTTIME, &ts);
            return (ts.tv_sec * 1000ULL) + (ts.tv_nsec / 1000000ULL);
        }

        bool     m_pending        = false;
        uint64_t m_pendingAtMs    = 0; // CLOCK_BOOTTIME ms when the flip was submitted
        bool     m_frameScheduled = false;
        bool     m_frameRunning   = false;
    };
}
