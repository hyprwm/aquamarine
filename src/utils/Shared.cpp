#include "Shared.hpp"
#include <cstdlib>

bool Aquamarine::envEnabled(const std::string& env) {
    auto e = getenv(env.c_str());
    return e && e == std::string{"1"};
}