#include <aquamarine/allocator/GBM.hpp>
#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include "FormatUtils.hpp"
#include <xf86drm.h>
#include <gbm.h>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
#define SP CSharedPointer

static SDRMFormat guessFormatFrom(std::vector<SDRMFormat> formats, bool cursor) {
    if (formats.empty())
        return SDRMFormat{};

    if (!cursor) {
        /*
            Try to find 10bpp formats first, as they offer better color precision.
            For cursors, don't, as these almost never support that.
        */
        if (auto it = std::find_if(formats.begin(), formats.end(), [](const auto& f) { return f.drmFormat == DRM_FORMAT_ARGB2101010; }); it != formats.end())
            return *it;

        if (auto it = std::find_if(formats.begin(), formats.end(), [](const auto& f) { return f.drmFormat == DRM_FORMAT_XRGB2101010; }); it != formats.end())
            return *it;
    }

    if (auto it = std::find_if(formats.begin(), formats.end(), [](const auto& f) { return f.drmFormat == DRM_FORMAT_ARGB8888; }); it != formats.end())
        return *it;

    if (auto it = std::find_if(formats.begin(), formats.end(), [](const auto& f) { return f.drmFormat == DRM_FORMAT_XRGB8888; }); it != formats.end())
        return *it;

    for (auto& f : formats) {
        auto name = fourccToName(f.drmFormat);

        /* 10 bpp RGB */
        if (name.contains("30"))
            return f;
    }

    for (auto& f : formats) {
        auto name = fourccToName(f.drmFormat);

        /* 8 bpp RGB */
        if (name.contains("24"))
            return f;
    }

    return formats.at(0);
}

Aquamarine::CGBMBuffer::CGBMBuffer(const SAllocatorBufferParams& params, Hyprutils::Memory::CWeakPointer<CGBMAllocator> allocator_,
                                   Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain) :
    allocator(allocator_) {
    if (!allocator)
        return;

    attrs.size   = params.size;
    attrs.format = params.format;
    size         = attrs.size;

    const bool            CURSOR = params.cursor && params.scanout;

    const auto            FORMATS = CURSOR ? swapchain->backendImpl->getCursorFormats() : swapchain->backendImpl->getRenderFormats();

    std::vector<uint64_t> explicitModifiers;

    if (attrs.format == DRM_FORMAT_INVALID) {
        attrs.format = guessFormatFrom(FORMATS, CURSOR).drmFormat;
        if (attrs.format != DRM_FORMAT_INVALID)
            allocator->backend->log(AQ_LOG_DEBUG, std::format("GBM: Automatically selected format {} for new GBM buffer", fourccToName(attrs.format)));
    }

    if (attrs.format == DRM_FORMAT_INVALID) {
        allocator->backend->log(AQ_LOG_ERROR, "GBM: Failed to allocate a GBM buffer: no format found");
        return;
    }

    // check if we can use modifiers. If the requested support has any explicit modifier
    // supported by the primary backend, we can.
    for (auto& f : FORMATS) {
        if (f.drmFormat != attrs.format)
            continue;

        for (auto& m : f.modifiers) {
            if (m == DRM_FORMAT_MOD_INVALID)
                continue;

            explicitModifiers.push_back(m);
        }
    }

    if (explicitModifiers.empty()) {
        // fall back to using a linear buffer.
        // FIXME: Nvidia cannot render to linear buffers.
        explicitModifiers.push_back(DRM_FORMAT_MOD_LINEAR);
    }

    uint32_t flags = GBM_BO_USE_RENDERING;
    if (params.scanout)
        flags |= GBM_BO_USE_SCANOUT;

    if (explicitModifiers.empty()) {
        allocator->backend->log(AQ_LOG_WARNING, "GBM: Using modifier-less allocation");
        bo = gbm_bo_create(allocator->gbmDevice, params.size.x, params.size.y, params.format, flags);
    } else
        bo = gbm_bo_create_with_modifiers(allocator->gbmDevice, params.size.x, params.size.y, params.format, explicitModifiers.data(), explicitModifiers.size());

    if (!bo) {
        allocator->backend->log(AQ_LOG_ERROR, "GBM: Failed to allocate a GBM buffer: bo null");
        return;
    }

    attrs.planes   = gbm_bo_get_plane_count(bo);
    attrs.modifier = gbm_bo_get_modifier(bo);

    for (size_t i = 0; i < (size_t)attrs.planes; ++i) {
        attrs.strides.at(i) = gbm_bo_get_stride_for_plane(bo, i);
        attrs.offsets.at(i) = gbm_bo_get_offset(bo, i);
        attrs.fds.at(i)     = gbm_bo_get_fd_for_plane(bo, i);

        if (attrs.fds.at(i) < 0) {
            allocator->backend->log(AQ_LOG_ERROR, std::format("GBM: Failed to query fd for plane {}", i));
            for (size_t j = 0; j < i; ++j) {
                close(attrs.fds.at(j));
            }
            attrs.planes = 0;
            return;
        }
    }

    attrs.success = true;

    auto modName = drmGetFormatModifierName(attrs.modifier);

    allocator->backend->log(
        AQ_LOG_DEBUG,
        std::format("GBM: Allocated a new buffer with size {} and format {} with modifier {}", attrs.size, fourccToName(attrs.format), modName ? modName : "Unknown"));

    free(modName);
}

