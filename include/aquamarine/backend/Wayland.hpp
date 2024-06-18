#pragma once

#include "./Backend.hpp"
#include "../output/Output.hpp"
#include "../input/Input.hpp"
#include <hyprutils/memory/WeakPtr.hpp>
#include <wayland-client.h>
#include <wayland.hpp>
#include <xdg-shell.hpp>

namespace Aquamarine {
    class CBackend;
    class CWaylandBackend;

    class CWaylandOutput : public IOutput {
      public:
        virtual ~CWaylandOutput();
        virtual void commit();

      private:
        CWaylandOutput(const std::string& name_, Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend_);

        std::string                                      name;
        Hyprutils::Memory::CWeakPointer<CWaylandBackend> backend;

        struct {
            Hyprutils::Memory::CSharedPointer<CWlSurface>   surface;
            Hyprutils::Memory::CSharedPointer<CXdgSurface>  xdgSurface;
            Hyprutils::Memory::CSharedPointer<CXdgToplevel> xdgToplevel;
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
        virtual bool                                     dispatchEvents();

        Hyprutils::Memory::CWeakPointer<CWaylandBackend> self;

      private:
        CWaylandBackend(Hyprutils::Memory::CSharedPointer<CBackend> backend);

        void initSeat();
        void initShell();
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
            Hyprutils::Memory::CSharedPointer<CWlRegistry>   registry;
            Hyprutils::Memory::CSharedPointer<CWlSeat>       seat;
            Hyprutils::Memory::CSharedPointer<CXdgWmBase>    xdg;
            Hyprutils::Memory::CSharedPointer<CWlCompositor> compositor;
        } waylandState;

        friend class CBackend;
        friend class CWaylandKeyboard;
        friend class CWaylandPointer;
        friend class CWaylandOutput;
    };
};
