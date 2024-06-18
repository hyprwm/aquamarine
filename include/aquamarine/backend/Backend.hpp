#pragma once

#include <hyprutils/memory/SharedPtr.hpp>
#include <vector>
#include <functional>

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
        virtual ~IBackendImplementation();
        virtual eBackendType type() = 0;
    };

    class CBackend {
      public:
        /* Create a backend, with the provided options. May return a single or a multi-backend. */
        static Hyprutils::Memory::CSharedPointer<CBackend> create(const std::vector<SBackendImplementationOptions>& backends, const SBackendOptions& options);

        ~CBackend();

        /* start the backend. Initializes all the stuff, and will return true on success, false on fail. */
        bool start();

        void log(eBackendLogLevel level, const std::string& msg);

      private:
        CBackend();

        std::vector<IBackendImplementation> implementations;
        SBackendOptions                     options;
    };
};