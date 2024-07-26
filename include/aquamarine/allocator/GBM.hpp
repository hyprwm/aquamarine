#pragma once

#include "Allocator.hpp"

struct gbm_device;
struct gbm_bo;

namespace Aquamarine {
    class CGBMAllocator;
    class CBackend;
    class CSwapchain;

    class CGBMBuffer : public IBuffer {
      public:
        virtual ~CGBMBuffer();

        virtual eBufferCapability                      caps();
        virtual eBufferType                            type();
        virtual void                                   update(const Hyprutils::Math::CRegion& damage);
        virtual bool                                   isSynchronous();
        virtual bool                                   good();
        virtual SDMABUFAttrs                           dmabuf();
        virtual std::tuple<uint8_t*, uint32_t, size_t> beginDataPtr(uint32_t flags);
        virtual void                                   endDataPtr();

      private:
        CGBMBuffer(const SAllocatorBufferParams& params, Hyprutils::Memory::CWeakPointer<CGBMAllocator> allocator_, Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain);

        Hyprutils::Memory::CWeakPointer<CGBMAllocator> allocator;

        // gbm stuff
        gbm_bo*      bo         = nullptr;
        void*        boBuffer   = nullptr;
        void*        gboMapping = nullptr;
        SDMABUFAttrs attrs{.success = false};

        friend class CGBMAllocator;
    };

    class CGBMAllocator : public IAllocator {
      public:
        ~CGBMAllocator();
        static Hyprutils::Memory::CSharedPointer<CGBMAllocator> create(int drmfd_, Hyprutils::Memory::CWeakPointer<CBackend> backend_);

        virtual Hyprutils::Memory::CSharedPointer<IBuffer>      acquire(const SAllocatorBufferParams& params, Hyprutils::Memory::CSharedPointer<CSwapchain> swapchain_);
        virtual Hyprutils::Memory::CSharedPointer<CBackend>     getBackend();
        virtual int                                             drmFD();

        //
        Hyprutils::Memory::CWeakPointer<CGBMAllocator> self;

      private:
        CGBMAllocator(int fd_, Hyprutils::Memory::CWeakPointer<CBackend> backend_);

        // a vector purely for tracking (debugging) the buffers and nothing more
        std::vector<Hyprutils::Memory::CWeakPointer<CGBMBuffer>> buffers;

        int                                                      fd = -1;
        Hyprutils::Memory::CWeakPointer<CBackend>                backend;

        // gbm stuff
        gbm_device* gbmDevice            = nullptr;
        std::string gbmDeviceBackendName = "";
        std::string drmName              = "";

        friend class CGBMBuffer;
        friend class CDRMRenderer;
    };
};
