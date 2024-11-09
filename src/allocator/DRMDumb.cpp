#include <aquamarine/allocator/DRMDumb.hpp>
#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/backend/DRM.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include "FormatUtils.hpp"
#include "Shared.hpp"
#include <xf86drm.h>
#include <unistd.h>
#include <cstring>
#include <sys/mman.h>
#include "../backend/drm/Renderer.hpp"

using namespace Aquamarine;
using namespace Hyprutils::Memory;
#define SP CSharedPointer
#define WP CWeakPointer

Aquamarine::CDRMDumbBuffer::CDRMDumbBuffer(const SAllocatorBufferParams& params, Hyprutils::Memory::CWeakPointer<CDRMDumbAllocator> allocator_,
                                           Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain) : allocator(allocator_) {
    attrs.format = params.format;

    drm_mode_create_dumb request = {
        .height = (uint32_t)params.size.y,
        .width  = (uint32_t)params.size.x,
        .bpp    = 32,
    };

    if (int ret = drmIoctl(allocator->drmFD(), DRM_IOCTL_MODE_CREATE_DUMB, &request); ret < 0) {
        allocator->backend->log(AQ_LOG_ERROR, std::format("failed to create a drm_dumb buffer: {}", strerror(-ret)));
        return;
    }

    stride    = request.pitch;
    handle    = request.handle;
    size      = request.size;
    pixelSize = {(double)request.width, (double)request.height};

    attrs.size   = pixelSize;
    attrs.fd     = request.handle;
    attrs.stride = stride;

    drm_mode_map_dumb request2 = {
        .handle = handle,
    };

    if (int ret = drmIoctl(allocator->drmFD(), DRM_IOCTL_MODE_MAP_DUMB, &request2); ret < 0) {
        allocator->backend->log(AQ_LOG_ERROR, std::format("failed to map a drm_dumb buffer: {}", strerror(-ret)));
        return;
    }

    data = (uint8_t*)mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, allocator->drmFD(), request2.offset);
    if (!data) {
        allocator->backend->log(AQ_LOG_ERROR, "failed to mmmap a drm_dumb buffer");
        return;
    }

    // null the entire buffer so we dont get garbage
    memset(data, 0x00, size);

    attrs.success = true;
}

Aquamarine::CDRMDumbBuffer::~CDRMDumbBuffer() {
    events.destroy.emit();

    if (handle == 0)
        return;

    if (data)
        munmap(data, size);

    drm_mode_destroy_dumb request = {
        .handle = handle,
    };
    drmIoctl(allocator->drmFD(), DRM_IOCTL_MODE_DESTROY_DUMB, &request);
}

eBufferCapability Aquamarine::CDRMDumbBuffer::caps() {
    return eBufferCapability::BUFFER_CAPABILITY_DATAPTR;
}

eBufferType Aquamarine::CDRMDumbBuffer::type() {
    return eBufferType::BUFFER_TYPE_SHM;
}

void Aquamarine::CDRMDumbBuffer::update(const Hyprutils::Math::CRegion& damage) {
    ; // nothing to do
}

bool Aquamarine::CDRMDumbBuffer::isSynchronous() {
    return true;
}

bool Aquamarine::CDRMDumbBuffer::good() {
    return attrs.success && data;
}

SSHMAttrs Aquamarine::CDRMDumbBuffer::shm() {
    return attrs;
}

std::tuple<uint8_t*, uint32_t, size_t> Aquamarine::CDRMDumbBuffer::beginDataPtr(uint32_t flags) {
    return {data, attrs.format, size};
}

void Aquamarine::CDRMDumbBuffer::endDataPtr() {
    ; // nothing to do
}

uint32_t Aquamarine::CDRMDumbBuffer::drmHandle() {
    return handle;
}

Aquamarine::CDRMDumbAllocator::~CDRMDumbAllocator() {
    ; // nothing to do
}

SP<CDRMDumbAllocator> Aquamarine::CDRMDumbAllocator::create(int drmfd_, Hyprutils::Memory::CWeakPointer<CBackend> backend_) {
    auto a  = SP<CDRMDumbAllocator>(new CDRMDumbAllocator(drmfd_, backend_));
    a->self = a;
    return a;
}

SP<IBuffer> Aquamarine::CDRMDumbAllocator::acquire(const SAllocatorBufferParams& params, SP<CSwapchain> swapchain_) {
    auto buf = SP<IBuffer>(new CDRMDumbBuffer(params, self, swapchain_));
    if (!buf->good())
        return nullptr;
    return buf;
}

SP<CBackend> Aquamarine::CDRMDumbAllocator::getBackend() {
    return backend.lock();
}

int Aquamarine::CDRMDumbAllocator::drmFD() {
    return drmfd;
}

eAllocatorType Aquamarine::CDRMDumbAllocator::type() {
    return eAllocatorType::AQ_ALLOCATOR_TYPE_DRM_DUMB;
}

Aquamarine::CDRMDumbAllocator::CDRMDumbAllocator(int fd_, Hyprutils::Memory::CWeakPointer<CBackend> backend_) : drmfd(fd_), backend(backend_) {
    ; // nothing to do
}
