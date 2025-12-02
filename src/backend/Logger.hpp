#pragma once

#include <aquamarine/backend/Backend.hpp>

#include <hyprutils/cli/Logger.hpp>

namespace Aquamarine {
    class CLogger {
      public:
        CLogger();
        ~CLogger() = default;

        void log(eBackendLogLevel level, const std::string& str);
        void updateLevels();

        template <typename... Args>
        //NOLINTNEXTLINE
        void log(eBackendLogLevel level, std::format_string<Args...> fmt, Args&&... args) {
            if (!m_loggerConnection && !m_logFn)
                return;

            std::string logMsg = "";

            // no need for try {} catch {} because std::format_string<Args...> ensures that vformat never throw std::format_error
            // because
            // 1. any faulty format specifier that sucks will cause a compilation error.
            // 2. and `std::bad_alloc` is catastrophic, (Almost any operation in stdlib could throw this.)
            // 3. this is actually what std::format in stdlib does
            logMsg += std::vformat(fmt.get(), std::make_format_args(args...));

            log(level, logMsg);
        }

        std::function<void(eBackendLogLevel, std::string)>                   m_logFn;
        Hyprutils::Memory::CSharedPointer<Hyprutils::CLI::CLoggerConnection> m_loggerConnection;
    };
};