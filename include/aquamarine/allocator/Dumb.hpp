#pragma once

#include "Allocator.hpp"

namespace Aquamarine {
    class CDumbAllocator;
    class CBackend;
    class CSwapchain;

    class CDumbBuffer : public IBuffer {
      public:
        virtual ~CDumbBuffer();

        virtual eBufferCapability                      caps();
        virtual eBufferType                            type();
        virtual void                                   update(const Hyprutils::Math::CRegion& damage);
        virtual bool                                   isSynchronous();
        virtual bool                                   good();
        virtual SSHMAttrs                              shm();
        virtual std::tuple<uint8_t*, uint32_t, size_t> beginDataPtr(uint32_t flags);
        virtual void                                   endDataPtr();

      private:
        CDumbBuffer(const SAllocatorBufferParams& params, Hyprutils::Memory::CWeakPointer<CDumbAllocator> allocator_, Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain);

        Hyprutils::Memory::CWeakPointer<CDumbAllocator> allocator;

        // dumb stuff
        int       drmFd  = -1;
        uint32_t  handle = 0;
        void*     data   = nullptr;
        size_t    length = 0;

        SSHMAttrs attrs{.success = false};

        friend class CDumbAllocator;
    };

    class CDumbAllocator : public IAllocator {
      public:
        ~CDumbAllocator();
        static Hyprutils::Memory::CSharedPointer<CDumbAllocator> create(int drmfd_, Hyprutils::Memory::CWeakPointer<CBackend> backend_);

        virtual Hyprutils::Memory::CSharedPointer<IBuffer>       acquire(const SAllocatorBufferParams& params, Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain_);
        virtual Hyprutils::Memory::CSharedPointer<CBackend>      getBackend();
        virtual int                                              drmFD();

        //
        Hyprutils::Memory::CWeakPointer<CDumbAllocator> self;

      private:
        CDumbAllocator(int drmfd_, Hyprutils::Memory::CWeakPointer<CBackend> backend_);

        // a vector purely for tracking (debugging) the buffers and nothing more
        std::vector<Hyprutils::Memory::CWeakPointer<CDumbBuffer>> buffers;

        int                                                       fd = -1;
        Hyprutils::Memory::CWeakPointer<CBackend>                 backend;

        friend class CDumbBuffer;
        friend class CDRMRenderer;
    };
};
