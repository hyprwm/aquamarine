#pragma once

#include <hyprutils/memory/SharedPtr.hpp>
#include "../buffer/Buffer.hpp"
#include <drm_fourcc.h>

namespace Aquamarine {
    class CBackend;
    class CSwapchain;

    struct SAllocatorBufferParams {
        Hyprutils::Math::Vector2D size;
        uint32_t                  format  = DRM_FORMAT_INVALID;
        bool                      scanout = false, cursor = false, multigpu = false;
    };

    enum eAllocatorType {
        AQ_ALLOCATOR_TYPE_GBM = 0,
        AQ_ALLOCATOR_TYPE_DRM_DUMB,
    };

    class IAllocator {
      public:
        virtual ~IAllocator()                                                                                                                                      = default;
        virtual Hyprutils::Memory::CSharedPointer<IBuffer>  acquire(const SAllocatorBufferParams& params, Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain) = 0;
        virtual Hyprutils::Memory::CSharedPointer<CBackend> getBackend()                                                                                           = 0;
        virtual int                                         drmFD()                                                                                                = 0;
        virtual eAllocatorType                              type()                                                                                                 = 0;
        virtual void                                        destroyBuffers();
    };
};
