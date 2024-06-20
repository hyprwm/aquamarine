#pragma once

#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/signal/Signal.hpp>
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include "../allocator/Allocator.hpp"

namespace Aquamarine {
    enum eBackendType {
        AQ_BACKEND_WAYLAND = 0,
        AQ_BACKEND_DRM,
        AQ_BACKEND_HEADLESS,
    };

    enum eBackendRequestMode {
        /*
            Require the provided backend, will error out if it's not available.
        */
        AQ_BACKEND_REQUEST_MANDATORY = 0,
        /*
            Start the backend if it's available
        */
        AQ_BACKEND_REQUEST_IF_AVAILABLE,
        /*
            If any IF_AVAILABLE backend fails, use this one
        */
        AQ_BACKEND_REQUEST_FALLBACK,
    };

    enum eBackendLogLevel {
        AQ_LOG_TRACE = 0,
        AQ_LOG_DEBUG,
        AQ_LOG_WARNING,
        AQ_LOG_ERROR,
        AQ_LOG_CRITICAL,
    };

    struct SBackendImplementationOptions {
        explicit SBackendImplementationOptions();
        eBackendType        backendType;
        eBackendRequestMode backendRequestMode;
    };

    struct SBackendOptions {
        explicit SBackendOptions();
        std::function<void(eBackendLogLevel, std::string)> logFunction;
    };

    class IBackendImplementation {
      public:
        virtual ~IBackendImplementation() {
            ;
        }

        enum eBackendCapabilities : uint32_t {
            AQ_BACKEND_CAPABILITY_POINTER = (1 << 0),
        };

        virtual eBackendType type()           = 0;
        virtual bool         start()          = 0;
        virtual int          pollFD()         = 0;
        virtual int          drmFD()          = 0;
        virtual bool         dispatchEvents() = 0;
        virtual uint32_t     capabilities()   = 0;
        virtual void         onReady()        = 0;
    };

    class CBackend {
      public:
        /* Create a backend, with the provided options. May return a single or a multi-backend. */
        static Hyprutils::Memory::CSharedPointer<CBackend> create(const std::vector<SBackendImplementationOptions>& backends, const SBackendOptions& options);

        ~CBackend();

        /* start the backend. Initializes all the stuff, and will return true on success, false on fail. */
        bool start();

        void log(eBackendLogLevel level, const std::string& msg);

        /* Enters the event loop synchronously. For simple clients, this is probably what you want. For more complex ones,
           see the async methods further below */
        void enterLoop();

        /* Gets all the FDs you have to poll. When any single one fires, call dispatchEventsAsync */
        std::vector<int> getPollFDs();

        /* Dispatches all pending events on all queues then returns */
        void dispatchEventsAsync();

        /* Checks if the backend has a session - iow if it's a DRM backend */
        bool hasSession();

        /* Get the primary DRM FD */
        int drmFD();

        struct {
            Hyprutils::Signal::CSignal newOutput;
            Hyprutils::Signal::CSignal newPointer;
            Hyprutils::Signal::CSignal newKeyboard;
            Hyprutils::Signal::CSignal newTouch;
        } events;

        Hyprutils::Memory::CSharedPointer<IAllocator> allocator;

      private:
        CBackend();

        bool                                                                   terminate = false;

        std::vector<SBackendImplementationOptions>                             implementationOptions;
        std::vector<Hyprutils::Memory::CSharedPointer<IBackendImplementation>> implementations;
        SBackendOptions                                                        options;
        Hyprutils::Memory::CWeakPointer<CBackend>                              self;

        //
        struct {
            std::condition_variable loopSignal;
            std::mutex              loopMutex;
            std::atomic<bool>       shouldProcess = false;
            std::mutex              loopRequestMutex;
            std::mutex              eventLock;
        } m_sEventLoopInternals;
    };
};