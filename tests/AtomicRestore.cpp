#include "AtomicRestore.hpp"
#include "shared.hpp"

#include <vector>

using namespace Aquamarine;

int main() {
    int ret = 0;

    // Docked startup assigned eDP to 81, but fbcon moved it to 62 on suspend.
    std::vector<SAtomicConnectorBinding> bindings{{139, 62}, {132, 0}};
    std::vector<uint32_t>                reserved{81};
    EXPECT(staleCrtcForConnector(139, 81, bindings, reserved).value_or(0), 62U);

    // A failed/test-only commit must leave the same cleanup available to retry.
    EXPECT(staleCrtcForConnector(139, 81, bindings, reserved).value_or(0), 62U);
    EXPECT(bindings[0].crtc, 62U);
    EXPECT(reserved[0], 81U);

    // A successful commit updates kernel routing; the next request is a no-op.
    bindings[0].crtc = 81;
    EXPECT(staleCrtcForConnector(139, 81, bindings, reserved).has_value(), false);

    // Undocked startup and the kernel console agree on the internal controller.
    bindings[0].crtc = 62;
    reserved         = {62};
    EXPECT(staleCrtcForConnector(139, 62, bindings, reserved).has_value(), false);

    // Do not turn off another output's controller, including leased CRTCs and
    // controllers reserved for outputs whose restore has not run yet.
    reserved = {81, 62};
    EXPECT(staleCrtcForConnector(139, 81, bindings, reserved).has_value(), false);

    // A kernel-side clone still needs this CRTC even if it is not reserved.
    reserved         = {81};
    bindings[1].crtc = 62;
    EXPECT(staleCrtcForConnector(139, 81, bindings, reserved).has_value(), false);

    // An unrelated output on a different controller does not block cleanup.
    bindings[1].crtc = 100;
    reserved         = {81, 100};
    EXPECT(staleCrtcForConnector(139, 81, bindings, reserved).value_or(0), 62U);

    // Disabling a connector also needs to clean up an obsolete kernel binding.
    EXPECT(staleCrtcForConnector(139, 0, bindings, reserved).value_or(0), 62U);

    bindings[0].crtc = 0;
    EXPECT(staleCrtcForConnector(139, 81, bindings, reserved).has_value(), false);
    EXPECT(staleCrtcForConnector(999, 81, bindings, reserved).has_value(), false);

    // IDs come from DRM; the regression test must not encode a special case.
    bindings = {{7, 900}, {8, 0}};
    reserved = {901};
    EXPECT(staleCrtcForConnector(7, 901, bindings, reserved).value_or(0), 900U);

    return ret;
}
