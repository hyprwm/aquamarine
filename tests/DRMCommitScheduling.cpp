#include <aquamarine/backend/FrameScheduler.hpp>
#include "backend/drm/CommitScheduling.hpp"
#include "shared.hpp"

using namespace Aquamarine;

int main() {
    int             ret = 0;

    CFrameScheduler scheduler;

    // A blocking modeset completes before its ioctl returns. It must not leave
    // userspace waiting for an event that the event loop has not drained yet.
    const auto blockingModeset = drmPageFlipOptions(false, true, true, true, false);
    EXPECT(blockingModeset.event, false);
    EXPECT(blockingModeset.async, false);
    if (blockingModeset.event)
        scheduler.onFrameSubmitted();
    EXPECT(scheduler.frameInFlight(), false);
    EXPECT(scheduler.canSchedule(), true);

    // A normal nonblocking page flip needs an event to release the scheduler.
    const auto nonblockingFlip = drmPageFlipOptions(false, true, false, true, false);
    EXPECT(nonblockingFlip.event, true);
    EXPECT(nonblockingFlip.async, false);
    if (nonblockingFlip.event)
        scheduler.onFrameSubmitted();
    EXPECT(scheduler.frameInFlight(), true);
    EXPECT(scheduler.canSchedule(), false);
    scheduler.onFrameComplete();
    EXPECT(scheduler.canSchedule(), true);

    const auto immediateFlip = drmPageFlipOptions(false, true, false, true, true);
    EXPECT(immediateFlip.event, true);
    EXPECT(immediateFlip.async, true);

    const auto testCommit = drmPageFlipOptions(true, true, false, true, true);
    EXPECT(testCommit.event, false);
    EXPECT(testCommit.async, false);

    return ret;
}
