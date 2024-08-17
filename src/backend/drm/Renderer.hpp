#pragma once

#include <aquamarine/backend/DRM.hpp>
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

    class CDRMRendererBufferAttachment : public IAttachment {
      public:
        CDRMRendererBufferAttachment(Hyprutils::Memory::CWeakPointer<CDRMRenderer> renderer_, Hyprutils::Memory::CSharedPointer<IBuffer> buffer, EGLImageKHR image, GLuint fbo_,
                                     GLuint rbo_, SGLTex tex);
        virtual ~CDRMRendererBufferAttachment() {
            ;
        }
        virtual eAttachmentType type() {
            return AQ_ATTACHMENT_DRM_RENDERER_DATA;
        }

        EGLImageKHR                                   eglImage = nullptr;
        GLuint                                        fbo = 0, rbo = 0;
        SGLTex                                        tex;
        Hyprutils::Signal::CHyprSignalListener        bufferDestroy;

        Hyprutils::Memory::CWeakPointer<CDRMRenderer> renderer;
    };

    class CDRMRenderer {
      public:
        static Hyprutils::Memory::CSharedPointer<CDRMRenderer> attempt(Hyprutils::Memory::CSharedPointer<CGBMAllocator> allocator_,
                                                                       Hyprutils::Memory::CSharedPointer<CBackend>      backend_);

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

        void onBufferAttachmentDrop(CDRMRendererBufferAttachment* attachment);

        struct {
            struct SShader {
                GLuint program = 0;
                GLint  proj = -1, tex = -1, posAttrib = -1, texAttrib = -1;
            } shader, shaderExt;
        } gl;

        struct {
            EGLDisplay                                    display        = nullptr;
            EGLContext                                    context        = nullptr;
            EGLSync                                       lastBlitSync   = nullptr;
            int                                           lastBlitSyncFD = -1;

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
        } egl;

        struct {
            EGLDisplay display = nullptr;
            EGLContext context = nullptr;
            EGLSurface draw = nullptr, read = nullptr;
        } savedEGLState;

        SGLTex                                        glTex(Hyprutils::Memory::CSharedPointer<IBuffer> buf);

        Hyprutils::Memory::CWeakPointer<CDRMRenderer> self;
        std::vector<SGLFormat>                        formats;

      private:
        CDRMRenderer() = default;

        EGLImageKHR                                           createEGLImage(const SDMABUFAttrs& attrs);
        std::optional<std::vector<std::pair<uint64_t, bool>>> getModsForFormat(EGLint format);
        bool                                                  initDRMFormats();
        bool                                                  verifyDestinationDMABUF(const SDMABUFAttrs& attrs);
        void                                                  waitOnSync(int fd);
        int                                                   recreateBlitSync();
        bool                                                  hasModifiers = false;

        Hyprutils::Memory::CWeakPointer<CBackend>             backend;
    };
};