#include <aquamarine/backend/Backend.hpp>
#include <iostream>

static const char* aqLevelToString(Aquamarine::eBackendLogLevel level) {
    switch (level) {
        case Aquamarine::eBackendLogLevel::AQ_LOG_TRACE: return "TRACE";
        case Aquamarine::eBackendLogLevel::AQ_LOG_DEBUG: return "DEBUG";
        case Aquamarine::eBackendLogLevel::AQ_LOG_ERROR: return "ERROR";
        case Aquamarine::eBackendLogLevel::AQ_LOG_WARNING: return "WARNING";
        case Aquamarine::eBackendLogLevel::AQ_LOG_CRITICAL: return "CRITICAL";
        default: break;
    }

    return "UNKNOWN";
}

void aqLog(Aquamarine::eBackendLogLevel level, std::string msg) {
    std::cout << "[AQ] [" << aqLevelToString(level) << "] " << msg << "\n";
}

int main(int argc, char** argv, char** envp) {
    Aquamarine::SBackendOptions options;
    options.logFunction = aqLog;

    std::vector<Aquamarine::SBackendImplementationOptions> implementations;
    Aquamarine::SBackendImplementationOptions waylandOptions;
    waylandOptions.backendType = Aquamarine::eBackendType::AQ_BACKEND_WAYLAND;
    waylandOptions.backendRequestMode = Aquamarine::eBackendRequestMode::AQ_BACKEND_REQUEST_IF_AVAILABLE;
    implementations.emplace_back(waylandOptions);

    auto aqBackend = Aquamarine::CBackend::create(implementations, options);

    if (!aqBackend->start()) {
        std::cout << "Failed to start the aq backend\n";
        return 1;
    }

    aqBackend->enterLoop();

    return 0;
}