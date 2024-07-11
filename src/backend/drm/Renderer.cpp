#include "Renderer.hpp"
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <cstring>
#include <fcntl.h>
#include "Math.hpp"
#include "Shared.hpp"

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
#define SP CSharedPointer
#define WP CWeakPointer

// static funcs
WP<CBackend> gBackend;

// ------------------- shader utils

GLuint compileShader(const GLuint& type, std::string src) {
    auto shader = glCreateShader(type);

    auto shaderSource = src.c_str();

    glShaderSource(shader, 1, (const GLchar**)&shaderSource, nullptr);
    glCompileShader(shader);

    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);

    if (ok == GL_FALSE)
        return 0;

    return shader;
}

GLuint createProgram(const std::string& vert, const std::string& frag) {
    auto vertCompiled = compileShader(GL_VERTEX_SHADER, vert);
    if (vertCompiled == 0)
        return 0;

    auto fragCompiled = compileShader(GL_FRAGMENT_SHADER, frag);
    if (fragCompiled == 0)
        return 0;

    auto prog = glCreateProgram();
    glAttachShader(prog, vertCompiled);
    glAttachShader(prog, fragCompiled);
    glLinkProgram(prog);

    glDetachShader(prog, vertCompiled);
    glDetachShader(prog, fragCompiled);
    glDeleteShader(vertCompiled);
    glDeleteShader(fragCompiled);

    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok == GL_FALSE)
        return 0;

    return prog;
}

inline const std::string VERT_SRC = R"#(
uniform mat3 proj;
attribute vec2 pos;
attribute vec2 texcoord;
varying vec2 v_texcoord;

void main() {
    gl_Position = vec4(proj * vec3(pos, 1.0), 1.0);
    v_texcoord = texcoord;
})#";

inline const std::string FRAG_SRC = R"#(
precision highp float;
varying vec2 v_texcoord; // is in 0-1
uniform sampler2D tex;

void main() {
    gl_FragColor = texture2D(tex, v_texcoord);
})#";

// ------------------- gbm stuff

static int openRenderNode(int drmFd) {
    auto renderName = drmGetRenderDeviceNameFromFd(drmFd);
    if (!renderName) {
        // This can happen on split render/display platforms, fallback to
        // primary node
        renderName = drmGetPrimaryDeviceNameFromFd(drmFd);
        if (!renderName) {
            gBackend->log(AQ_LOG_ERROR, "drmRenderer: drmGetPrimaryDeviceNameFromFd failed");
            return -1;
        }
        gBackend->log(AQ_LOG_WARNING, std::format("DRM dev {} has no render node, falling back to primary", renderName));

        drmVersion* render_version = drmGetVersion(drmFd);
        if (render_version && render_version->name) {
            if (strcmp(render_version->name, "evdi") == 0) {
                free(renderName);
                renderName = (char*)malloc(sizeof(char) * 15);
                strcpy(renderName, "/dev/dri/card0");
            }
            drmFreeVersion(render_version);
        }
    }

    int renderFD = open(renderName, O_RDWR | O_CLOEXEC);
    if (renderFD < 0)
        gBackend->log(AQ_LOG_ERROR, std::format("openRenderNode failed to open drm device {}", renderName));

    free(renderName);
    return renderFD;
}

// ------------------- egl stuff

inline void loadGLProc(void* pProc, const char* name) {
    void* proc = (void*)eglGetProcAddress(name);
    if (proc == NULL) {
        gBackend->log(AQ_LOG_ERROR, std::format("eglGetProcAddress({}) failed", name));
        abort();
    }
    *(void**)pProc = proc;
}

// -------------------

