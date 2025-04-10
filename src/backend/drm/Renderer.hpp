#pragma once

#include <hyprutils/memory/SharedPtr.hpp>
#include <hyprutils/memory/UniquePtr.hpp>
#include <aquamarine/backend/DRM.hpp>
#include <aquamarine/misc/Attachment.hpp>
#include "FormatUtils.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <optional>
#include <vector>
#include <span>
#include <condition_variable>
#include <hyprutils/os/FileDescriptor.hpp>

#define VULKAN_HPP_NO_EXCEPTIONS
#define VULKAN_HPP_ASSERT_ON_RESULT(...)                                                                                                                                           \
    do {                                                                                                                                                                           \
    } while (0)
#include <vulkan/vulkan.hpp>

namespace Aquamarine {

    class CGBMAllocator;

    struct SGLTex {
        EGLImage image  = nullptr;
        GLuint   texid  = 0;
        GLuint   target = GL_TEXTURE_2D;
    };

    inline vk::UniqueInstance g_pVulkanInstance;

    class CDRMRendererBufferAttachment : public IAttachment {
      public:
        CDRMRendererBufferAttachment(Hyprutils::Memory::CWeakPointer<CDRMRenderer> renderer_, Hyprutils::Memory::CSharedPointer<IBuffer> buffer, EGLImageKHR image, GLuint fbo_,
                                     GLuint rbo_, SGLTex tex, std::vector<uint8_t> intermediateBuf_);
        virtual ~CDRMRendererBufferAttachment() {
            ;
        }

        EGLImageKHR                                   eglImage = nullptr;
        GLuint                                        fbo = 0, rbo = 0;
        SGLTex                                        tex;
        Hyprutils::Signal::CHyprSignalListener        bufferDestroy;
        std::vector<uint8_t>                          intermediateBuf;

        Hyprutils::Memory::CWeakPointer<CDRMRenderer> renderer;
    };

    // CEglContextGuard is a RAII abstraction for the EGL context.
    // On initialization, it sets the EGL context to the renderer's display,
    // and on destruction, it restores the previous EGL context.
    class CEglContextGuard {
      public:
        CEglContextGuard(const CDRMRenderer& renderer_);
        ~CEglContextGuard();

        // No copy or move constructors
        CEglContextGuard(const CEglContextGuard&)            = delete;
        CEglContextGuard& operator=(const CEglContextGuard&) = delete;
        CEglContextGuard(CEglContextGuard&&)                 = delete;
        CEglContextGuard& operator=(CEglContextGuard&&)      = delete;

      private:
        const CDRMRenderer& renderer;
        struct {
            EGLDisplay display = nullptr;
            EGLContext context = nullptr;
            EGLSurface draw = nullptr, read = nullptr;
        } savedEGLState;
    };

    class CDRMRenderer {
      public:
        ~CDRMRenderer();

        static Hyprutils::Memory::CSharedPointer<CDRMRenderer> attempt(Hyprutils::Memory::CSharedPointer<CBackend> backend_, int drmFD, bool GLES2 = true);
        static Hyprutils::Memory::CSharedPointer<CDRMRenderer> attempt(Hyprutils::Memory::CSharedPointer<CBackend>      backend_,
                                                                       Hyprutils::Memory::CSharedPointer<CGBMAllocator> allocator_, bool GLES2 = true);

        int                                                    drmFD = -1;

        struct SBlitResult {
            bool               success = false;
            std::optional<int> syncFD;
        };

        SBlitResult blit(Hyprutils::Memory::CSharedPointer<IBuffer> from, Hyprutils::Memory::CSharedPointer<IBuffer> to,
                         Hyprutils::Memory::CSharedPointer<CDRMRenderer> primaryRenderer, int waitFD = -1);
        // can't be a SP<> because we call it from buf's ctor...
        void clearBuffer(IBuffer* buf);

        void onBufferAttachmentDrop(CDRMRendererBufferAttachment* attachment);

        struct {
            struct SShader {
                GLuint program = 0;
                GLint  proj = -1, tex = -1, posAttrib = -1, texAttrib = -1;
            } shader, shaderExt;
        } gl;

        struct {
            PFNEGLGETPLATFORMDISPLAYEXTPROC               eglGetPlatformDisplayEXT               = nullptr;
            PFNEGLCREATEIMAGEKHRPROC                      eglCreateImageKHR                      = nullptr;
            PFNEGLDESTROYIMAGEKHRPROC                     eglDestroyImageKHR                     = nullptr;
            PFNGLEGLIMAGETARGETTEXTURE2DOESPROC           glEGLImageTargetTexture2DOES           = nullptr;
            PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC glEGLImageTargetRenderbufferStorageOES = nullptr;
            PFNEGLQUERYDMABUFFORMATSEXTPROC               eglQueryDmaBufFormatsEXT               = nullptr;
            PFNEGLQUERYDMABUFMODIFIERSEXTPROC             eglQueryDmaBufModifiersEXT             = nullptr;
            PFNEGLDESTROYSYNCKHRPROC                      eglDestroySyncKHR                      = nullptr;
            PFNEGLWAITSYNCKHRPROC                         eglWaitSyncKHR                         = nullptr;
            PFNEGLCREATESYNCKHRPROC                       eglCreateSyncKHR                       = nullptr;
            PFNEGLDUPNATIVEFENCEFDANDROIDPROC             eglDupNativeFenceFDANDROID             = nullptr;
            PFNEGLDEBUGMESSAGECONTROLKHRPROC              eglDebugMessageControlKHR              = nullptr;
            PFNEGLQUERYDEVICESEXTPROC                     eglQueryDevicesEXT                     = nullptr;
            PFNEGLQUERYDEVICESTRINGEXTPROC                eglQueryDeviceStringEXT                = nullptr;
            PFNGLREADNPIXELSEXTPROC                       glReadnPixelsEXT                       = nullptr;
        } proc;

