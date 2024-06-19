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
#include <tuple>

namespace Aquamarine {
    class CBackend;
    class CWaylandBackend;
    class CWaylandOutput;

    class CWaylandBuffer {
      public:
        CWaylandBuffer(Hyprutils::Memory::CSharedPointer<IBuffer> buffer_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_);
        ~CWaylandBuffer();
        bool good();

        bool pendingRelease = false;

      private:
        struct {
            Hyprutils::Memory::CSharedPointer<CWlBuffer> buffer;
        } waylandState;

        Hyprutils::Memory::CWeakPointer<IBuffer>         buffer;
        Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend;

        friend class CWaylandOutput;
    };

    class CWaylandOutput : public IOutput {
      public:
        virtual ~CWaylandOutput();
        virtual bool commit();

      private:
        CWaylandOutput(const std::string& name_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_);

        std::string                                       name;
        Hyprutils::Memory::CWeakPointer<CWaylandBackend>  backend;

        Hyprutils::Memory::CSharedPointer<CWaylandBuffer> wlBufferFromBuffer(Hyprutils::Memory::CSharedPointer<IBuffer> buffer);

        void                                              sendFrameAndSetCallback();

        struct {
            std::vector<std::pair<Hyprutils::Memory::CWeakPointer<IBuffer>, Hyprutils::Memory::CSharedPointer<CWaylandBuffer>>> buffers;
        } backendState;

        struct {
            Hyprutils::Memory::CSharedPointer<CWlSurface>   surface;
            Hyprutils::Memory::CSharedPointer<CXdgSurface>  xdgSurface;
            Hyprutils::Memory::CSharedPointer<CXdgToplevel> xdgToplevel;
            Hyprutils::Memory::CSharedPointer<CWlCallback>  frameCallback;
        } waylandState;

        friend class CWaylandBackend;
        friend class CWaylandPointer;
    };

    class CWaylandKeyboard : public IKeyboard {
      public:
        CWaylandKeyboard(Hyprutils::Memory::CSharedPointer<CWlKeyboard> keyboard_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_);
        virtual ~CWaylandKeyboard();

        virtual const std::string&                       getName();

        Hyprutils::Memory::CSharedPointer<CWlKeyboard>   keyboard;
        Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend;

      private:
        const std::string name = "wl_keyboard";
    };

    class CWaylandPointer : public IPointer {
      public:
        CWaylandPointer(Hyprutils::Memory::CSharedPointer<CWlPointer> pointer_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_);
        virtual ~CWaylandPointer();

        virtual const std::string&                       getName();

        Hyprutils::Memory::CSharedPointer<CWlPointer>    pointer;
        Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend;

      private:
        const std::string name = "wl_pointer";
    };

    class CWaylandBackend : public IBackendImplementation {
      public:
        virtual ~CWaylandBackend();
        virtual eBackendType                             type();
        virtual bool                                     start();
        virtual int                                      pollFD();
        virtual int                                      drmFD();
        virtual bool                                     dispatchEvents();

        Hyprutils::Memory::CWeakPointer<CWaylandBackend> self;

      private:
        CWaylandBackend(Hyprutils::Memory::CSharedPointer<CBackend> backend);

        void initSeat();
        void initShell();
        bool initDmabuf();
        void createOutput(const std::string& szName);

        //
        Hyprutils::Memory::CWeakPointer<CBackend>                        backend;
        std::vector<Hyprutils::Memory::CSharedPointer<CWaylandOutput>>   outputs;
        std::vector<Hyprutils::Memory::CSharedPointer<CWaylandKeyboard>> keyboards;
        std::vector<Hyprutils::Memory::CSharedPointer<CWaylandPointer>>  pointers;

        // pointer focus
        Hyprutils::Memory::CWeakPointer<CWaylandOutput> focusedOutput;
        uint32_t                                        lastEnterSerial = 0;

        struct {
            wl_display* display = nullptr;

            // hw-s types
            Hyprutils::Memory::CSharedPointer<CWlRegistry>               registry;
            Hyprutils::Memory::CSharedPointer<CWlSeat>                   seat;
            Hyprutils::Memory::CSharedPointer<CXdgWmBase>                xdg;
            Hyprutils::Memory::CSharedPointer<CWlCompositor>             compositor;
            Hyprutils::Memory::CSharedPointer<CZwpLinuxDmabufV1>         dmabuf;
            Hyprutils::Memory::CSharedPointer<CZwpLinuxDmabufFeedbackV1> dmabufFeedback;

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
