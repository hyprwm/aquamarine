#pragma once

#include "Allocator.hpp"

namespace Aquamarine {
    class CDRMDumbAllocator;
    class CBackend;
    class CSwapchain;

    class CDRMDumbBuffer : public IBuffer {
      public:
        virtual ~CDRMDumbBuffer();

        virtual eBufferCapability                      caps();
        virtual eBufferType                            type();
        virtual void                                   update(const Hyprutils::Math::CRegion& damage);
        virtual bool                                   isSynchronous();
        virtual bool                                   good();
        virtual SSHMAttrs                              shm();
        virtual std::tuple<uint8_t*, uint32_t, size_t> beginDataPtr(uint32_t flags);
        virtual void                                   endDataPtr();
        virtual uint32_t                               drmHandle();

      private:
        CDRMDumbBuffer(const SAllocatorBufferParams& params, Hyprutils::Memory::CWeakPointer<CDRMDumbAllocator> allocator_,
                       Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain);

        Hyprutils::Memory::CWeakPointer<CDRMDumbAllocator> allocator;

        //
        uint32_t                  drmID = 0;
        Hyprutils::Math::Vector2D pixelSize;
        uint32_t                  stride = 0, handle = 0;
        uint64_t                  size = 0;
        uint8_t*                  data = nullptr;

        //
        SSHMAttrs attrs{.success = false};

        friend class CDRMDumbAllocator;
    };

    class CDRMDumbAllocator : public IAllocator {
      public:
        ~CDRMDumbAllocator();
        static Hyprutils::Memory::CSharedPointer<CDRMDumbAllocator> create(int drmfd_, Hyprutils::Memory::CWeakPointer<CBackend> backend_);

        virtual Hyprutils::Memory::CSharedPointer<IBuffer>          acquire(const SAllocatorBufferParams& params, Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain_);
        virtual Hyprutils::Memory::CSharedPointer<CBackend>         getBackend();
        virtual int                                                 drmFD();
        virtual eAllocatorType                                      type();

        //
        Hyprutils::Memory::CWeakPointer<CDRMDumbAllocator> self;

      private:
        CDRMDumbAllocator(int fd_, Hyprutils::Memory::CWeakPointer<CBackend> backend_);

        // a vector purely for tracking (debugging) the buffers and nothing more
        std::vector<Hyprutils::Memory::CWeakPointer<CDRMDumbBuffer>> buffers;

        Hyprutils::Memory::CWeakPointer<CBackend>                    backend;

        int                                                          drmfd = -1;

        friend class CDRMDumbBuffer;
        friend class CDRMRenderer;
    };
};
