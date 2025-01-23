#include <aquamarine/backend/Headless.hpp>
#include <fcntl.h>
#include <ctime>
#include <sys/timerfd.h>
#include <cstring>
#include "Shared.hpp"

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
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

Aquamarine::CHeadlessOutput::CHeadlessOutput(const std::string& name_, Hyprutils::Memory::CWeakPointer<CHeadlessBackend> backend_) : backend(backend_) {
    name = name_;

    framecb = makeShared<std::function<void()>>([this]() {
        frameScheduled = false;
        events.frame.emit();
    });
}

Aquamarine::CHeadlessOutput::~CHeadlessOutput() {
    backend->backend->removeIdleEvent(framecb);
    events.destroy.emit();
}

bool Aquamarine::CHeadlessOutput::commit() {
    events.commit.emit();
    state->onCommit();
    needsFrame = false;
    return true;
}

bool Aquamarine::CHeadlessOutput::test() {
    return true;
}

std::vector<SDRMFormat> Aquamarine::CHeadlessOutput::getRenderFormats() {
    return backend->getRenderFormats();
}

Hyprutils::Memory::CSharedPointer<IBackendImplementation> Aquamarine::CHeadlessOutput::getBackend() {
    return backend.lock();
}

void Aquamarine::CHeadlessOutput::scheduleFrame(const scheduleFrameReason reason) {
    TRACE(backend->backend->log(AQ_LOG_TRACE,
                                std::format("CHeadlessOutput::scheduleFrame: reason {}, needsFrame {}, frameScheduled {}", (uint32_t)reason, needsFrame, frameScheduled)));
    // FIXME: limit fps to the committed framerate.
    needsFrame = true;

    if (frameScheduled)
        return;

    frameScheduled = true;
    backend->backend->addIdleEvent(framecb);
}

bool Aquamarine::CHeadlessOutput::destroy() {
    events.destroy.emit();
    std::erase(backend->outputs, self.lock());
    return true;
}

Aquamarine::CHeadlessBackend::~CHeadlessBackend() {
    ;
}

Aquamarine::CHeadlessBackend::CHeadlessBackend(SP<CBackend> backend_) : backend(backend_) {
    timers.timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
}

eBackendType Aquamarine::CHeadlessBackend::type() {
    return eBackendType::AQ_BACKEND_HEADLESS;
}

bool Aquamarine::CHeadlessBackend::start() {
    return true;
}

std::vector<SP<SPollFD>> Aquamarine::CHeadlessBackend::pollFDs() {
    return {makeShared<SPollFD>(timers.timerfd, [this]() { dispatchTimers(); })};
}

int Aquamarine::CHeadlessBackend::drmFD() {
    return -1;
}

bool Aquamarine::CHeadlessBackend::dispatchEvents() {
    return true;
}

uint32_t Aquamarine::CHeadlessBackend::capabilities() {
    return 0;
}

bool Aquamarine::CHeadlessBackend::setCursor(SP<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot) {
    return false;
}

void Aquamarine::CHeadlessBackend::onReady() {
    ;
}

