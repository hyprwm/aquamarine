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

    struct SGLTex {
        EGLImage image  = nullptr;
        GLuint   texid  = 0;
        GLuint   target = GL_TEXTURE_2D;
    };

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
                ~SShader() {
                    if (program == 0)
                        return;

                    if (shaderVao)
                        glDeleteVertexArrays(1, &shaderVao);

                    if (shaderVboPos)
                        glDeleteBuffers(1, &shaderVboPos);

                    if (shaderVboUv)
                        glDeleteBuffers(1, &shaderVboUv);

                    glDeleteProgram(program);
                    program = 0;
                }

                GLuint program = 0;
                GLint  proj = -1, tex = -1, posAttrib = -1, texAttrib = -1;
                GLuint shaderVao = 0, shaderVboPos = 0, shaderVboUv = 0;

                void   createVao() {
                    const float fullVerts[] = {
                        1, 0, // top right
                        0, 0, // top left
                        1, 1, // bottom right
                        0, 1, // bottom left
                    };

                    glGenVertexArrays(1, &shaderVao);
                    glBindVertexArray(shaderVao);

                    if (posAttrib != -1) {
                        glGenBuffers(1, &shaderVboPos);
                        glBindBuffer(GL_ARRAY_BUFFER, shaderVboPos);
                        glBufferData(GL_ARRAY_BUFFER, sizeof(fullVerts), fullVerts, GL_STATIC_DRAW);
                        glEnableVertexAttribArray(posAttrib);
                        glVertexAttribPointer(posAttrib, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
                    }

                    if (texAttrib != -1) {
                        glGenBuffers(1, &shaderVboUv);
                        glBindBuffer(GL_ARRAY_BUFFER, shaderVboUv);
                        glBufferData(GL_ARRAY_BUFFER, sizeof(fullVerts), fullVerts, GL_STATIC_DRAW);
                        glEnableVertexAttribArray(texAttrib);
                        glVertexAttribPointer(texAttrib, 2, GL_FLOAT, GL_FALSE, 0, (void*)0);
                    }

                    glBindVertexArray(0);
                    glBindBuffer(GL_ARRAY_BUFFER, 0);
                }
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
        CDRMRenderer() = default;

        EGLImageKHR                                           createEGLImage(const SDMABUFAttrs& attrs);
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
        void                                                  useProgram(GLuint prog);
        GLuint                                                m_currentProgram = 0;

        Hyprutils::Memory::CWeakPointer<CBackend>             backend;

        friend class CEglContextGuard;
    };
};
