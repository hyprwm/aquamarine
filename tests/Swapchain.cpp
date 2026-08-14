#include <aquamarine/allocator/Swapchain.hpp>
#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/buffer/Buffer.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <drm_fourcc.h>
#include "shared.hpp"

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
#define SP CSharedPointer

// Regression test: CSwapchain must hand identical SAllocatorBufferParams to the allocator no
// matter which internal path (fullReconfigure vs resize) performs the allocation. resize() used
// to omit .multigpu, so any reallocation that only changed the length silently dropped the flag.
// On a multi-GPU secondary output this happened on every DPMS off -> on cycle (off clears the
// swapchain to length 0, on re-adds buffers via resize()) and produced non-multigpu buffers the
// consumer could not use, permanently blanking the output.

class CMockBuffer : public IBuffer {
  public:
    virtual eBufferCapability caps() {
        return (eBufferCapability)0;
    }
    virtual eBufferType type() {
        return BUFFER_TYPE_MISC;
    }
    virtual void update(const CRegion& damage) {
        ;
    }
    virtual bool isSynchronous() {
        return false;
    }
    virtual bool good() {
        return true;
    }
};

class CMockAllocator : public IAllocator {
  public:
    CMockAllocator(SP<CBackend> backend_) : backend(backend_) {
        ;
    }

    virtual SP<IBuffer> acquire(const SAllocatorBufferParams& params, SP<CSwapchain> swapchain) {
        lastParams = params;
        return makeShared<CMockBuffer>();
    }
    virtual SP<CBackend> getBackend() {
        return backend;
    }
    virtual int drmFD() {
        return -1;
    }
    virtual eAllocatorType type() {
        return AQ_ALLOCATOR_TYPE_GBM;
    }

    SAllocatorBufferParams lastParams;
    SP<CBackend>           backend;
};

int main() {
    int ret = 0;

    // a headless backend is enough for the swapchain's logging
    auto implOptions               = SBackendImplementationOptions{};
    implOptions.backendType        = AQ_BACKEND_HEADLESS;
    implOptions.backendRequestMode = AQ_BACKEND_REQUEST_IF_AVAILABLE;

    auto backendOptions = SBackendOptions{};

    auto backend = CBackend::create({implOptions}, backendOptions);
    EXPECT(!!backend, true);
    if (!backend)
        return 1;

    auto allocator = makeShared<CMockAllocator>(backend);
    auto swapchain = CSwapchain::create(allocator, nullptr);
    EXPECT(!!swapchain, true);
    if (!swapchain)
        return 1;

    SSwapchainOptions options;
    options.length   = 3;
    options.size     = {1920, 1080};
    options.format   = DRM_FORMAT_XRGB8888;
    options.scanout  = true;
    options.multigpu = true;

    // initial configuration: fullReconfigure path
    EXPECT(swapchain->reconfigure(options), true);
    EXPECT(allocator->lastParams.multigpu, true);
    EXPECT(allocator->lastParams.scanout, true);

    // a dpms-off style teardown: clear to length 0, options retained
    auto cleared   = options;
    cleared.length = 0;
    EXPECT(swapchain->reconfigure(cleared), true);

    // a dpms-on style reconfigure: same format/size, only the length differs -> resize() path.
    // the allocation must still carry multigpu.
    allocator->lastParams = {};
    EXPECT(swapchain->reconfigure(options), true);
    EXPECT(allocator->lastParams.multigpu, true);
    EXPECT(allocator->lastParams.scanout, true);

    // shrink then grow (pure resize both ways) must preserve it too
    auto shorter   = options;
    shorter.length = 2;
    EXPECT(swapchain->reconfigure(shorter), true);
    allocator->lastParams = {};
    EXPECT(swapchain->reconfigure(options), true);
    EXPECT(allocator->lastParams.multigpu, true);

    return ret;
}
