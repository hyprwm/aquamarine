#include <algorithm>
#include <aquamarine/allocator/GBM.hpp>
#include <aquamarine/backend/Backend.hpp>
#include <aquamarine/backend/DRM.hpp>
#include <aquamarine/allocator/Swapchain.hpp>
#include "FormatUtils.hpp"
#include "Shared.hpp"
#include <xf86drm.h>
#include <gbm.h>
#include <unistd.h>
#include "../backend/drm/Renderer.hpp"

using namespace Aquamarine;
using namespace Hyprutils::Memory;
#define SP CSharedPointer

static SDRMFormat guessFormatFrom(std::vector<SDRMFormat> formats, bool cursor, bool scanout) {
    if (formats.empty())
        return SDRMFormat{};

    if (!cursor) {
        /*
            Try to find 10bpp formats first, as they offer better color precision.
            For cursors, don't, as these almost never support that.
        */
        if (!scanout) {
            if (auto it = std::ranges::find_if(formats, [](const auto& f) { return f.drmFormat == DRM_FORMAT_ARGB2101010 || f.drmFormat == DRM_FORMAT_ABGR2101010; });
                it != formats.end())
                return *it;
        }

        if (auto it = std::ranges::find_if(formats, [](const auto& f) { return f.drmFormat == DRM_FORMAT_XRGB2101010 || f.drmFormat == DRM_FORMAT_XBGR2101010; });
            it != formats.end())
            return *it;
    }

    if (!scanout || cursor /* don't set opaque for cursor plane */) {
        if (auto it = std::ranges::find_if(formats, [](const auto& f) { return f.drmFormat == DRM_FORMAT_ARGB8888 || f.drmFormat == DRM_FORMAT_ABGR8888; }); it != formats.end())
            return *it;
    }

    if (auto it = std::ranges::find_if(formats, [](const auto& f) { return f.drmFormat == DRM_FORMAT_XRGB8888 || f.drmFormat == DRM_FORMAT_XBGR8888; }); it != formats.end())
        return *it;

    for (auto const& f : formats) {
        auto name = fourccToName(f.drmFormat);

        /* 10 bpp RGB */
        if (name.contains("30"))
            return f;
    }

    for (auto const& f : formats) {
        auto name = fourccToName(f.drmFormat);

        /* 8 bpp RGB */
        if (name.contains("24"))
            return f;
    }

    return formats.at(0);
}

