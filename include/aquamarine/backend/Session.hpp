#pragma once

#include <sys/types.h>
#include <hyprutils/signal/Signal.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include "../input/Input.hpp"
#include <vector>

struct udev;
struct udev_monitor;
struct udev_device;
struct libseat;
struct libinput;
struct libinput_event;
struct libinput_device;
struct libinput_tablet_tool;

namespace Aquamarine {
    class CBackend;
    class CSession;
    class CLibinputDevice;
    struct SPollFD;

    class CSessionDevice {
      public:
        CSessionDevice(Hyprutils::Memory::CSharedPointer<CSession> session_, const std::string& path_);
        ~CSessionDevice();

        static Hyprutils::Memory::CSharedPointer<CSessionDevice> openIfKMS(Hyprutils::Memory::CSharedPointer<CSession> session_, const std::string& path_);

        bool                                                     supportsKMS();

        int                                                      fd       = -1;
        int                                                      deviceID = -1;
        dev_t                                                    dev;
        std::string                                              path;

        enum eChangeEventType : uint32_t {
            AQ_SESSION_EVENT_CHANGE_HOTPLUG = 0,
            AQ_SESSION_EVENT_CHANGE_LEASE,
        };

        struct SChangeEvent {
            eChangeEventType type = AQ_SESSION_EVENT_CHANGE_HOTPLUG;

            struct {
                uint32_t connectorID = 0, propID = 0;
            } hotplug;
        };

        struct {
            Hyprutils::Signal::CSignal change;
            Hyprutils::Signal::CSignal remove;
        } events;

      private:
        Hyprutils::Memory::CWeakPointer<CSession> session;
    };

    class CLibinputKeyboard : public IKeyboard {
      public:
        CLibinputKeyboard(Hyprutils::Memory::CSharedPointer<CLibinputDevice> dev);
        virtual ~CLibinputKeyboard() {
            ;
        }

        virtual libinput_device*   getLibinputHandle();
        virtual const std::string& getName();
        virtual void               updateLEDs(uint32_t leds);

      private:
        Hyprutils::Memory::CWeakPointer<CLibinputDevice> device;

        friend class CLibinputDevice;
    };

    class CLibinputMouse : public IPointer {
      public:
        CLibinputMouse(Hyprutils::Memory::CSharedPointer<CLibinputDevice> dev);
        virtual ~CLibinputMouse() {
            ;
        }

        virtual libinput_device*   getLibinputHandle();
        virtual const std::string& getName();

      private:
        Hyprutils::Memory::CWeakPointer<CLibinputDevice> device;

        friend class CLibinputDevice;
    };

    class CLibinputTouch : public ITouch {
      public:
        CLibinputTouch(Hyprutils::Memory::CSharedPointer<CLibinputDevice> dev);
        virtual ~CLibinputTouch() {
            ;
        }

        virtual libinput_device*   getLibinputHandle();
        virtual const std::string& getName();

      private:
        Hyprutils::Memory::CWeakPointer<CLibinputDevice> device;

        friend class CLibinputDevice;
    };

    class CLibinputSwitch : public ISwitch {
      public:
        CLibinputSwitch(Hyprutils::Memory::CSharedPointer<CLibinputDevice> dev);
        virtual ~CLibinputSwitch() {
            ;
        }

        virtual libinput_device*   getLibinputHandle();
        virtual const std::string& getName();

        eSwitchType                type  = AQ_SWITCH_TYPE_UNKNOWN;
        bool                       state = false;

      private:
        Hyprutils::Memory::CWeakPointer<CLibinputDevice> device;

        friend class CLibinputDevice;
    };

    class CLibinputTablet : public ITablet {
      public:
        CLibinputTablet(Hyprutils::Memory::CSharedPointer<CLibinputDevice> dev);
        virtual ~CLibinputTablet() {
            ;
        }

        virtual libinput_device*   getLibinputHandle();
        virtual const std::string& getName();

      private:
        Hyprutils::Memory::CWeakPointer<CLibinputDevice> device;

        friend class CLibinputDevice;
    };

