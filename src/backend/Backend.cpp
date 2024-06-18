#include <aquamarine/backend/Backend.hpp>

using namespace Hyprutils::Memory;
using namespace Aquamarine;
#define SP CSharedPointer

Aquamarine::CBackend::CBackend() {
    ;
}

Hyprutils::Memory::CSharedPointer<CBackend> Aquamarine::CBackend::create(const std::vector<SBackendImplementationOptions>& backends, const SBackendOptions& options) {
    auto backend = SP<CBackend>(new CBackend());

    backend->options = options;

    backend->log(AQ_LOG_DEBUG, "Hello world!\n");

    return backend;
}

Aquamarine::CBackend::~CBackend() {
    log(AQ_LOG_DEBUG, "Bye world!\n");
}

bool Aquamarine::CBackend::start() {
    log(AQ_LOG_DEBUG, "Starting world!\n");
    return true;
}

void Aquamarine::CBackend::log(eBackendLogLevel level, const std::string& msg) {
    if (!options.logFunction)
        return;

    options.logFunction(level, msg);
}
