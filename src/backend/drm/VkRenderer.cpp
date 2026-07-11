#include "VkRenderer.hpp"

//macros
#define VKCALL(__CALL__)                                                                                                                                                           \
    do {                                                                                                                                                                           \
        auto res = __CALL__;                                                                                                                                                       \
        if (Aquamarine::isTrace()) {                                                                                                                                               \
            if (res != VK_SUCCESS) {                                                                                                                                               \
                backend->log(AQ_LOG_ERROR,                                                                                                                                         \
                             std::format("[Vulkan] Error in call at {}@{}: 0x{:x}", __LINE__,                                                                                      \
                                         ([]() constexpr -> std::string { return std::string(__FILE__).substr(std::string(__FILE__).find_last_of('/') + 1); })(), res));           \
            }                                                                                                                                                                      \
        }                                                                                                                                                                          \
    } while (0)

namespace Aquamarine {
    bool CVkTex::importDmabuf(int fd, uint32_t width, uint32_t height, uint64_t modifier, uint32_t format) {
        //TODO: import the file descriptor via VkExternalMemoryImageCreateInfoKHR.

        return true;
    }
};