        struct {
            bool EXT_read_format_bgra               = false;
            bool EXT_texture_format_BGRA8888        = false;
            bool EXT_platform_device                = false;
            bool KHR_platform_gbm                   = false;
            bool EXT_image_dma_buf_import           = false;
            bool EXT_image_dma_buf_import_modifiers = false;
            bool KHR_display_reference              = false;
            bool IMG_context_priority               = false;
            bool EXT_create_context_robustness      = false;
        } exts;

        struct {
            EGLDisplay display        = nullptr;
            EGLContext context        = nullptr;
            EGLSync    lastBlitSync   = nullptr;
            int        lastBlitSyncFD = -1;
        } egl;

        SGLTex                                        glTex(Hyprutils::Memory::CSharedPointer<IBuffer> buf);
        void                                          readBuffer(Hyprutils::Memory::CSharedPointer<IBuffer> buf, std::span<uint8_t> out);

        Hyprutils::Memory::CWeakPointer<CDRMRenderer> self;
        std::vector<SGLFormat>                        formats;

      private:
        CDRMRenderer(Hyprutils::Memory::CWeakPointer<CBackend> backend_, int drmFD_) : drmFD(drmFD_), backend(backend_) {}

        EGLImageKHR                                           createEGLImage(const SDMABUFAttrs& attrs);
        bool                                                  verifyDestinationDMABUF(const SDMABUFAttrs& attrs);
        void                                                  waitOnSync(int fd);
        void                                                  cpuWaitOnSync(int fd);
        int                                                   recreateBlitSync();

        void                                                  loadEGLAPI();
        void                                                  loadVulkanAPI();
        void                                                  loadVulkanDevice();
        EGLDeviceEXT                                          eglDeviceFromDRMFD(int drmFD);
        void                                                  initContext(bool GLES2);
        void                                                  initResources();
        bool                                                  initDRMFormats();
        std::optional<std::vector<std::pair<uint64_t, bool>>> getModsForFormat(EGLint format);
        bool                                                  hasModifiers = false;

        Hyprutils::Memory::CWeakPointer<CBackend>             backend;

        class CVulkanBufferAttachment : public IAttachment {
          public:
            CVulkanBufferAttachment();
            virtual ~CVulkanBufferAttachment();

            void                           submitCopyThreadTask(std::function<void()> task);
            void                           waitForCopyTaskCompletion();

            vk::UniqueSemaphore            syncFdSemaphore;
            vk::UniqueSemaphore            timelineSemaphore;
            uint64_t                       timelineSemaphorePoint = 0;
            vk::UniqueDeviceMemory         gpuMem;
            vk::UniqueBuffer               gpuBuf;
            vk::UniqueDeviceMemory         hostVisibleMem;
            vk::UniqueBuffer               hostVisibleBuf;
            vk::UniqueFence                fence;
            Hyprutils::OS::CFileDescriptor fenceFD;
            vk::UniqueCommandBuffer        commandBuffer;
            std::span<uint8_t>             hostMapping;

            std::thread                    copyThread;
            std::function<void()>          copyThreadTask;
            std::mutex                     copyThreadMutex;
            std::condition_variable        copyThreadCondVar;
            bool                           copyThreadShuttingDown = false;
        };

        SBlitResult                       vkBlit(Hyprutils::Memory::CSharedPointer<IBuffer> from, Hyprutils::Memory::CSharedPointer<IBuffer> to,
                                                 Hyprutils::Memory::CSharedPointer<CDRMRenderer> primaryRenderer, int waitFD = -1);
        void                              finishAndCleanupVkBlit(Hyprutils::Memory::CSharedPointer<CVulkanBufferAttachment> att);
        std::span<uint8_t>                vkMapBufferToHost(Hyprutils::Memory::CSharedPointer<IBuffer> from, bool writing);
        SBlitResult                       copyVkStagingBuffer(Hyprutils::Memory::CSharedPointer<IBuffer> buf, bool writing, int waitFD, bool waitForTimelineSemaphore);
        vk::UniqueDevice                  vkDevice;
        vk::UniqueCommandPool             vkCmdPool;
        vk::Queue                         vkQueue;
        vk::detail::DispatchLoaderDynamic vkDynamicDispatcher;
        uint32_t                          vkGpuMemTypeIdx  = UINT32_MAX;
        uint32_t                          vkHostMemTypeIdx = UINT32_MAX;

        friend class CEglContextGuard;
    };
};
