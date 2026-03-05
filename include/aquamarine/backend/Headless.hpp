#pragma once

#include "./Backend.hpp"
#include "../allocator/Swapchain.hpp"
#include "../output/Output.hpp"
#include <hyprutils/memory/WeakPtr.hpp>
#include <hyprutils/os/FileDescriptor.hpp>

namespace Aquamarine {
    class CBackend;
    class CHeadlessBackend;
    class IAllocator;

    class CHeadlessOutput : public IOutput {
      public:
        virtual ~CHeadlessOutput();
        virtual bool                                                      commit();
        virtual bool                                                      test();
        virtual Hyprutils::Memory::CSharedPointer<IBackendImplementation> getBackend();
        virtual void                                                      scheduleFrame(const scheduleFrameReason reason = AQ_SCHEDULE_UNKNOWN);
        virtual bool                                                      destroy();
        virtual std::vector<SDRMFormat>                                   getRenderFormats();
        virtual bool                                                      pendingPageFlip();

        Hyprutils::Memory::CWeakPointer<CHeadlessOutput>                  self;

      private:
        CHeadlessOutput(const std::string& name_, Hyprutils::Memory::CWeakPointer<CHeadlessBackend> backend_);

        Hyprutils::Memory::CWeakPointer<CHeadlessBackend>        backend;

        Hyprutils::Memory::CSharedPointer<std::function<void()>> framecb;
        bool                                                     frameScheduled = false;
        std::chrono::steady_clock::time_point                    lastFrame;

        friend class CHeadlessBackend;
    };

    class CHeadlessBackend : public IBackendImplementation {
      public:
        virtual ~CHeadlessBackend();
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

        Hyprutils::Memory::CWeakPointer<CHeadlessBackend>                  self;
        virtual int                                                        drmRenderNodeFD();

      private:
        CHeadlessBackend(Hyprutils::Memory::CSharedPointer<CBackend> backend_);

        Hyprutils::Memory::CWeakPointer<CBackend>                       backend;
        std::vector<Hyprutils::Memory::CSharedPointer<CHeadlessOutput>> outputs;

        size_t                                                          outputIDCounter = 0;

        class CTimer {
          public:
            std::chrono::steady_clock::time_point when;
            std::function<void(void)>             what;
            bool                                  expired();
        };

        struct {
            Hyprutils::OS::CFileDescriptor timerfd;
            std::vector<CTimer>            timers;
        } timers;

        void dispatchTimers();
        void updateTimerFD();
        void addTimer(std::chrono::steady_clock::time_point when, std::function<void(void)> what);

        friend class CBackend;
        friend class CHeadlessOutput;
    };
};