std::vector<SDRMFormat> Aquamarine::CHeadlessBackend::getRenderFormats() {
    for (const auto& impl : backend->getImplementations()) {
        if (impl->type() != AQ_BACKEND_DRM || impl->getRenderableFormats().empty())
            continue;
        return impl->getRenderableFormats();
    }

    // formats probably supported by EGL
    return {SDRMFormat{.drmFormat = DRM_FORMAT_XRGB8888, .modifiers = {DRM_FORMAT_INVALID}},
            SDRMFormat{.drmFormat = DRM_FORMAT_XBGR8888, .modifiers = {DRM_FORMAT_INVALID}},
            SDRMFormat{.drmFormat = DRM_FORMAT_RGBX8888, .modifiers = {DRM_FORMAT_INVALID}},
            SDRMFormat{.drmFormat = DRM_FORMAT_BGRX8888, .modifiers = {DRM_FORMAT_INVALID}},
            SDRMFormat{.drmFormat = DRM_FORMAT_ARGB8888, .modifiers = {DRM_FORMAT_INVALID}},
            SDRMFormat{.drmFormat = DRM_FORMAT_ABGR8888, .modifiers = {DRM_FORMAT_INVALID}},
            SDRMFormat{.drmFormat = DRM_FORMAT_RGBA8888, .modifiers = {DRM_FORMAT_INVALID}},
            SDRMFormat{.drmFormat = DRM_FORMAT_BGRA8888, .modifiers = {DRM_FORMAT_INVALID}},
            SDRMFormat{.drmFormat = DRM_FORMAT_XRGB2101010, .modifiers = {DRM_FORMAT_MOD_LINEAR}},
            SDRMFormat{.drmFormat = DRM_FORMAT_XBGR2101010, .modifiers = {DRM_FORMAT_MOD_LINEAR}},
            SDRMFormat{.drmFormat = DRM_FORMAT_RGBX1010102, .modifiers = {DRM_FORMAT_MOD_LINEAR}},
            SDRMFormat{.drmFormat = DRM_FORMAT_BGRX1010102, .modifiers = {DRM_FORMAT_MOD_LINEAR}},
            SDRMFormat{.drmFormat = DRM_FORMAT_ARGB2101010, .modifiers = {DRM_FORMAT_MOD_LINEAR}},
            SDRMFormat{.drmFormat = DRM_FORMAT_ABGR2101010, .modifiers = {DRM_FORMAT_MOD_LINEAR}},
            SDRMFormat{.drmFormat = DRM_FORMAT_RGBA1010102, .modifiers = {DRM_FORMAT_MOD_LINEAR}},
            SDRMFormat{.drmFormat = DRM_FORMAT_BGRA1010102, .modifiers = {DRM_FORMAT_MOD_LINEAR}}};
}

std::vector<SDRMFormat> Aquamarine::CHeadlessBackend::getCursorFormats() {
    return {}; // No cursor support
}

bool Aquamarine::CHeadlessBackend::createOutput(const std::string& name) {
    auto output = SP<CHeadlessOutput>(new CHeadlessOutput(name.empty() ? std::format("HEADLESS-{}", ++outputIDCounter) : name, self.lock()));
    outputs.emplace_back(output);
    output->modes.emplace_back(SP<SOutputMode>(new SOutputMode(Vector2D{1920, 1080}, 60, true)));
    output->swapchain = CSwapchain::create(backend->primaryAllocator, self.lock());
    output->self      = output;
    backend->events.newOutput.emit(SP<IOutput>(output));

    return true;
}

void Aquamarine::CHeadlessBackend::dispatchTimers() {
    std::vector<CTimer> toFire;
    for (size_t i = 0; i < timers.timers.size(); ++i) {
        if (timers.timers.at(i).expired()) {
            toFire.emplace_back(timers.timers.at(i));
            timers.timers.erase(timers.timers.begin() + i);
            i--;
            continue;
        }
    }

    for (auto const& copy : toFire) {
        if (copy.what)
            copy.what();
    }

    updateTimerFD();
}

void Aquamarine::CHeadlessBackend::updateTimerFD() {
    long long  lowestNs = TIMESPEC_NSEC_PER_SEC * 240 /* 240s, 4 mins */;
    const auto clocknow = std::chrono::steady_clock::now();
    bool       any      = false;

    for (auto const& t : timers.timers) {
        auto delta = std::chrono::duration_cast<std::chrono::microseconds>(t.when - clocknow).count() * 1000 /* µs -> ns */;

        if (delta < lowestNs)
            lowestNs = delta;
    }

    if (lowestNs < 0)
        lowestNs = 0;

    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    timespecAddNs(&now, lowestNs);

    itimerspec ts = {.it_value = now};

    if (timerfd_settime(timers.timerfd, TFD_TIMER_ABSTIME, &ts, nullptr))
        backend->log(AQ_LOG_ERROR, std::format("headless: failed to arm timerfd: {}", strerror(errno)));
}

SP<IAllocator> Aquamarine::CHeadlessBackend::preferredAllocator() {
    return backend->primaryAllocator;
}

std::vector<SP<IAllocator>> Aquamarine::CHeadlessBackend::getAllocators() {
    return {backend->primaryAllocator};
}

bool Aquamarine::CHeadlessBackend::CTimer::expired() {
    return std::chrono::steady_clock::now() > when;
}
