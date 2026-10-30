#pragma once

#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

namespace Aquamarine {
    struct SAtomicRouting {
        uint32_t current = 0;
        uint32_t next    = 0;
    };

    // A CRTC left without a connector must be disabled in the same atomic
    // transaction. Use the complete final routing, not cached reservations:
    // a disabled output may hold a backup CRTC without using it in the kernel.
    inline std::vector<uint32_t> vacatedCrtcs(std::span<const SAtomicRouting> routes, std::span<const uint32_t> protectedCrtcs) {
        std::vector<uint32_t> result;
        for (const auto& route : routes) {
            if (!route.current || std::ranges::find(protectedCrtcs, route.current) != protectedCrtcs.end())
                continue;
            if (std::ranges::any_of(routes, [&](const auto& other) { return other.next == route.current; }))
                continue;
            if (std::ranges::find(result, route.current) == result.end())
                result.emplace_back(route.current);
        }
        return result;
    }
}