SP<CDRMRenderer> CDRMRenderer::attempt(int drmfd, SP<CBackend> backend_) {
    SP<CDRMRenderer> renderer = SP<CDRMRenderer>(new CDRMRenderer());
    renderer->drmFD           = drmfd;
    renderer->backend         = backend_;
    gBackend                  = backend_;

    const std::string EGLEXTENSIONS = (const char*)eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

    if (!EGLEXTENSIONS.contains("KHR_platform_gbm")) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, no gbm support");
        return nullptr;
    }

    // init gbm stuff

    renderer->gbm.fd = openRenderNode(drmfd);
    if (!renderer->gbm.fd) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, no gbm fd");
        return nullptr;
    }

    renderer->gbm.device = gbm_create_device(renderer->gbm.fd);
    if (!renderer->gbm.device) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, no gbm device");
        return nullptr;
    }

    // init egl

    if (eglBindAPI(EGL_OPENGL_ES_API) == EGL_FALSE) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, eglBindAPI failed");
        return nullptr;
    }

    loadGLProc(&renderer->egl.eglGetPlatformDisplayEXT, "eglGetPlatformDisplayEXT");
    loadGLProc(&renderer->egl.eglCreateImageKHR, "eglCreateImageKHR");
    loadGLProc(&renderer->egl.eglDestroyImageKHR, "eglDestroyImageKHR");
    loadGLProc(&renderer->egl.glEGLImageTargetTexture2DOES, "glEGLImageTargetTexture2DOES");
    loadGLProc(&renderer->egl.glEGLImageTargetRenderbufferStorageOES, "glEGLImageTargetRenderbufferStorageOES");

    if (!renderer->egl.eglGetPlatformDisplayEXT) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, no eglGetPlatformDisplayEXT");
        return nullptr;
    }

    if (!renderer->egl.eglCreateImageKHR) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, no eglCreateImageKHR");
        return nullptr;
    }

    std::vector<EGLint> attrs = {EGL_NONE};
    renderer->egl.display     = renderer->egl.eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, renderer->gbm.device, attrs.data());
    if (renderer->egl.display == EGL_NO_DISPLAY) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, eglGetPlatformDisplayEXT failed");
        return nullptr;
    }

    EGLint major, minor;
    if (eglInitialize(renderer->egl.display, &major, &minor) == EGL_FALSE) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, eglInitialize failed");
        return nullptr;
    }

    attrs.clear();

    const std::string EGLEXTENSIONS2 = (const char*)eglQueryString(renderer->egl.display, EGL_EXTENSIONS);

    if (EGLEXTENSIONS2.contains("IMG_context_priority")) {
        attrs.push_back(EGL_CONTEXT_PRIORITY_LEVEL_IMG);
        attrs.push_back(EGL_CONTEXT_PRIORITY_HIGH_IMG);
    }

    attrs.push_back(EGL_CONTEXT_MAJOR_VERSION);
    attrs.push_back(2);
    attrs.push_back(EGL_CONTEXT_MINOR_VERSION);
    attrs.push_back(0);
    attrs.push_back(EGL_CONTEXT_OPENGL_DEBUG);
    attrs.push_back(EGL_FALSE);

    attrs.push_back(EGL_NONE);

    renderer->egl.context = eglCreateContext(renderer->egl.display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs.data());
    if (renderer->egl.context == EGL_NO_CONTEXT) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, eglCreateContext failed");
        return nullptr;
    }

    if (EGLEXTENSIONS2.contains("IMG_context_priority")) {
        EGLint priority = EGL_CONTEXT_PRIORITY_MEDIUM_IMG;
        eglQueryContext(renderer->egl.display, renderer->egl.context, EGL_CONTEXT_PRIORITY_LEVEL_IMG, &priority);
        if (priority != EGL_CONTEXT_PRIORITY_HIGH_IMG)
            backend_->log(AQ_LOG_DEBUG, "CDRMRenderer: didnt get a high priority context");
        else
            backend_->log(AQ_LOG_DEBUG, "CDRMRenderer: got a high priority context");
    }

    // init shaders

    renderer->setEGL();

    renderer->gl.shader.program = createProgram(VERT_SRC, FRAG_SRC);
    if (renderer->gl.shader.program == 0) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, shader failed");
        return nullptr;
    }

    renderer->gl.shader.proj      = glGetUniformLocation(renderer->gl.shader.program, "proj");
    renderer->gl.shader.posAttrib = glGetAttribLocation(renderer->gl.shader.program, "pos");
    renderer->gl.shader.texAttrib = glGetAttribLocation(renderer->gl.shader.program, "texcoord");
    renderer->gl.shader.tex       = glGetUniformLocation(renderer->gl.shader.program, "tex");

    renderer->restoreEGL();

    backend_->log(AQ_LOG_DEBUG, "CDRMRenderer: success");

    return renderer;
}

void CDRMRenderer::setEGL() {
    savedEGLState.display = eglGetCurrentDisplay();
    savedEGLState.context = eglGetCurrentContext();
    savedEGLState.draw    = eglGetCurrentSurface(EGL_DRAW);
    savedEGLState.read    = eglGetCurrentSurface(EGL_READ);

    if (!eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl.context))
        backend->log(AQ_LOG_WARNING, "CDRMRenderer: setEGL eglMakeCurrent failed");
}

