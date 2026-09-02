#pragma once

namespace Aquamarine {
    struct SDRMPageFlipOptions {
        bool event = false;
        bool async = false;
    };

    constexpr SDRMPageFlipOptions drmPageFlipOptions(bool test, bool enabled, bool blocking, bool bufferCommitted, bool immediate) {
        if (test || !enabled || blocking || !bufferCommitted)
            return {};

        return {
            .event = true,
            .async = immediate,
        };
    }
}