    class CLibinputTabletTool : public ITabletTool {
      public:
        CLibinputTabletTool(Hyprutils::Memory::CSharedPointer<CLibinputDevice> dev, libinput_tablet_tool* tool);
        virtual ~CLibinputTabletTool();

        virtual libinput_device*   getLibinputHandle();
        virtual const std::string& getName();

      private:
        Hyprutils::Memory::CWeakPointer<CLibinputDevice> device;
        libinput_tablet_tool*                            libinputTool = nullptr;

        friend class CLibinputDevice;
    };

    class CLibinputTabletPad : public ITabletPad {
      public:
        CLibinputTabletPad(Hyprutils::Memory::CSharedPointer<CLibinputDevice> dev);
        virtual ~CLibinputTabletPad();

        virtual libinput_device*   getLibinputHandle();
        virtual const std::string& getName();

      private:
        Hyprutils::Memory::CWeakPointer<CLibinputDevice>               device;

        Hyprutils::Memory::CSharedPointer<ITabletPad::STabletPadGroup> createGroupFromID(int id);

        friend class CLibinputDevice;
    };

    class CLibinputDevice {
      public:
        CLibinputDevice(libinput_device* device, Hyprutils::Memory::CWeakPointer<CSession> session_);
        ~CLibinputDevice();

        void                                                                init();

        libinput_device*                                                    device;
        Hyprutils::Memory::CWeakPointer<CLibinputDevice>                    self;
        Hyprutils::Memory::CWeakPointer<CSession>                           session;
        std::string                                                         name;

        Hyprutils::Memory::CSharedPointer<CLibinputKeyboard>                keyboard;
        Hyprutils::Memory::CSharedPointer<CLibinputMouse>                   mouse;
        Hyprutils::Memory::CSharedPointer<CLibinputTouch>                   touch;
        Hyprutils::Memory::CSharedPointer<CLibinputSwitch>                  switchy; // :)
        Hyprutils::Memory::CSharedPointer<CLibinputTablet>                  tablet;
        Hyprutils::Memory::CSharedPointer<CLibinputTabletPad>               tabletPad;
        std::vector<Hyprutils::Memory::CSharedPointer<CLibinputTabletTool>> tabletTools;

        Hyprutils::Memory::CSharedPointer<CLibinputTabletTool>              toolFrom(libinput_tablet_tool* tool);
    };

    class CSession {
      public:
        ~CSession();

        static Hyprutils::Memory::CSharedPointer<CSession>              attempt(Hyprutils::Memory::CSharedPointer<CBackend> backend_);

        bool                                                            active = true; // whether the current vt is ours
        uint32_t                                                        vt     = 0;    // 0 means unsupported
        std::string                                                     seatName;
        Hyprutils::Memory::CWeakPointer<CSession>                       self;

        std::vector<Hyprutils::Memory::CSharedPointer<CSessionDevice>>  sessionDevices;
        std::vector<Hyprutils::Memory::CSharedPointer<CLibinputDevice>> libinputDevices;

        udev*                                                           udevHandle     = nullptr;
        udev_monitor*                                                   udevMonitor    = nullptr;
        libseat*                                                        libseatHandle  = nullptr;
        libinput*                                                       libinputHandle = nullptr;

        std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>>         pollFDs();
        void                                                            dispatchPendingEventsAsync();
        bool                                                            switchVT(uint32_t vt);
        void                                                            onReady();

        struct SAddDrmCardEvent {
            std::string path;
        };

        struct {
            Hyprutils::Signal::CSignal changeActive;
            Hyprutils::Signal::CSignal addDrmCard;
            Hyprutils::Signal::CSignal destroy;
        } events;

      private:
        Hyprutils::Memory::CWeakPointer<CBackend>               backend;
        std::vector<Hyprutils::Memory::CSharedPointer<SPollFD>> polls;

        void                                                    dispatchUdevEvents();
        void                                                    dispatchLibinputEvents();
        void                                                    dispatchLibseatEvents();
        void                                                    handleLibinputEvent(libinput_event* e);

        friend class CSessionDevice;
        friend class CLibinputDevice;
    };
};
