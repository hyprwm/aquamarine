#include "AtomicRouting.hpp"
#include "shared.hpp"

using namespace Aquamarine;

int main() {
    int                         ret = 0;
    std::vector<SAtomicRouting> routes;
    std::vector<uint32_t>       protectedCrtcs;

    // Input switch: the external output moves from D to A; the closed laptop
    // has a userspace backup reservation for D but no kernel connector on D.
    routes = {{505, 151}, {0, 0}, {269, 269}, {387, 387}};
    EXPECT(vacatedCrtcs(routes, protectedCrtcs).size(), 1U);
    EXPECT(vacatedCrtcs(routes, protectedCrtcs).at(0), 505U);

    // TEST_ONLY and failed requests do not consume the cleanup; retry it.
    EXPECT(vacatedCrtcs(routes, protectedCrtcs).at(0), 505U);
    EXPECT(routes.at(0).current, 505U);
    routes.at(0).current = 151;
    EXPECT(vacatedCrtcs(routes, protectedCrtcs).empty(), true);

    // An MST connector disappears, but the replacement uses the same CRTC.
    // Detach the old connector, retaining the CRTC for its new owner.
    routes = {{269, 0}, {0, 269}};
    EXPECT(vacatedCrtcs(routes, protectedCrtcs).empty(), true);

    // Two live outputs swap controllers in one transaction. Neither is idle.
    routes = {{151, 269}, {269, 151}};
    EXPECT(vacatedCrtcs(routes, protectedCrtcs).empty(), true);

    // A kernel clone or an unknown connector still uses the old controller.
    routes = {{62, 81}, {62, 62}};
    EXPECT(vacatedCrtcs(routes, protectedCrtcs).empty(), true);

    // fbcon moved the internal panel during suspend. Disable the vacated
    // controller even when it is not reserved by any userspace output.
    routes = {{62, 81}, {0, 0}};
    EXPECT(vacatedCrtcs(routes, protectedCrtcs).at(0), 62U);

    // Leased controllers may not be visible in the connector snapshot.
    protectedCrtcs = {62};
    EXPECT(vacatedCrtcs(routes, protectedCrtcs).empty(), true);
    protectedCrtcs.clear();

    // Multiple connectors can vacate one controller; clear it only once.
    routes = {{62, 0}, {62, 0}, {81, 0}, {0, 0}};
    EXPECT(vacatedCrtcs(routes, protectedCrtcs).size(), 2U);
    EXPECT(vacatedCrtcs(routes, protectedCrtcs).at(0), 62U);
    EXPECT(vacatedCrtcs(routes, protectedCrtcs).at(1), 81U);

    routes = {{62, 62}, {81, 81}};
    EXPECT(vacatedCrtcs(routes, protectedCrtcs).empty(), true);
    EXPECT(vacatedCrtcs({}, {}).empty(), true);

    return ret;
}
