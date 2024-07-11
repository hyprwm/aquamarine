#pragma once

#include <aquamarine/backend/DRM.hpp>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <gbm.h>

namespace Aquamarine {
    class CDRMRendererBufferAttachment : public IAttachment {
      public:
        CDRMRendererBufferAttachment(Hyprutils::Memory::CWeakPointer<CDRMRenderer> renderer_, Hyprutils::Memory::CSharedPointer<IBuffer> buffer, EGLImageKHR image, GLuint fbo_,
                                     GLuint rbo_, GLuint texid_);
        virtual ~CDRMRendererBufferAttachment() {
            ;
        }
        virtual eAttachmentType type() {
            return AQ_ATTACHMENT_DRM_RENDERER_DATA;
        }

        EGLImageKHR                                   eglImage = nullptr;
        GLuint                                        fbo = 0, rbo = 0, texid = 0;
        Hyprutils::Signal::CHyprSignalListener        bufferDestroy;

        Hyprutils::Memory::CWeakPointer<CDRMRenderer> renderer;
    };

    class CDRMRenderer {
      public:
        static Hyprutils::Memory::CSharedPointer<CDRMRenderer> attempt(int drmfd, Hyprutils::Memory::CSharedPointer<CBackend> backend_);

        int                                                    drmFD = -1;

        bool                                                   blit(Hyprutils::Memory::CSharedPointer<IBuffer> from, Hyprutils::Memory::CSharedPointer<IBuffer> to);

        void                                                   setEGL();
        void                                                   restoreEGL();

        void                                                   onBufferAttachmentDrop(CDRMRendererBufferAttachment* attachment);

        struct {
            struct {
                GLuint program = 0;
                GLint  proj = -1, tex = -1, posAttrib = -1, texAttrib = -1;
            } shader;
        } gl;

        struct {
            int         fd     = -1;
            gbm_device* device = nullptr;
        } gbm;

        struct {
            EGLDisplay                                    display = nullptr;
            EGLContext                                    context = nullptr;

            PFNEGLGETPLATFORMDISPLAYEXTPROC               eglGetPlatformDisplayEXT               = nullptr;
            PFNEGLCREATEIMAGEKHRPROC                      eglCreateImageKHR                      = nullptr;
            PFNEGLDESTROYIMAGEKHRPROC                     eglDestroyImageKHR                     = nullptr;
            PFNGLEGLIMAGETARGETTEXTURE2DOESPROC           glEGLImageTargetTexture2DOES           = nullptr;
            PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC glEGLImageTargetRenderbufferStorageOES = nullptr;
        } egl;

        struct {
            EGLDisplay display = nullptr;
            EGLContext context = nullptr;
            EGLSurface draw = nullptr, read = nullptr;
        } savedEGLState;

        struct GLTex {
            EGLImage image = nullptr;
            GLuint   texid = 0;
        };

        GLTex                                         glTex(Hyprutils::Memory::CSharedPointer<IBuffer> buf);

        Hyprutils::Memory::CWeakPointer<CDRMRenderer> self;

      private:
        CDRMRenderer() = default;

        EGLImageKHR                               createEGLImage(const SDMABUFAttrs& attrs);

        Hyprutils::Memory::CWeakPointer<CBackend> backend;
    };
};