Aquamarine::CGBMBuffer::~CGBMBuffer() {
    if (bo)
        gbm_bo_destroy(bo);
    for (size_t i = 0; i < (size_t)attrs.planes; i++)
        close(attrs.fds.at(i));
}

eBufferCapability Aquamarine::CGBMBuffer::caps() {
    return (Aquamarine::eBufferCapability)0;
}

eBufferType Aquamarine::CGBMBuffer::type() {
    return Aquamarine::eBufferType::BUFFER_TYPE_DMABUF;
}

void Aquamarine::CGBMBuffer::update(const Hyprutils::Math::CRegion& damage) {
    ;
}

bool Aquamarine::CGBMBuffer::isSynchronous() {
    return false;
}

bool Aquamarine::CGBMBuffer::good() {
    return true;
}

SDMABUFAttrs Aquamarine::CGBMBuffer::dmabuf() {
    return attrs;
}

SP<CGBMAllocator> Aquamarine::CGBMAllocator::create(int drmfd_, Hyprutils::Memory::CWeakPointer<CBackend> backend_) {
    uint64_t capabilities = 0;
    if (drmGetCap(drmfd_, DRM_CAP_PRIME, &capabilities) || !(capabilities & DRM_PRIME_CAP_EXPORT)) {
        backend_->log(AQ_LOG_ERROR, "Cannot create a GBM Allocator: PRIME export is not supported by the gpu.");
        return nullptr;
    }

    auto allocator = SP<CGBMAllocator>(new CGBMAllocator(drmfd_, backend_));

    if (!allocator->gbmDevice) {
        backend_->log(AQ_LOG_ERROR, "Cannot create a GBM Allocator: gbm failed to create a device.");
        return nullptr;
    }

    backend_->log(AQ_LOG_DEBUG, std::format("Created a GBM allocator with drm fd {}", drmfd_));

    allocator->self = allocator;

    return allocator;
}

Aquamarine::CGBMAllocator::CGBMAllocator(int fd_, Hyprutils::Memory::CWeakPointer<CBackend> backend_) : fd(fd_), backend(backend_) {
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

SP<IBuffer> Aquamarine::CGBMAllocator::acquire(const SAllocatorBufferParams& params, Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain_) {
    if (params.size.x < 1 || params.size.y < 1) {
        backend->log(AQ_LOG_ERROR, std::format("Couldn't allocate a gbm buffer with invalid size {}", params.size));
        return nullptr;
    }

    auto newBuffer = SP<CGBMBuffer>(new CGBMBuffer(params, self, swapchain_));

    if (!newBuffer->good()) {
        backend->log(AQ_LOG_ERROR, std::format("Couldn't allocate a gbm buffer with size {} and format {}", params.size, fourccToName(params.format)));
        return nullptr;
    }

    buffers.emplace_back(newBuffer);
    std::erase_if(buffers, [](const auto& b) { return b.expired(); });
    return newBuffer;
}

Hyprutils::Memory::CSharedPointer<CBackend> Aquamarine::CGBMAllocator::getBackend() {
    return backend.lock();
}
