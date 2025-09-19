#pragma once

#include "./Backend.hpp"
#include "../allocator/Swapchain.hpp"
#include "../output/Output.hpp"
#include <hyprutils/memory/WeakPtr.hpp>

namespace Aquamarine {
    class CBackend;
    class IAllocator;

    class CNullBackend : public IBackendImplementation {
      public:
        virtual ~CNullBackend();
        virtual eBackendType                                               type();
        virtual bool                                                       start();
        virtual std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>>    pollFDs();
        virtual int                                                        drmFD();
        virtual bool                                                       dispatchEvents();
        virtual uint32_t                                                   capabilities();
        virtual bool                                                       setCursor(Hyprutils::Memory::CSharedPointer<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot);
        virtual void                                                       onReady();
        virtual std::vector<SDRMFormat>                                    getRenderFormats();
        virtual std::vector<SDRMFormat>                                    getCursorFormats();
        virtual bool                                                       createOutput(const std::string& name = "");
        virtual Hyprutils::Memory::CSharedPointer<IAllocator>              preferredAllocator();
        virtual std::vector<Hyprutils::Memory::CSharedPointer<IAllocator>> getAllocators();
        virtual Hyprutils::Memory::CWeakPointer<IBackendImplementation>    getPrimary();

        Hyprutils::Memory::CWeakPointer<CNullBackend>                      self;
        virtual int                                                        drmRenderNodeFD();

        void                                                               setFormats(const std::vector<SDRMFormat>& fmts);

      private:
        CNullBackend(Hyprutils::Memory::CSharedPointer<CBackend> backend_);

        Hyprutils::Memory::CWeakPointer<CBackend> backend;

        std::vector<SDRMFormat>                   m_formats;

        friend class CBackend;
        friend class CHeadlessOutput;
    };
};
