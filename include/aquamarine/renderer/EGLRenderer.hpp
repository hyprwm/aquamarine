#pragma once

#include <aquamarine/backend/DRM.hpp>
#include <aquamarine/buffer/Buffer.hpp>
#include "FormatUtils.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <optional>
#include <tuple>
#include <vector>

namespace Aquamarine {

    class CGBMAllocator;

    struct SGLTex {
        EGLImage image  = nullptr;
        GLuint   texid  = 0;
        GLuint   target = GL_TEXTURE_2D;
    };

    class CEGLRendererBufferAttachment : public IAttachment {
      public:
        CEGLRendererBufferAttachment(Hyprutils::Memory::CWeakPointer<CEGLRenderer> renderer_, Hyprutils::Memory::CSharedPointer<IBuffer> buffer, EGLImageKHR image, GLuint fbo_,
                                     GLuint rbo_, SGLTex tex);
        virtual ~CEGLRendererBufferAttachment() {
            ;
        }
        virtual eAttachmentType type() {
            return AQ_ATTACHMENT_DRM_RENDERER_DATA;
        }

        EGLImageKHR                                   eglImage = nullptr;
        GLuint                                        fbo = 0, rbo = 0;
        SGLTex                                        tex;
        Hyprutils::Signal::CHyprSignalListener        bufferDestroy;

        Hyprutils::Memory::CWeakPointer<CEGLRenderer> renderer;
    };

    class CEGLSync {
      public:
        ~CEGLSync();

        EGLSyncKHR sync = nullptr;

        int        fd();
        bool       wait();

      private:
        CEGLSync() = default;

        int m_iFd = -1;

        friend class CEGLRenderer;
    };

    class CEGLRenderer {
      public:
        ~CEGLRenderer();

        static Hyprutils::Memory::CSharedPointer<CEGLRenderer> attempt(Hyprutils::Memory::CSharedPointer<CBackend> backend_, int drmFD, bool GLES2 = true);
        static Hyprutils::Memory::CSharedPointer<CEGLRenderer> attempt(Hyprutils::Memory::CSharedPointer<CBackend>      backend_,
                                                                       Hyprutils::Memory::CSharedPointer<CGBMAllocator> allocator_, bool GLES2 = true);

        int                                                    drmFD = -1;

        struct SBlitResult {
            bool               success = false;
            std::optional<int> syncFD;
        };

        SBlitResult blit(Hyprutils::Memory::CSharedPointer<IBuffer> from, Hyprutils::Memory::CSharedPointer<IBuffer> to, int waitFD = -1);
        // can't be a SP<> because we call it from buf's ctor...
        void clearBuffer(IBuffer* buf);

        void setEGL();
        void restoreEGL();

        void onBufferAttachmentDrop(CEGLRendererBufferAttachment* attachment);

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

        struct {
            EGLDisplay display = nullptr;
            EGLContext context = nullptr;
            EGLSurface draw = nullptr, read = nullptr;
        } savedEGLState;

        SGLTex                                        glTex(Hyprutils::Memory::CSharedPointer<IBuffer> buf);

        Hyprutils::Memory::CWeakPointer<CEGLRenderer> self;
        std::vector<SGLFormat>                        formats;

      private:
        CEGLRenderer() = default;

        EGLImageKHR                                           createEGLImage(const SDMABUFAttrs& attrs);
        Hyprutils::Memory::CSharedPointer<CEGLSync>           createEGLSync(int fenceFD);
        bool                                                  verifyDestinationDMABUF(const SDMABUFAttrs& attrs);
        void                                                  waitOnSync(int fd);
        int                                                   recreateBlitSync();

        void                                                  loadEGLAPI();
        EGLDeviceEXT                                          eglDeviceFromDRMFD(int drmFD);
        void                                                  initContext(bool GLES2);
        void                                                  initResources();
        bool                                                  initDRMFormats();
        std::optional<std::vector<std::pair<uint64_t, bool>>> getModsForFormat(EGLint format);
        bool                                                  hasModifiers = false;

        Hyprutils::Memory::CWeakPointer<CBackend>             backend;
    };
};
