#pragma once

//volk over raw vulkan.h because it defines all extensions
#include <volk.h>
//possible other quality of life like vkb and/or vma?

#include <cstdint>

namespace Aquamarine {
    //possible gbm via VK_EXT_external_memory_fd or use pure drm via VK_EXT_external_memory_dmabuf and the before mentioned extension

    class CVkTex {
      public:
        CVkTex()  = default;
        ~CVkTex() = default; //cleanup device memory?

        //possible samplers?

        bool          importDmabuf(int fd, uint32_t width, uint32_t height, uint64_t modifier, uint32_t format);

        VkImage       image     = VK_NULL_HANDLE;
        VkImageView   imageview = VK_NULL_HANDLE;
        std::uint32_t textureid = 0;
        VkImageType   target;

      private:
        VkDeviceMemory memory;
    };

    class CDRMRenderer {
      public:
        ~CDRMRenderer();

      private:
        VkSwapchainKHR swapchain;
    };
};
