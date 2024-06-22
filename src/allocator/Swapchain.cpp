#include <aquamarine/allocator/Swapchain.hpp>
#include <aquamarine/backend/Backend.hpp>
#include "FormatUtils.hpp"

using namespace Aquamarine;
using namespace Hyprutils::Memory;
#define SP CSharedPointer

Aquamarine::CSwapchain::CSwapchain(SP<IAllocator> allocator_) : allocator(allocator_) {
    if (!allocator)
        return;
}

bool Aquamarine::CSwapchain::reconfigure(const SSwapchainOptions& options_) {
    if (!allocator)
        return false;

    if (options_.format == options.format && options_.size == options.size && options_.length == options.length)
        return true; // no need to reconfigure

    if (options_.format == options.format && options_.size == options.size) {
        bool ok = resize(options_.length);
        if (!ok)
            return false;

        options = options_;

        allocator->getBackend()->log(AQ_LOG_DEBUG, std::format("Swapchain: Resized a {} {} swapchain to length {}", options.size, fourccToName(options.format), options.length));
        return true;
    }

    bool ok = fullReconfigure(options_);
    if (!ok)
        return false;

    options = options_;

    allocator->getBackend()->log(AQ_LOG_DEBUG,
                                 std::format("Swapchain: Reconfigured a swapchain to {} {} of length {}", options.size, fourccToName(options.format), options.length));
    return true;
}

SP<IBuffer> Aquamarine::CSwapchain::next(int* age) {
    if (!allocator || options.length <= 0)
        return nullptr;

    lastAcquired = (lastAcquired + 1) % options.length;

    if (age)
        *age = 1;

    return buffers.at(lastAcquired);
}

bool Aquamarine::CSwapchain::fullReconfigure(const SSwapchainOptions& options_) {
    buffers.clear();
    for (size_t i = 0; i < options_.length; ++i) {
        auto buf = allocator->acquire(SAllocatorBufferParams{.size = options_.size, .format = options_.format});
        if (!buf) {
            allocator->getBackend()->log(AQ_LOG_ERROR, "Swapchain: Failed acquiring a buffer");
            return false;
        }
        buffers.emplace_back(buf);
    }

    return true;
}

bool Aquamarine::CSwapchain::resize(size_t newSize) {
    if (newSize == buffers.size())
        return true;

    if (newSize < buffers.size()) {
        while (buffers.size() > newSize) {
            buffers.pop_back();
        }
    } else {
        while (buffers.size() < newSize) {
            auto buf = allocator->acquire(SAllocatorBufferParams{.size = options.size, .format = options.format});
            if (!buf) {
                allocator->getBackend()->log(AQ_LOG_ERROR, "Swapchain: Failed acquiring a buffer");
                return false;
            }
            buffers.emplace_back(buf);
        }
    }

    return true;
}

bool Aquamarine::CSwapchain::contains(Hyprutils::Memory::CSharedPointer<IBuffer> buffer) {
    return std::find(buffers.begin(), buffers.end(), buffer) != buffers.end();
}

const SSwapchainOptions& Aquamarine::CSwapchain::currentOptions() {
    return options;
}
