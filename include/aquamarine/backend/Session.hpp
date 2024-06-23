#pragma once

#include <sys/types.h>
#include <hyprutils/signal/Signal.hpp>
#include <hyprutils/memory/SharedPtr.hpp>
#include <vector>

struct udev;
struct udev_monitor;
struct udev_device;
struct libseat;

namespace Aquamarine {
    class CBackend;
    class CSession;

    class CSessionDevice {
      public:
        CSessionDevice(Hyprutils::Memory::CSharedPointer<CSession> session_, const std::string& path_);
        ~CSessionDevice();

        static Hyprutils::Memory::CSharedPointer<CSessionDevice> openIfKMS(Hyprutils::Memory::CSharedPointer<CSession> session_, const std::string& path_);

        bool supportsKMS();

        int                                                      fd       = -1;
        int                                                      deviceID = -1;
        dev_t                                                    dev;
        std::string                                              path;

        enum eChangeEventType {
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

    class CSession {
      public:
        ~CSession();

        static Hyprutils::Memory::CSharedPointer<CSession>             attempt(Hyprutils::Memory::CSharedPointer<CBackend> backend_);

        bool                                                           active = true; // whether the current vt is ours
        uint32_t                                                       vt     = 0;    // 0 means unsupported
        std::string                                                    seatName;

        std::vector<Hyprutils::Memory::CSharedPointer<CSessionDevice>> sessionDevices;

        udev*                                                          udevHandle    = nullptr;
        udev_monitor*                                                  udevMonitor   = nullptr;
        libseat*                                                       libseatHandle = nullptr;

        std::vector<int>                                               pollFDs();

        void                                                           dispatchPendingEventsAsync();

        struct SAddDrmCardEvent {
            std::string path;
        };

        struct {
            Hyprutils::Signal::CSignal changeActive;
            Hyprutils::Signal::CSignal addDrmCard;
            Hyprutils::Signal::CSignal destroy;
        } events;

      private:
        Hyprutils::Memory::CWeakPointer<CBackend> backend;

        void                                      dispatchUdevEvents();

        friend class CSessionDevice;
    };
};
