#pragma once

#include "Allocator.hpp"

namespace Aquamarine {

    struct SSwapchainOptions {
        size_t                    length = 0;
        Hyprutils::Math::Vector2D size;
        uint32_t                  format  = DRM_FORMAT_INVALID;
        bool                      scanout = false;
    };

    class CSwapchain {
      public:
        CSwapchain(Hyprutils::Memory::CSharedPointer<IAllocator> allocator_);

        bool                                       reconfigure(const SSwapchainOptions& options_);

        bool                                       contains(Hyprutils::Memory::CSharedPointer<IBuffer> buffer);
        Hyprutils::Memory::CSharedPointer<IBuffer> next(int* age);
        const SSwapchainOptions&                   currentOptions();

      private:
        bool fullReconfigure(const SSwapchainOptions& options_);
        bool resize(size_t newSize);

        //
        SSwapchainOptions                                       options;
        Hyprutils::Memory::CSharedPointer<IAllocator>           allocator;
        std::vector<Hyprutils::Memory::CSharedPointer<IBuffer>> buffers;
        int                                                     lastAcquired = 0;
    };
};
