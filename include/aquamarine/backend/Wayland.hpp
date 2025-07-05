#pragma once

#include "./Backend.hpp"
#include "../allocator/Swapchain.hpp"
#include "../output/Output.hpp"
#include "../input/Input.hpp"
#include <hyprutils/memory/WeakPtr.hpp>
#include <wayland-client.h>
#include <wayland.hpp>
#include <xdg-shell.hpp>
#include <linux-dmabuf-v1.hpp>
#include <xdg-decoration-unstable-v1.hpp>
#include <tuple>

namespace Aquamarine {
    class CBackend;
    class CWaylandBackend;
    class CWaylandOutput;
    class CWaylandPointer;

    typedef std::function<void(void)> FIdleCallback;

    class CWaylandBuffer {
      public:
        CWaylandBuffer(Hyprutils::Memory::CSharedPointer<IBuffer> buffer_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_);
        ~CWaylandBuffer();
        bool good();

        bool pendingRelease = false;

      private:
        struct {
            Hyprutils::Memory::CSharedPointer<CCWlBuffer> buffer;
        } waylandState;

        Hyprutils::Memory::CWeakPointer<IBuffer>         buffer;
        Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend;

        friend class CWaylandOutput;
    };

    class CWaylandOutput : public IOutput {
      public:
        virtual ~CWaylandOutput();
        virtual bool                                                      commit();
        virtual bool                                                      test();
        virtual Hyprutils::Memory::CSharedPointer<IBackendImplementation> getBackend();
        virtual bool                                                      setCursor(Hyprutils::Memory::CSharedPointer<IBuffer> buffer, const Hyprutils::Math::Vector2D& hotspot);
        virtual void                                                      moveCursor(const Hyprutils::Math::Vector2D& coord, bool skipSchedule = false);
        virtual void                                                      scheduleFrame(const scheduleFrameReason reason = AQ_SCHEDULE_UNKNOWN);
        virtual Hyprutils::Math::Vector2D                                 cursorPlaneSize();
        virtual bool                                                      destroy();
        virtual std::vector<SDRMFormat>                                   getRenderFormats();

        Hyprutils::Memory::CWeakPointer<CWaylandOutput>                   self;

      private:
        CWaylandOutput(const std::string& name_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_);

        Hyprutils::Memory::CWeakPointer<CWaylandBackend>  backend;

        Hyprutils::Memory::CSharedPointer<CWaylandBuffer> wlBufferFromBuffer(Hyprutils::Memory::CSharedPointer<IBuffer> buffer);

        void                                              sendFrameAndSetCallback();
        void                                              onFrameDone();
        void                                              onEnter(Hyprutils::Memory::CSharedPointer<CCWlPointer> pointer, uint32_t serial);

        // frame loop
        bool frameScheduledWhileWaiting = false;
        bool readyForFrameCallback      = false; // true after attaching a buffer
        bool frameScheduled             = false;

        struct {
            std::vector<std::pair<Hyprutils::Memory::CWeakPointer<IBuffer>, Hyprutils::Memory::CSharedPointer<CWaylandBuffer>>> buffers;
        } backendState;

        struct {
            Hyprutils::Memory::CSharedPointer<IBuffer>     cursorBuffer;
            Hyprutils::Memory::CSharedPointer<CCWlSurface> cursorSurface;
            Hyprutils::Memory::CSharedPointer<CCWlBuffer>  cursorWlBuffer;
            uint32_t                                       serial = 0;
            Hyprutils::Math::Vector2D                      hotspot;
        } cursorState;

        struct {
            Hyprutils::Memory::CSharedPointer<CCWlSurface>   surface;
            Hyprutils::Memory::CSharedPointer<CCXdgSurface>  xdgSurface;
            Hyprutils::Memory::CSharedPointer<CCXdgToplevel> xdgToplevel;
            Hyprutils::Memory::CSharedPointer<CCWlCallback>  frameCallback;
            Hyprutils::Memory::CSharedPointer<CCZxdgToplevelDecorationV1> decoration;
        } waylandState;

        friend class CWaylandBackend;
        friend class CWaylandPointer;
    };

    class CWaylandKeyboard : public IKeyboard {
      public:
        CWaylandKeyboard(Hyprutils::Memory::CSharedPointer<CCWlKeyboard> keyboard_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_);
        virtual ~CWaylandKeyboard();

        virtual const std::string&                       getName();

        Hyprutils::Memory::CSharedPointer<CCWlKeyboard>  keyboard;
        Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend;

      private:
        const std::string name = "wl_keyboard";
    };

    class CWaylandPointer : public IPointer {
      public:
        CWaylandPointer(Hyprutils::Memory::CSharedPointer<CCWlPointer> pointer_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_);
        virtual ~CWaylandPointer();

        virtual const std::string&                       getName();

        Hyprutils::Memory::CSharedPointer<CCWlPointer>   pointer;
        Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend;

      private:
        const std::string name = "wl_pointer";
    };

    class CWaylandBackend : public IBackendImplementation {
      public:
        virtual ~CWaylandBackend();
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

        Hyprutils::Memory::CWeakPointer<CWaylandBackend>                   self;

      private:
        CWaylandBackend(Hyprutils::Memory::CSharedPointer<CBackend> backend);

        void initSeat();
        void initShell();
        bool initDmabuf();

        //
        Hyprutils::Memory::CWeakPointer<CBackend>                        backend;
        std::vector<Hyprutils::Memory::CSharedPointer<CWaylandOutput>>   outputs;
        std::vector<Hyprutils::Memory::CSharedPointer<CWaylandKeyboard>> keyboards;
        std::vector<Hyprutils::Memory::CSharedPointer<CWaylandPointer>>  pointers;
        std::vector<FIdleCallback>                                       idleCallbacks;

        // pointer focus
        Hyprutils::Memory::CWeakPointer<CWaylandOutput> focusedOutput;
        uint32_t                                        lastEnterSerial = 0;

        // state
        size_t lastOutputID = 0;

        // dmabuf formats
        std::vector<SDRMFormat> dmabufFormats;

        struct {
            wl_display* display = nullptr;

            // hw-s types
            Hyprutils::Memory::CSharedPointer<CCWlRegistry>               registry;
            Hyprutils::Memory::CSharedPointer<CCWlSeat>                   seat;
            Hyprutils::Memory::CSharedPointer<CCWlShm>                    shm;
            Hyprutils::Memory::CSharedPointer<CCXdgWmBase>                xdg;
            Hyprutils::Memory::CSharedPointer<CCWlCompositor>             compositor;
            Hyprutils::Memory::CSharedPointer<CCZwpLinuxDmabufV1>         dmabuf;
            Hyprutils::Memory::CSharedPointer<CCZwpLinuxDmabufFeedbackV1> dmabufFeedback;
            Hyprutils::Memory::CSharedPointer<CCZxdgDecorationManagerV1> decorationManager;

            // control
            bool dmabufFailed = false;
        } waylandState;

        struct {
            int         fd       = -1;
            std::string nodeName = "";
        } drmState;

        friend class CBackend;
        friend class CWaylandKeyboard;
        friend class CWaylandPointer;
        friend class CWaylandOutput;
        friend class CWaylandBuffer;
    };
};
