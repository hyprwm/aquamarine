#pragma once

#include <hyprutils/memory/SharedPtr.hpp>
#include "../buffer/Buffer.hpp"
#include <drm_fourcc.h>

namespace Aquamarine {
    class CBackend;

    struct SAllocatorBufferParams {
        Hyprutils::Math::Vector2D size;
        uint32_t                  format  = DRM_FORMAT_INVALID;
        bool                      scanout = false;
    };

    class IAllocator {
      public:
        virtual Hyprutils::Memory::CSharedPointer<IBuffer>  acquire(const SAllocatorBufferParams& params) = 0;
        virtual Hyprutils::Memory::CSharedPointer<CBackend> getBackend()                                  = 0;
    };
};