Aquamarine::CGBMBuffer::CGBMBuffer(const SAllocatorBufferParams& params, Hyprutils::Memory::CWeakPointer<CGBMAllocator> allocator_,
                                   Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain) : allocator(allocator_) {
    if (!allocator)
        return;

    attrs.size   = params.size;
    attrs.format = params.format;
    size         = attrs.size;

    const bool CURSOR           = params.cursor && params.scanout;
    const bool MULTIGPU         = params.multigpu && params.scanout;
    const bool EXPLICIT_SCANOUT = params.scanout && swapchain->currentOptions().scanoutOutput && !params.multigpu;

    TRACE(allocator->backend->log(AQ_LOG_TRACE,
                                  std::format("GBM: Allocating a buffer: size {}, format {}, cursor: {}, multigpu: {}, scanout: {}", attrs.size, fourccToName(attrs.format), CURSOR,
                                              MULTIGPU, params.scanout)));

    if (EXPLICIT_SCANOUT)
        TRACE(allocator->backend->log(
            AQ_LOG_TRACE, std::format("GBM: Explicit scanout output, output has {} explicit formats", swapchain->currentOptions().scanoutOutput->getRenderFormats().size())));

    const auto FORMATS    = CURSOR ? swapchain->backendImpl->getCursorFormats() :
                                     (EXPLICIT_SCANOUT ? swapchain->currentOptions().scanoutOutput->getRenderFormats() : swapchain->backendImpl->getRenderFormats());
    const auto RENDERABLE = swapchain->backendImpl->getRenderableFormats();

    TRACE(allocator->backend->log(AQ_LOG_TRACE, std::format("GBM: Available formats: {}", FORMATS.size())));

    std::vector<uint64_t> explicitModifiers;

    if (attrs.format == DRM_FORMAT_INVALID) {
        attrs.format = guessFormatFrom(FORMATS, CURSOR, params.scanout).drmFormat;
        if (attrs.format != DRM_FORMAT_INVALID)
            allocator->backend->log(AQ_LOG_DEBUG, std::format("GBM: Automatically selected format {} for new GBM buffer", fourccToName(attrs.format)));
    }

    if (attrs.format == DRM_FORMAT_INVALID) {
        allocator->backend->log(AQ_LOG_ERROR, "GBM: Failed to allocate a GBM buffer: no format found");
        return;
    }

    bool foundFormat = false;
    // check if we can use modifiers. If the requested support has any explicit modifier
    // supported by the primary backend, we can.
    for (auto const& f : FORMATS) {
        if (f.drmFormat != attrs.format)
            continue;

        foundFormat = true;
        for (auto const& m : f.modifiers) {
            if (m == DRM_FORMAT_MOD_INVALID)
                continue;

            if (!RENDERABLE.empty()) {
                TRACE(allocator->backend->log(AQ_LOG_TRACE, std::format("GBM: Renderable has {} formats, clipping", RENDERABLE.size())));
                if (params.scanout && !CURSOR && !MULTIGPU) {
                    // regular scanout plane, check if the format is renderable
                    auto rformat = std::ranges::find_if(RENDERABLE, [f](const auto& e) { return e.drmFormat == f.drmFormat; });

                    if (rformat == RENDERABLE.end()) {
                        TRACE(allocator->backend->log(AQ_LOG_TRACE, std::format("GBM: Dropping format {} as it's not renderable", fourccToName(f.drmFormat))));
                        break;
                    }

                    if (std::find(rformat->modifiers.begin(), rformat->modifiers.end(), m) == rformat->modifiers.end()) {
                        TRACE(allocator->backend->log(AQ_LOG_TRACE, std::format("GBM: Dropping modifier 0x{:x} as it's not renderable", m)));
                        continue;
                    }
                }
            }
            explicitModifiers.push_back(m);
        }
    }

    if (!foundFormat) {
        allocator->backend->log(AQ_LOG_ERROR, std::format("GBM: Failed to allocate a GBM buffer: format {} isn't supported by primary backend", fourccToName(attrs.format)));
        bo = nullptr;
        return;
    }

    // FIXME: Nvidia cannot render to linear buffers. What do?
    // it seems it can import them, but not create? or is it nvidia <-> nvidia thats the trouble?
    // without this blitting on laptops intel/amd <-> nvidia makes eglCreateImageKHR error and
    // fallback to slow cpu copying.
    auto const oldMods = explicitModifiers;
    if (MULTIGPU) {
        allocator->backend->log(AQ_LOG_DEBUG, "GBM: Buffer is marked as multigpu, forcing linear");
        explicitModifiers = {DRM_FORMAT_MOD_LINEAR};
    }

    uint32_t flags = GBM_BO_USE_RENDERING;
    if (params.scanout && !MULTIGPU)
        flags |= GBM_BO_USE_SCANOUT;

    uint64_t modifier = DRM_FORMAT_MOD_INVALID;

    if (explicitModifiers.empty()) {
        allocator->backend->log(AQ_LOG_WARNING, "GBM: Using modifier-less allocation");
        bo = gbm_bo_create(allocator->gbmDevice, attrs.size.x, attrs.size.y, attrs.format, flags);
    } else {
        TRACE(allocator->backend->log(AQ_LOG_TRACE, std::format("GBM: Using modifier-based allocation, modifiers: {}", explicitModifiers.size())));
        for (auto const& mod : explicitModifiers) {
            TRACE(allocator->backend->log(AQ_LOG_TRACE, std::format("GBM: | mod 0x{:x}", mod)));
        }
        bo = gbm_bo_create_with_modifiers2(allocator->gbmDevice, attrs.size.x, attrs.size.y, attrs.format, explicitModifiers.data(), explicitModifiers.size(), flags);

        if (!bo && CURSOR) {
            // allow non-renderable cursor buffer for nvidia
            allocator->backend->log(AQ_LOG_ERROR, "GBM: Allocating with modifiers and flags failed, falling back to modifiers without flags");
            bo = gbm_bo_create_with_modifiers(allocator->gbmDevice, attrs.size.x, attrs.size.y, attrs.format, explicitModifiers.data(), explicitModifiers.size());
        }

        bool useLinear = explicitModifiers.size() == 1 && explicitModifiers[0] == DRM_FORMAT_MOD_LINEAR;
        if (bo) {
            modifier = gbm_bo_get_modifier(bo);
            if (useLinear && modifier == DRM_FORMAT_MOD_INVALID)
                modifier = DRM_FORMAT_MOD_LINEAR;
        } else {
            if (useLinear) {
                flags |= GBM_BO_USE_LINEAR;
                modifier = DRM_FORMAT_MOD_LINEAR;
                allocator->backend->log(AQ_LOG_ERROR, "GBM: Allocating with modifiers failed, falling back to modifier-less allocation");
            } else
                allocator->backend->log(AQ_LOG_ERROR, "GBM: Allocating with modifiers failed, falling back to implicit");
            bo = gbm_bo_create(allocator->gbmDevice, attrs.size.x, attrs.size.y, attrs.format, flags);
        }
    }

    // FIXME: most likely nvidia main gpu on multigpu
    if (!bo) {
        if (oldMods.empty())
            bo = gbm_bo_create(allocator->gbmDevice, attrs.size.x, attrs.size.y, attrs.format, GBM_BO_USE_RENDERING);
        else
            bo = gbm_bo_create_with_modifiers(allocator->gbmDevice, attrs.size.x, attrs.size.y, attrs.format, oldMods.data(), oldMods.size());

        if (!bo) {
            allocator->backend->log(AQ_LOG_ERROR, "GBM: Failed to allocate a GBM buffer: bo null");
            return;
        }

        modifier = gbm_bo_get_modifier(bo);
    }

    if (!bo) {
        allocator->backend->log(AQ_LOG_ERROR, "GBM: Failed to allocate a GBM buffer: bo null");
        return;
    }

    attrs.planes   = gbm_bo_get_plane_count(bo);
    attrs.modifier = modifier;

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

    allocator->backend->log(AQ_LOG_DEBUG,
                            std::format("GBM: Allocated a new buffer with size {} and format {} with modifier {} aka {}", attrs.size, fourccToName(attrs.format), attrs.modifier,
                                        modName ? modName : "Unknown"));

    free(modName);

    if (params.scanout && !MULTIGPU && swapchain->backendImpl->type() == AQ_BACKEND_DRM) {
        // clear the buffer using the DRM renderer to avoid uninitialized mem
        auto impl = (CDRMBackend*)swapchain->backendImpl.get();
        if (impl->rendererState.renderer)
            impl->rendererState.renderer->clearBuffer(this);
    }
}