void CDRMRenderer::restoreEGL() {
    EGLDisplay dpy = savedEGLState.display ? savedEGLState.display : egl.display;

    // egl can't handle this
    if (dpy == EGL_NO_DISPLAY)
        return;

    if (!eglMakeCurrent(dpy, savedEGLState.draw, savedEGLState.read, savedEGLState.context))
        backend->log(AQ_LOG_WARNING, "CDRMRenderer: restoreEGL eglMakeCurrent failed");
}

EGLImageKHR CDRMRenderer::createEGLImage(const SDMABUFAttrs& attrs) {
    std::vector<uint32_t> attribs;

    attribs.push_back(EGL_WIDTH);
    attribs.push_back(attrs.size.x);
    attribs.push_back(EGL_HEIGHT);
    attribs.push_back(attrs.size.y);
    attribs.push_back(EGL_LINUX_DRM_FOURCC_EXT);
    attribs.push_back(attrs.format);

    struct {
        EGLint fd;
        EGLint offset;
        EGLint pitch;
        EGLint modlo;
        EGLint modhi;
    } attrNames[4] = {
        {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
        {EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT}};

    for (int i = 0; i < attrs.planes; i++) {
        attribs.push_back(attrNames[i].fd);
        attribs.push_back(attrs.fds[i]);
        attribs.push_back(attrNames[i].offset);
        attribs.push_back(attrs.offsets[i]);
        attribs.push_back(attrNames[i].pitch);
        attribs.push_back(attrs.strides[i]);
        if (attrs.modifier != DRM_FORMAT_MOD_INVALID) { // FIXME: this will implode if we don't support mods. Does anyone not support them??
            attribs.push_back(attrNames[i].modlo);
            attribs.push_back(attrs.modifier & 0xFFFFFFFF);
            attribs.push_back(attrNames[i].modhi);
            attribs.push_back(attrs.modifier >> 32);
        }
    }

    attribs.push_back(EGL_IMAGE_PRESERVED_KHR);
    attribs.push_back(EGL_TRUE);

    attribs.push_back(EGL_NONE);

    EGLImageKHR image = egl.eglCreateImageKHR(egl.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, (int*)attribs.data());
    if (image == EGL_NO_IMAGE_KHR) {
        backend->log(AQ_LOG_ERROR, std::format("EGL: EGLCreateImageKHR failed: {}", eglGetError()));
        return EGL_NO_IMAGE_KHR;
    }

    return image;
}

#define GLCALL(__CALL__)                                                                                                                                                           \
    {                                                                                                                                                                              \
        __CALL__;                                                                                                                                                                  \
        auto err = glGetError();                                                                                                                                                   \
        if (err != GL_NO_ERROR) {                                                                                                                                                  \
            backend->log(AQ_LOG_ERROR,                                                                                                                                             \
                         std::format("[GLES] Error in call at {}@{}: 0x{:x}", __LINE__,                                                                                            \
                                     ([]() constexpr -> std::string { return std::string(__FILE__).substr(std::string(__FILE__).find_last_of('/') + 1); })(), err));               \
        }                                                                                                                                                                          \
    }

CDRMRenderer::GLTex CDRMRenderer::glTex(Hyprutils::Memory::CSharedPointer<IBuffer> buffa) {
    GLTex tex;

    tex.image = createEGLImage(buffa->dmabuf());
    if (tex.image == EGL_NO_IMAGE_KHR) {
        backend->log(AQ_LOG_ERROR, std::format("EGL (glTex): createEGLImage failed: {}", eglGetError()));
        return tex;
    }

    GLCALL(glGenTextures(1, &tex.texid));

    GLCALL(glBindTexture(GL_TEXTURE_2D, tex.texid));
    GLCALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GLCALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    egl.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, tex.image);
    GLCALL(glBindTexture(GL_TEXTURE_2D, 0));

    return tex;
}

inline const float fullVerts[] = {
    1, 0, // top right
    0, 0, // top left
    1, 1, // bottom right
    0, 1, // bottom left
};

