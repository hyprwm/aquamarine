#include "Logger.hpp"
#include "../include/Shared.hpp"

using namespace Aquamarine;

static Hyprutils::CLI::eLogLevel levelToHU(eBackendLogLevel l) {
    switch (l) {
        case Aquamarine::AQ_LOG_DEBUG: return Hyprutils::CLI::LOG_DEBUG;
        case Aquamarine::AQ_LOG_ERROR: return Hyprutils::CLI::LOG_ERR;
        case Aquamarine::AQ_LOG_WARNING: return Hyprutils::CLI::LOG_WARN;
        case Aquamarine::AQ_LOG_CRITICAL: return Hyprutils::CLI::LOG_CRIT;
        case Aquamarine::AQ_LOG_TRACE: return Hyprutils::CLI::LOG_TRACE;
    }
    return Hyprutils::CLI::LOG_DEBUG;
}

CLogger::CLogger() = default;

void CLogger::updateLevels() {
    const auto IS_TRACE = Aquamarine::isTrace();
    if (m_loggerConnection && IS_TRACE)
        m_loggerConnection->setLogLevel(Hyprutils::CLI::LOG_TRACE);
}

void CLogger::log(eBackendLogLevel level, const std::string& str) {
    if (m_logFn) {
        m_logFn(level, str);
        return;
    }

    if (m_loggerConnection) {
        m_loggerConnection->log(levelToHU(level), str);
        return;
    }
}
