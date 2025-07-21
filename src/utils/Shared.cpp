#include "Shared.hpp"
#include <cstdlib>

bool Aquamarine::envEnabled(const std::string& env) {
    auto e = getenv(env.c_str());
    return e && e == std::string{"1"};
}

bool Aquamarine::envExplicitlyDisabled(const std::string& env) {
    auto e = getenv(env.c_str());
    return e && e == std::string{"0"};
}

static bool trace = []() -> bool { return Aquamarine::envEnabled("AQ_TRACE"); }();

bool        Aquamarine::isTrace() {
    return trace;
}
