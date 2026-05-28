#pragma once

#include <hyprutils/signal/Signal.hpp>

namespace Aquamarine {
    // Per-output frame scheduling state, shared by the DRM and Wayland backends.
    //
    // Both backends produce an end-of-frame event — DRM from the kernel page-flip
    // completion, Wayland from wl_callback.done on a wl_surface.frame. This class
    // models that lifecycle uniformly and fires frameReady on completion.
    class CFrameScheduler {
      public:
        // a frame was submitted to the host with a completion event requested.
        // DRM: page-flip submitted with PAGE_FLIP_EVENT. Wayland: sendCommit followed
        // by a wl_surface.frame whose done is wired to onFrameComplete.
        void onFrameSubmitted() {
            m_pending = true;
        }

        // the host delivered the frame completion event.
        // DRM: kernel page-flip handler. Wayland: wl_callback.done.
        // State-only — clears m_pending. The backend fires frameReady explicitly
        // at the right point in its handler (preserving the present-before-frame
        // ordering on DRM).
        void onFrameComplete() {
            m_pending = false;
        }

        // the in-flight frame (if any) is void and no completion event will arrive;
        // reset the frame bookkeeping.
        void invalidate() {
            m_pending        = false;
            m_frameRunning   = false;
            m_frameScheduled = false;
        }

        // is a frame in flight awaiting completion?
        bool frameInFlight() const {
            return m_pending;
        }

        // may a new frame be scheduled? (does not consider output enabled state)
        bool canSchedule() const {
            return !m_pending && !m_frameRunning && !m_frameScheduled;
        }

        // State accessors. setFrameScheduled / frameScheduled() drive the idle-frame
        // loop; setFrameRunning / frameRunning() guard event emission on completion.
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

        // Fires from onFrameComplete. The output wires this to events.frame.emit.
        Hyprutils::Signal::CSignalT<> frameReady;

      private:
        bool m_pending        = false;
        bool m_frameScheduled = false;
        bool m_frameRunning   = false;
    };

    // RAII pair for isFrameRunning: set on ctor, cleared on every exit path so a
    // reentrant emit handler cannot strand it.
    class CFrameRunningGuard {
      public:
        explicit CFrameRunningGuard(CFrameScheduler& s) : m_s(s) {
            m_s.setFrameRunning(true);
        }
        ~CFrameRunningGuard() {
            m_s.setFrameRunning(false);
        }
        CFrameRunningGuard(const CFrameRunningGuard&)            = delete;
        CFrameRunningGuard& operator=(const CFrameRunningGuard&) = delete;

      private:
        CFrameScheduler& m_s;
    };
}
