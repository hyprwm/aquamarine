#include <aquamarine/backend/FrameScheduler.hpp>

using namespace Aquamarine;

void CFrameScheduler::onFrameSubmitted() {
    m_pending = true;
}

void CFrameScheduler::onFrameComplete() {
    m_pending = false;
}

void CFrameScheduler::invalidate() {
    m_pending             = false;
    m_frameRunning        = false;
    m_frameScheduled      = false;
    m_rescheduleRequested = false;
}

bool CFrameScheduler::frameInFlight() const {
    return m_pending;
}

bool CFrameScheduler::canSchedule() const {
    return !m_pending && !m_frameRunning && !m_frameScheduled;
}

bool CFrameScheduler::frameScheduled() const {
    return m_frameScheduled;
}

void CFrameScheduler::setFrameScheduled(bool v) {
    m_frameScheduled = v;
}

bool CFrameScheduler::frameRunning() const {
    return m_frameRunning;
}

void CFrameScheduler::setFrameRunning(bool v) {
    m_frameRunning = v;
}

void CFrameScheduler::requestReschedule() {
    m_rescheduleRequested = true;
}

CFrameRunningGuard::CFrameRunningGuard(CFrameScheduler& s) : m_s(s) {
    m_s.setFrameRunning(true);
}

CFrameRunningGuard::~CFrameRunningGuard() {
    m_s.setFrameRunning(false);

    if (!m_s.m_rescheduleRequested)
        return;

    m_s.m_rescheduleRequested = false;

    // the running frame committed, or something already armed an idle frame that path
    // delivers the next frame, re scheduling here would double fire.
    if (!m_s.canSchedule())
        return;

    m_s.rescheduleNeeded.emit();
}
