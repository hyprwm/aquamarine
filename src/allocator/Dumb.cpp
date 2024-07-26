#include <aquamarine/allocator/Dumb.hpp>
#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include "FormatUtils.hpp"
#include "Shared.hpp"
#include "aquamarine/buffer/Buffer.hpp"
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <xf86drm.h>
#include <gbm.h>
#include <unistd.h>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
#define SP CSharedPointer

Aquamarine::CDumbBuffer::CDumbBuffer(const SAllocatorBufferParams& params, Hyprutils::Memory::CWeakPointer<CDumbAllocator> allocator_,
                                     Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain) : allocator(allocator_) {
    if (!allocator)
        return;

    drm_mode_create_dumb createArgs{
        .height = uint32_t(params.size.x),
        .width  = uint32_t(params.size.y),
        .bpp    = 32,
    };

    TRACE(allocator->backend->log(AQ_LOG_TRACE, std::format("DUMB: Allocating a dumb buffer: size {}, format {}", params.size, fourccToName(params.format))));
    if (drmIoctl(gbm_device_get_fd(allocator->gbmDevice), DRM_IOCTL_MODE_CREATE_DUMB, &createArgs) != 0) {
        allocator->backend->log(AQ_LOG_ERROR, std::format("DUMB: DRM_IOCTL_MODE_CREATE_DUMB failed {}", strerror(errno)));
        return;
    }

    int primeFd;
    if (drmPrimeHandleToFD(gbm_device_get_fd(allocator->gbmDevice), createArgs.handle, DRM_CLOEXEC, &primeFd) != 0) {
        allocator->backend->log(AQ_LOG_ERROR, std::format("DUMB: drmPrimeHandleToFD() failed {}", strerror(errno)));
        drm_mode_destroy_dumb destroyArgs{
            .handle = createArgs.handle,
        };
        drmIoctl(gbm_device_get_fd(allocator->gbmDevice), DRM_IOCTL_MODE_DESTROY_DUMB, &destroyArgs);
        return;
    }

    drmFd  = gbm_device_get_fd(allocator->gbmDevice);
    handle = createArgs.handle;
    length = createArgs.pitch * params.size.y;

    attrs.size   = params.size;
    attrs.format = DRM_FORMAT_ARGB8888;
    attrs.offset = 0;
    attrs.fd     = primeFd;
    attrs.stride = createArgs.pitch;

    attrs.success = true;

    allocator->backend->log(AQ_LOG_DEBUG, std::format("DUMB: Allocated a new dumb buffer with size {} and format {}", attrs.size, fourccToName(attrs.format)));
}

Aquamarine::CDumbBuffer::~CDumbBuffer() {
    events.destroy.emit();

    endDataPtr();

    if (handle) {
        drm_mode_destroy_dumb destroyArgs{
            .handle = handle,
        };
        drmIoctl(drmFd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroyArgs);
    }
}

eBufferCapability Aquamarine::CDumbBuffer::caps() {
    return BUFFER_CAPABILITY_DATAPTR;
}

eBufferType Aquamarine::CDumbBuffer::type() {
    return Aquamarine::eBufferType::BUFFER_TYPE_DUMB;
}

void Aquamarine::CDumbBuffer::update(const Hyprutils::Math::CRegion& damage) {
    ;
}

bool Aquamarine::CDumbBuffer::isSynchronous() {
    return false; // FIXME is it correct?
}

bool Aquamarine::CDumbBuffer::good() {
    return true;
}

SSHMAttrs Aquamarine::CDumbBuffer::shm() {
    return attrs;
}

std::tuple<uint8_t*, uint32_t, size_t> Aquamarine::CDumbBuffer::beginDataPtr(uint32_t flags) {
    if (!data) {
        drm_mode_map_dumb mapArgs{
            .handle = handle,
        };
        if (drmIoctl(drmFd, DRM_IOCTL_MODE_MAP_DUMB, &mapArgs) != 0) {
            allocator->backend->log(AQ_LOG_ERROR, std::format("DUMB: DRM_IOCTL_MODE_MAP_DUMB failed {}", strerror(errno)));
            return {};
        }

        void* address = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, drmFd, mapArgs.offset);
        if (address == MAP_FAILED) {
            allocator->backend->log(AQ_LOG_ERROR, std::format("DUMB: mmap failed {}", strerror(errno)));
            return {};
        }

        data = address;
    }

    // FIXME: assumes a 32-bit pixel format
    return {(uint8_t*)data, attrs.format, attrs.stride};
}

void Aquamarine::CDumbBuffer::endDataPtr() {
    if (data) {
        munmap(data, length);
        data = nullptr;
    }
}

CDumbAllocator::~CDumbAllocator() {
    if (gbmDevice)
        gbm_device_destroy(gbmDevice);
}

SP<CDumbAllocator> Aquamarine::CDumbAllocator::create(int drmfd_, Hyprutils::Memory::CWeakPointer<CBackend> backend_) {
    uint64_t capabilities = 0;
    if (drmGetCap(drmfd_, DRM_CAP_PRIME, &capabilities) || !(capabilities & DRM_PRIME_CAP_EXPORT)) {
        backend_->log(AQ_LOG_ERROR, "Cannot create a Dumb Allocator: PRIME export is not supported by the gpu.");
        return nullptr;
    }

    auto allocator = SP<CDumbAllocator>(new CDumbAllocator(drmfd_, backend_));

    if (!allocator->gbmDevice) {
        backend_->log(AQ_LOG_ERROR, "Cannot create a Dumb Allocator: gbm failed to create a device.");
        return nullptr;
    }

    backend_->log(AQ_LOG_DEBUG, std::format("Created a Dumb Allocator with drm fd {}", drmfd_));

    allocator->self = allocator;

    return allocator;
}

Aquamarine::CDumbAllocator::CDumbAllocator(int fd_, Hyprutils::Memory::CWeakPointer<CBackend> backend_) : fd(fd_), backend(backend_) {
    gbmDevice = gbm_create_device(fd_);
    if (!gbmDevice) {
        backend->log(AQ_LOG_ERROR, std::format("Couldn't open a GBM device at fd {}", fd_));
        return;
    }

    gbmDeviceBackendName = gbm_device_get_backend_name(gbmDevice);
    auto drmName_        = drmGetDeviceNameFromFd2(fd_);
    drmName              = drmName_;
    free(drmName_);
}

SP<IBuffer> Aquamarine::CDumbAllocator::acquire(const SAllocatorBufferParams& params, Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain_) {
    if (params.size.x < 1 || params.size.y < 1) {
        backend->log(AQ_LOG_ERROR, std::format("Couldn't allocate a dumb buffer with invalid size {}", params.size));
        return nullptr;
    }

    auto newBuffer = SP<CDumbBuffer>(new CDumbBuffer(params, self, swapchain_));

    if (!newBuffer->good()) {
        backend->log(AQ_LOG_ERROR, std::format("Couldn't allocate a dumb buffer with size {} and format {}", params.size, fourccToName(params.format)));
        return nullptr;
    }

    buffers.emplace_back(newBuffer);
    std::erase_if(buffers, [](const auto& b) { return b.expired(); });
    return newBuffer;
}

Hyprutils::Memory::CSharedPointer<CBackend> Aquamarine::CDumbAllocator::getBackend() {
    return backend.lock();
}

int Aquamarine::CDumbAllocator::drmFD() {
    return fd;
}
