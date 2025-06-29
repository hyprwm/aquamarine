#pragma once

#include <aquamarine/backend/DRM.hpp>
#include "FormatUtils.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>
#define __gl2_h_ // define guard for gl2ext.h
#include <GLES2/gl2ext.h>
#include <gbm.h>
#include <optional>
#include <tuple>
#include <vector>
#include <span>

namespace Aquamarine {

    class CGBMAllocator;

    class CGLTex {
      public:
        CGLTex() = default;
        void     bind();
        void     unbind();
        void     setTexParameter(GLenum pname, GLint param);
        EGLImage image  = nullptr;
        GLuint   texid  = 0;
        GLuint   target = GL_TEXTURE_2D;

      private:
        enum eTextureParam : uint8_t {
            TEXTURE_PAR_WRAP_S = 0,
            TEXTURE_PAR_WRAP_T,
            TEXTURE_PAR_MAG_FILTER,
            TEXTURE_PAR_MIN_FILTER,
            TEXTURE_PAR_LAST,
        };

        inline constexpr std::optional<size_t>             getCacheStateIndex(GLenum pname);
        std::array<std::optional<GLint>, TEXTURE_PAR_LAST> m_cachedStates;
    };

    class CDRMRendererBufferAttachment : public IAttachment {
      public:
        CDRMRendererBufferAttachment(Hyprutils::Memory::CWeakPointer<CDRMRenderer> renderer_, Hyprutils::Memory::CSharedPointer<IBuffer> buffer, EGLImageKHR image, GLuint fbo_,
                                     GLuint rbo_, CGLTex&& tex, std::vector<uint8_t> intermediateBuf_);
        virtual ~CDRMRendererBufferAttachment() {
            ;
        }

        EGLImageKHR                                   eglImage = nullptr;
        GLuint                                        fbo = 0, rbo = 0;
        Hyprutils::Memory::CUniquePointer<CGLTex>     tex;
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

        static Hyprutils::Memory::CSharedPointer<CDRMRenderer> attempt(Hyprutils::Memory::CSharedPointer<CBackend> backend_, int drmFD);
        static Hyprutils::Memory::CSharedPointer<CDRMRenderer> attempt(Hyprutils::Memory::CSharedPointer<CBackend>      backend_,
                                                                       Hyprutils::Memory::CSharedPointer<CGBMAllocator> allocator_);

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

        struct SShader {
            ~SShader();
            void   createVao();

            GLuint program = 0;
            GLint  proj = -1, tex = -1, posAttrib = -1, texAttrib = -1;
            GLuint shaderVao = 0, shaderVboPos = 0, shaderVboUv = 0;
        } shader, shaderExt;

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

        CGLTex                                        glTex(Hyprutils::Memory::CSharedPointer<IBuffer> buf);
        void                                          readBuffer(Hyprutils::Memory::CSharedPointer<IBuffer> buf, std::span<uint8_t> out);

        Hyprutils::Memory::CWeakPointer<CDRMRenderer> self;
        std::vector<SGLFormat>                        formats;

      private:
        CDRMRenderer() = default;

        EGLImageKHR                                           createEGLImage(const SDMABUFAttrs& attrs);
        bool                                                  verifyDestinationDMABUF(const SDMABUFAttrs& attrs);
        void                                                  waitOnSync(int fd);
        int                                                   recreateBlitSync();

        void                                                  loadEGLAPI();
        EGLDeviceEXT                                          eglDeviceFromDRMFD(int drmFD);
        void                                                  initContext();
        void                                                  initResources();
        bool                                                  initDRMFormats();
        std::optional<std::vector<std::pair<uint64_t, bool>>> getModsForFormat(EGLint format);
        bool                                                  hasModifiers = false;
        void                                                  useProgram(GLuint prog);
        GLuint                                                m_currentProgram = 0;

        Hyprutils::Memory::CWeakPointer<CBackend>             backend;

        friend class CEglContextGuard;
    };
};
