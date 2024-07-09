#pragma once

#include "Allocator.hpp"

namespace Aquamarine {

    class IBackendImplementation;

    struct SSwapchainOptions {
        size_t                    length = 0;
        Hyprutils::Math::Vector2D size;
        uint32_t                  format  = DRM_FORMAT_INVALID; // if you leave this on invalid, the swapchain will choose an appropriate format (and modifier) for you.
        bool                      scanout = false, cursor = false /* requires scanout = true */, multigpu = false /* if true, will force linear */;
    };

    class CSwapchain {
      public:
        static Hyprutils::Memory::CSharedPointer<CSwapchain> create(Hyprutils::Memory::CSharedPointer<IAllocator>             allocator_,
                                                                    Hyprutils::Memory::CSharedPointer<IBackendImplementation> backendImpl_);

        bool                                                 reconfigure(const SSwapchainOptions& options_);

        bool                                                 contains(Hyprutils::Memory::CSharedPointer<IBuffer> buffer);
        Hyprutils::Memory::CSharedPointer<IBuffer>           next(int* age);
        const SSwapchainOptions&                             currentOptions();
        Hyprutils::Memory::CSharedPointer<IAllocator>        getAllocator();

        // rolls the buffers back, marking the last consumed as the next valid.
        // useful if e.g. a commit fails and we don't wanna write to the previous buffer that is
        // in use.
        void rollback();

      private:
        CSwapchain(Hyprutils::Memory::CSharedPointer<IAllocator> allocator_, Hyprutils::Memory::CSharedPointer<IBackendImplementation> backendImpl_);

        bool fullReconfigure(const SSwapchainOptions& options_);
        bool resize(size_t newSize);

        //
        Hyprutils::Memory::CWeakPointer<CSwapchain>             self;
        SSwapchainOptions                                       options;
        Hyprutils::Memory::CSharedPointer<IAllocator>           allocator;
        Hyprutils::Memory::CWeakPointer<IBackendImplementation> backendImpl;
        std::vector<Hyprutils::Memory::CSharedPointer<IBuffer>> buffers;
        int                                                     lastAcquired = 0;

        friend class CGBMBuffer;
    };
};
