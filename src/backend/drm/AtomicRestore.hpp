#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>

namespace Aquamarine {
    struct SAtomicConnectorBinding {
        uint32_t connector = 0;
        uint32_t crtc      = 0;
    };

    // Moving the last connector off a CRTC also requires disabling that CRTC.
    // Leave controllers reserved by other outputs (including leases) alone.
    inline std::optional<uint32_t> staleCrtcForConnector(uint32_t connector, uint32_t targetCrtc, std::span<const SAtomicConnectorBinding> bindings,
                                                         std::span<const uint32_t> reservedCrtcs) {
        const auto CURRENT = std::ranges::find_if(bindings, [connector](const auto& binding) { return binding.connector == connector; });
        if (CURRENT == bindings.end() || !CURRENT->crtc || CURRENT->crtc == targetCrtc)
            return std::nullopt;

        const auto CRTC = CURRENT->crtc;
        if (std::ranges::find(reservedCrtcs, CRTC) != reservedCrtcs.end())
            return std::nullopt;

        if (std::ranges::any_of(bindings, [connector, CRTC](const auto& binding) { return binding.connector != connector && binding.crtc == CRTC; }))
            return std::nullopt;

        return CRTC;
    }
}