Aquamarine::CGBMBuffer::~CGBMBuffer() {
    for (size_t i = 0; i < (size_t)attrs.planes; i++) {
        close(attrs.fds.at(i));
    }

    events.destroy.emit();
    if (bo) {
        if (gboMapping)
            gbm_bo_unmap(bo, gboMapping); // FIXME: is it needed before destroy?
        gbm_bo_destroy(bo);
    }
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
    return bo;
}

SDMABUFAttrs Aquamarine::CGBMBuffer::dmabuf() {
    return attrs;
}

std::tuple<uint8_t*, uint32_t, size_t> Aquamarine::CGBMBuffer::beginDataPtr(uint32_t flags) {
    uint32_t stride = 0;
    if (boBuffer)
        allocator->backend->log(AQ_LOG_ERROR, "beginDataPtr is called a second time without calling endDataPtr first. Returning old mapping");
    else
        boBuffer = gbm_bo_map(bo, 0, 0, attrs.size.x, attrs.size.y, flags, &stride, &gboMapping);

    return {(uint8_t*)boBuffer, attrs.format, stride * attrs.size.y};
}

void Aquamarine::CGBMBuffer::endDataPtr() {
    if (gboMapping) {
        gbm_bo_unmap(bo, gboMapping);
        gboMapping = nullptr;
        boBuffer   = nullptr;
    }
}

void CGBMAllocator::destroyBuffers() {
    for (auto& buf : buffers) {
        buf.reset();
    }
}

CGBMAllocator::~CGBMAllocator() {
    if (!gbmDevice)
        return;

    int fd = gbm_device_get_fd(gbmDevice);
    gbm_device_destroy(gbmDevice);

    if (fd < 0)
        return;

    close(fd);
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

Aquamarine::CGBMAllocator::CGBMAllocator(int fd_, Hyprutils::Memory::CWeakPointer<CBackend> backend_) : fd(fd_), backend(backend_), gbmDevice(gbm_create_device(fd_)) {
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

int Aquamarine::CGBMAllocator::drmFD() {
    return fd;
}

eAllocatorType Aquamarine::CGBMAllocator::type() {
    return AQ_ALLOCATOR_TYPE_GBM;
}