bool CDRMRenderer::blit(SP<IBuffer> from, SP<IBuffer> to) {
    setEGL();

    if (from->dmabuf().size != to->dmabuf().size) {
        backend->log(AQ_LOG_ERROR, "EGL (blit): buffer sizes mismatched");
        return false;
    }

    // firstly, get a texture from the from buffer
    auto fromTex = glTex(from);

    TRACE(backend->log(AQ_LOG_TRACE, std::format("EGL (blit): fromTex id {}, image 0x{:x}", fromTex.texid, (uintptr_t)fromTex.image)));

    // then, get a rbo from our to buffer
    auto toDma = to->dmabuf();

    auto rboImage = createEGLImage(toDma);
    if (rboImage == EGL_NO_IMAGE_KHR) {
        backend->log(AQ_LOG_ERROR, std::format("EGL (blit): createEGLImage failed: {}", eglGetError()));
        return false;
    }

    TRACE(backend->log(AQ_LOG_TRACE, std::format("EGL (blit): rboImage 0x{:x}", (uintptr_t)rboImage)));

    GLuint rboID = 0, fboID = 0;

    // TODO: don't spam this?
    GLCALL(glGenRenderbuffers(1, &rboID));
    GLCALL(glBindRenderbuffer(GL_RENDERBUFFER, rboID));
    egl.glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, (GLeglImageOES)rboImage);
    GLCALL(glBindRenderbuffer(GL_RENDERBUFFER, 0));

    GLCALL(glGenFramebuffers(1, &fboID));
    GLCALL(glBindFramebuffer(GL_FRAMEBUFFER, fboID));
    GLCALL(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rboID));

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        backend->log(AQ_LOG_ERROR, std::format("EGL (blit): glCheckFramebufferStatus failed: {}", glGetError()));
        return false;
    }

    GLCALL(glBindRenderbuffer(GL_RENDERBUFFER, rboID));
    GLCALL(glBindFramebuffer(GL_FRAMEBUFFER, fboID));

    TRACE(backend->log(AQ_LOG_TRACE, std::format("EGL (blit): fbo {} rbo {}", fboID, rboID)));

    glClearColor(0.77F, 0.F, 0.74F, 1.F);
    glClear(GL_COLOR_BUFFER_BIT);

    // done, let's render the texture to the rbo
    CBox renderBox = {{}, toDma.size};

    TRACE(backend->log(AQ_LOG_TRACE, std::format("EGL (blit): box size {}", renderBox.size())));

    float mtx[9];
    float identity[9];
    float monitorProj[9];
    matrixIdentity(identity);
    projectBox(mtx, renderBox, HYPRUTILS_TRANSFORM_NORMAL, 0, identity);

    matrixProjection(monitorProj, toDma.size.x, toDma.size.y, HYPRUTILS_TRANSFORM_NORMAL);

    float glMtx[9];
    matrixMultiply(glMtx, monitorProj, mtx);

    GLCALL(glViewport(0, 0, toDma.size.x, toDma.size.y));

    GLCALL(glActiveTexture(GL_TEXTURE0));
    GLCALL(glBindTexture(GL_TEXTURE_2D, fromTex.texid));
    GLCALL(glUseProgram(gl.shader.program));
    GLCALL(glDisable(GL_BLEND));
    GLCALL(glDisable(GL_SCISSOR_TEST));

    matrixTranspose(glMtx, glMtx);
    GLCALL(glUniformMatrix3fv(gl.shader.proj, 1, GL_FALSE, glMtx));

    GLCALL(glUniform1i(gl.shader.tex, 0));

    GLCALL(glVertexAttribPointer(gl.shader.posAttrib, 2, GL_FLOAT, GL_FALSE, 0, fullVerts));
    GLCALL(glVertexAttribPointer(gl.shader.texAttrib, 2, GL_FLOAT, GL_FALSE, 0, fullVerts));

    GLCALL(glEnableVertexAttribArray(gl.shader.posAttrib));
    GLCALL(glEnableVertexAttribArray(gl.shader.texAttrib));

    GLCALL(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));

    GLCALL(glDisableVertexAttribArray(gl.shader.posAttrib));
    GLCALL(glDisableVertexAttribArray(gl.shader.texAttrib));

    GLCALL(glBindTexture(GL_TEXTURE_2D, 0));

    // rendered, cleanup

    GLCALL(glBindRenderbuffer(GL_RENDERBUFFER, 0));
    GLCALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));

    GLCALL(glDeleteTextures(1, &fromTex.texid));

    GLCALL(glDeleteRenderbuffers(1, &rboID));
    GLCALL(glDeleteFramebuffers(1, &fboID));

    egl.eglDestroyImageKHR(egl.display, rboImage);
    egl.eglDestroyImageKHR(egl.display, fromTex.image);

    restoreEGL();

    return true;
}
