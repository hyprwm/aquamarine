#include "Renderer.hpp"
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include "Math.hpp"
#include "Shared.hpp"
#include "FormatUtils.hpp"
#include <aquamarine/allocator/GBM.hpp>

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

inline const std::string FRAG_SRC_EXT = R"#(
#extension GL_OES_EGL_image_external : require
precision highp float;
varying vec2 v_texcoord; // is in 0-1
uniform samplerExternalOES texture0;

void main() {
    gl_FragColor = texture2D(texture0, v_texcoord);
})#";

// ------------------- egl stuff

inline void loadGLProc(void* pProc, const char* name) {
    void* proc = (void*)eglGetProcAddress(name);
    if (!proc) {
        gBackend->log(AQ_LOG_ERROR, std::format("eglGetProcAddress({}) failed", name));
        abort();
    }
    *(void**)pProc = proc;
}

// -------------------

std::optional<std::vector<std::pair<uint64_t, bool>>> CDRMRenderer::getModsForFormat(EGLint format) {
    // TODO: return std::expected when clang supports it

    EGLint len = 0;
    if (!egl.eglQueryDmaBufModifiersEXT(egl.display, format, 0, nullptr, nullptr, &len)) {
        backend->log(AQ_LOG_ERROR, std::format("EGL: eglQueryDmaBufModifiersEXT failed for format {}", fourccToName(format)));
        return std::nullopt;
    }

    if (len <= 0)
        return std::vector<std::pair<uint64_t, bool>>{};

    std::vector<uint64_t>   mods;
    std::vector<EGLBoolean> external;

    mods.resize(len);
    external.resize(len);

    egl.eglQueryDmaBufModifiersEXT(egl.display, format, len, mods.data(), external.data(), &len);

    std::vector<std::pair<uint64_t, bool>> result;
    result.reserve(mods.size());
    for (size_t i = 0; i < mods.size(); ++i) {
        result.emplace_back(mods.at(i), external.at(i));
    }

    if (std::ranges::find(mods, DRM_FORMAT_MOD_LINEAR) == mods.end() && mods.size() == 0)
        result.emplace_back(DRM_FORMAT_MOD_LINEAR, true);

    return result;
}

bool CDRMRenderer::initDRMFormats() {
    std::vector<EGLint> formats;

    EGLint              len = 0;
    egl.eglQueryDmaBufFormatsEXT(egl.display, 0, nullptr, &len);
    formats.resize(len);
    egl.eglQueryDmaBufFormatsEXT(egl.display, len, formats.data(), &len);

    if (formats.size() == 0) {
        backend->log(AQ_LOG_ERROR, "EGL: Failed to get formats");
        return false;
    }

    TRACE(backend->log(AQ_LOG_TRACE, "EGL: Supported formats:"));

    std::vector<SGLFormat> dmaFormats;

    for (auto const& fmt : formats) {
        std::vector<std::pair<uint64_t, bool>> mods;

        auto                                   ret = getModsForFormat(fmt);
        if (!ret.has_value())
            continue;

        mods = *ret;

        hasModifiers = hasModifiers || mods.size() > 0;

        // EGL can always do implicit modifiers.
        mods.emplace_back(DRM_FORMAT_MOD_INVALID, true);

        for (auto const& [mod, external] : mods) {
            dmaFormats.push_back(SGLFormat{
                .drmFormat = (uint32_t)fmt,
                .modifier  = mod,
                .external  = external,
            });
        }

        TRACE(backend->log(AQ_LOG_TRACE, std::format("EGL: GPU Supports Format {} (0x{:x})", fourccToName((uint32_t)fmt), fmt)));
        for (auto const& [mod, external] : mods) {
            auto modName = drmGetFormatModifierName(mod);
            TRACE(backend->log(AQ_LOG_TRACE, std::format("EGL:  | {}with modifier 0x{:x}: {}", (external ? "external only " : ""), mod, modName ? modName : "?unknown?")));
            free(modName);
        }
    }

    TRACE(backend->log(AQ_LOG_TRACE, std::format("EGL: Found {} formats", dmaFormats.size())));

    if (dmaFormats.empty()) {
        backend->log(AQ_LOG_ERROR, "EGL: No formats");
        return false;
    }

    this->formats = dmaFormats;
    return true;
}

Aquamarine::CDRMRenderer::~CDRMRenderer() {
    eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(egl.display, egl.context);

    eglTerminate(egl.display);

    eglReleaseThread();
}

SP<CDRMRenderer> CDRMRenderer::attempt(Hyprutils::Memory::CSharedPointer<CGBMAllocator> allocator_, SP<CBackend> backend_) {
    SP<CDRMRenderer> renderer = SP<CDRMRenderer>(new CDRMRenderer());
    renderer->drmFD           = allocator_->drmFD();
    renderer->backend         = backend_;
    gBackend                  = backend_;

    const std::string EGLEXTENSIONS = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

    if (!EGLEXTENSIONS.contains("KHR_platform_gbm")) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, no gbm support");
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
    loadGLProc(&renderer->egl.eglQueryDmaBufFormatsEXT, "eglQueryDmaBufFormatsEXT");
    loadGLProc(&renderer->egl.eglQueryDmaBufModifiersEXT, "eglQueryDmaBufModifiersEXT");
    loadGLProc(&renderer->egl.eglDestroySyncKHR, "eglDestroySyncKHR");
    loadGLProc(&renderer->egl.eglWaitSyncKHR, "eglWaitSyncKHR");
    loadGLProc(&renderer->egl.eglCreateSyncKHR, "eglCreateSyncKHR");
    loadGLProc(&renderer->egl.eglDupNativeFenceFDANDROID, "eglDupNativeFenceFDANDROID");

    if (!renderer->egl.eglCreateSyncKHR) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, no eglCreateSyncKHR");
        return nullptr;
    }

    if (!renderer->egl.eglDupNativeFenceFDANDROID) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, no eglDupNativeFenceFDANDROID");
        return nullptr;
    }

    if (!renderer->egl.eglGetPlatformDisplayEXT) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, no eglGetPlatformDisplayEXT");
        return nullptr;
    }

    if (!renderer->egl.eglCreateImageKHR) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, no eglCreateImageKHR");
        return nullptr;
    }

    if (!renderer->egl.eglQueryDmaBufFormatsEXT) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, no eglQueryDmaBufFormatsEXT");
        return nullptr;
    }

    if (!renderer->egl.eglQueryDmaBufModifiersEXT) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, no eglQueryDmaBufModifiersEXT");
        return nullptr;
    }

    std::vector<EGLint> attrs = {EGL_NONE};
    renderer->egl.display     = renderer->egl.eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, allocator_->gbmDevice, attrs.data());
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

    const std::string EGLEXTENSIONS2 = eglQueryString(renderer->egl.display, EGL_EXTENSIONS);

    if (EGLEXTENSIONS2.contains("IMG_context_priority")) {
        attrs.push_back(EGL_CONTEXT_PRIORITY_LEVEL_IMG);
        attrs.push_back(EGL_CONTEXT_PRIORITY_HIGH_IMG);
    }

    if (EGLEXTENSIONS2.contains("EXT_create_context_robustness")) {
        attrs.push_back(EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT);
        attrs.push_back(EGL_LOSE_CONTEXT_ON_RESET_EXT);
    }

    if (!EGLEXTENSIONS2.contains("EXT_image_dma_buf_import_modifiers")) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, no EXT_image_dma_buf_import_modifiers ext");
        return nullptr;
    }

    if (!EGLEXTENSIONS2.contains("EXT_image_dma_buf_import")) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, no EXT_image_dma_buf_import ext");
        return nullptr;
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

    if (!renderer->initDRMFormats()) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, initDRMFormats failed");
        return nullptr;
    }

    renderer->gl.shader.program = createProgram(VERT_SRC, FRAG_SRC);
    if (renderer->gl.shader.program == 0) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, shader failed");
        return nullptr;
    }

    renderer->gl.shader.proj      = glGetUniformLocation(renderer->gl.shader.program, "proj");
    renderer->gl.shader.posAttrib = glGetAttribLocation(renderer->gl.shader.program, "pos");
    renderer->gl.shader.texAttrib = glGetAttribLocation(renderer->gl.shader.program, "texcoord");
    renderer->gl.shader.tex       = glGetUniformLocation(renderer->gl.shader.program, "tex");

    renderer->gl.shaderExt.program = createProgram(VERT_SRC, FRAG_SRC_EXT);
    if (renderer->gl.shaderExt.program == 0) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, shaderExt failed");
        return nullptr;
    }

    renderer->gl.shaderExt.proj      = glGetUniformLocation(renderer->gl.shaderExt.program, "proj");
    renderer->gl.shaderExt.posAttrib = glGetAttribLocation(renderer->gl.shaderExt.program, "pos");
    renderer->gl.shaderExt.texAttrib = glGetAttribLocation(renderer->gl.shaderExt.program, "texcoord");
    renderer->gl.shaderExt.tex       = glGetUniformLocation(renderer->gl.shaderExt.program, "tex");

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

    TRACE(backend->log(AQ_LOG_TRACE, std::format("EGL: createEGLImage: size {} with format {} and modifier 0x{:x}", attrs.size, fourccToName(attrs.format), attrs.modifier)));

    struct {
        EGLint fd;
        EGLint offset;
        EGLint pitch;
        EGLint modlo;
        EGLint modhi;
    } attrNames[4] = {{.fd     = EGL_DMA_BUF_PLANE0_FD_EXT,
                       .offset = EGL_DMA_BUF_PLANE0_OFFSET_EXT,
                       .pitch  = EGL_DMA_BUF_PLANE0_PITCH_EXT,
                       .modlo  = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
                       .modhi  = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT},
                      {.fd     = EGL_DMA_BUF_PLANE1_FD_EXT,
                       .offset = EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                       .pitch  = EGL_DMA_BUF_PLANE1_PITCH_EXT,
                       .modlo  = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
                       .modhi  = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT},
                      {.fd     = EGL_DMA_BUF_PLANE2_FD_EXT,
                       .offset = EGL_DMA_BUF_PLANE2_OFFSET_EXT,
                       .pitch  = EGL_DMA_BUF_PLANE2_PITCH_EXT,
                       .modlo  = EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT,
                       .modhi  = EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT},
                      {.fd     = EGL_DMA_BUF_PLANE3_FD_EXT,
                       .offset = EGL_DMA_BUF_PLANE3_OFFSET_EXT,
                       .pitch  = EGL_DMA_BUF_PLANE3_PITCH_EXT,
                       .modlo  = EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT,
                       .modhi  = EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT}};

    for (int i = 0; i < attrs.planes; i++) {
        attribs.push_back(attrNames[i].fd);
        attribs.push_back(attrs.fds[i]);
        attribs.push_back(attrNames[i].offset);
        attribs.push_back(attrs.offsets[i]);
        attribs.push_back(attrNames[i].pitch);
        attribs.push_back(attrs.strides[i]);
        if (hasModifiers && attrs.modifier != DRM_FORMAT_MOD_INVALID) {
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

SGLTex CDRMRenderer::glTex(Hyprutils::Memory::CSharedPointer<IBuffer> buffa) {
    SGLTex     tex;

    const auto dma = buffa->dmabuf();

    tex.image = createEGLImage(dma);
    if (tex.image == EGL_NO_IMAGE_KHR) {
        backend->log(AQ_LOG_ERROR, std::format("EGL (glTex): createEGLImage failed: {}", eglGetError()));
        return tex;
    }

    bool external = false;
    for (auto const& fmt : formats) {
        if (fmt.drmFormat != dma.format || fmt.modifier != dma.modifier)
            continue;

        backend->log(AQ_LOG_DEBUG, std::format("CDRMRenderer::glTex: found format+mod, external = {}", fmt.external));
        external = fmt.external;
        break;
    }

    tex.target = external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;

    GLCALL(glGenTextures(1, &tex.texid));

    GLCALL(glBindTexture(tex.target, tex.texid));
    GLCALL(glTexParameteri(tex.target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GLCALL(glTexParameteri(tex.target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    GLCALL(egl.glEGLImageTargetTexture2DOES(tex.target, tex.image));
    GLCALL(glBindTexture(tex.target, 0));

    return tex;
}

inline const float fullVerts[] = {
    1, 0, // top right
    0, 0, // top left
    1, 1, // bottom right
    0, 1, // bottom left
};

void CDRMRenderer::waitOnSync(int fd) {
    TRACE(backend->log(AQ_LOG_TRACE, std::format("EGL (waitOnSync): attempting to wait on fd {}", fd)));

    std::vector<EGLint> attribs;
    int                 dupFd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (dupFd < 0) {
        backend->log(AQ_LOG_TRACE, "EGL (waitOnSync): failed to dup fd for wait");
        return;
    }

    attribs.push_back(EGL_SYNC_NATIVE_FENCE_FD_ANDROID);
    attribs.push_back(dupFd);
    attribs.push_back(EGL_NONE);

    EGLSyncKHR sync = egl.eglCreateSyncKHR(egl.display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs.data());
    if (sync == EGL_NO_SYNC_KHR) {
        TRACE(backend->log(AQ_LOG_TRACE, "EGL (waitOnSync): failed to create an egl sync for explicit"));
        if (dupFd >= 0)
            close(dupFd);
        return;
    }

    // we got a sync, now we just tell egl to wait before sampling
    if (egl.eglWaitSyncKHR(egl.display, sync, 0) != EGL_TRUE) {
        if (egl.eglDestroySyncKHR(egl.display, sync) != EGL_TRUE)
            TRACE(backend->log(AQ_LOG_TRACE, "EGL (waitOnSync): failed to destroy sync"));

        TRACE(backend->log(AQ_LOG_TRACE, "EGL (waitOnSync): failed to wait on the sync object"));
        return;
    }

    if (egl.eglDestroySyncKHR(egl.display, sync) != EGL_TRUE)
        TRACE(backend->log(AQ_LOG_TRACE, "EGL (waitOnSync): failed to destroy sync"));
}

int CDRMRenderer::recreateBlitSync() {
    TRACE(backend->log(AQ_LOG_TRACE, "EGL (recreateBlitSync): recreating blit sync"));

    if (egl.lastBlitSync) {
        TRACE(backend->log(AQ_LOG_TRACE, std::format("EGL (recreateBlitSync): cleaning up old sync (fd {})", egl.lastBlitSyncFD)));

        // cleanup last sync
        if (egl.eglDestroySyncKHR(egl.display, egl.lastBlitSync) != EGL_TRUE)
            TRACE(backend->log(AQ_LOG_TRACE, "EGL (recreateBlitSync): failed to destroy old sync"));

        if (egl.lastBlitSyncFD >= 0)
            close(egl.lastBlitSyncFD);

        egl.lastBlitSyncFD = -1;
        egl.lastBlitSync   = nullptr;
    }

    EGLSyncKHR sync = egl.eglCreateSyncKHR(egl.display, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    if (sync == EGL_NO_SYNC_KHR) {
        TRACE(backend->log(AQ_LOG_TRACE, "EGL (recreateBlitSync): failed to create an egl sync for explicit"));
        return -1;
    }

    // we need to flush otherwise we might not get a valid fd
    glFlush();

    int fd = egl.eglDupNativeFenceFDANDROID(egl.display, sync);
    if (fd == EGL_NO_NATIVE_FENCE_FD_ANDROID) {
        TRACE(backend->log(AQ_LOG_TRACE, "EGL (recreateBlitSync): failed to dup egl fence fd"));
        if (egl.eglDestroySyncKHR(egl.display, sync) != EGL_TRUE)
            TRACE(backend->log(AQ_LOG_TRACE, "EGL (recreateBlitSync): failed to destroy new sync"));
        return -1;
    }

    egl.lastBlitSync   = sync;
    egl.lastBlitSyncFD = fd;

    TRACE(backend->log(AQ_LOG_TRACE, std::format("EGL (recreateBlitSync): success, new fence exported with fd {}", fd)));

    return fd;
}

void CDRMRenderer::clearBuffer(IBuffer* buf) {
    setEGL();

    auto   dmabuf = buf->dmabuf();
    GLuint rboID = 0, fboID = 0;

    if (!dmabuf.success) {
        backend->log(AQ_LOG_ERROR, "EGL (clear): cannot clear a non-dmabuf");
        return;
    }

    auto rboImage = createEGLImage(dmabuf);
    if (rboImage == EGL_NO_IMAGE_KHR) {
        backend->log(AQ_LOG_ERROR, std::format("EGL (clear): createEGLImage failed: {}", eglGetError()));
        return;
    }

    GLCALL(glGenRenderbuffers(1, &rboID));
    GLCALL(glBindRenderbuffer(GL_RENDERBUFFER, rboID));
    GLCALL(egl.glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, (GLeglImageOES)rboImage));
    GLCALL(glBindRenderbuffer(GL_RENDERBUFFER, 0));

    GLCALL(glGenFramebuffers(1, &fboID));
    GLCALL(glBindFramebuffer(GL_FRAMEBUFFER, fboID));
    GLCALL(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rboID));

    GLCALL(glBindRenderbuffer(GL_RENDERBUFFER, rboID));
    GLCALL(glBindFramebuffer(GL_FRAMEBUFFER, fboID));

    TRACE(backend->log(AQ_LOG_TRACE, std::format("EGL (clear): fbo {} rbo {}", fboID, rboID)));

    glClearColor(0.F, 0.F, 0.F, 1.F);
    glClear(GL_COLOR_BUFFER_BIT);

    glFlush();

    GLCALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    GLCALL(glBindRenderbuffer(GL_RENDERBUFFER, 0));

    glDeleteFramebuffers(1, &fboID);
    glDeleteRenderbuffers(1, &rboID);
    egl.eglDestroyImageKHR(egl.display, rboImage);

    restoreEGL();
}

CDRMRenderer::SBlitResult CDRMRenderer::blit(SP<IBuffer> from, SP<IBuffer> to, int waitFD) {
    setEGL();

    if (from->dmabuf().size != to->dmabuf().size) {
        backend->log(AQ_LOG_ERROR, "EGL (blit): buffer sizes mismatched");
        return {};
    }

    if (waitFD >= 0) {
        // wait on a provided explicit fence
        waitOnSync(waitFD);
    }

    // firstly, get a texture from the from buffer
    // if it has an attachment, use that
    // both from and to have the same AQ_ATTACHMENT_DRM_RENDERER_DATA.
    // Those buffers always come from different swapchains, so it's OK.

    SGLTex fromTex;
    {
        auto attachment = from->attachments.get(AQ_ATTACHMENT_DRM_RENDERER_DATA);
        if (attachment) {
            TRACE(backend->log(AQ_LOG_TRACE, "EGL (blit): From attachment found"));
            auto att = (CDRMRendererBufferAttachment*)attachment.get();
            fromTex  = att->tex;
        }

        if (!fromTex.image) {
            backend->log(AQ_LOG_DEBUG, "EGL (blit): No attachment in from, creating a new image");
            fromTex = glTex(from);

            // should never remove anything, but JIC. We'll leak an EGLImage if this removes anything.
            from->attachments.removeByType(AQ_ATTACHMENT_DRM_RENDERER_DATA);
            from->attachments.add(makeShared<CDRMRendererBufferAttachment>(self, from, nullptr, 0, 0, fromTex));
        }
    }

    TRACE(backend->log(AQ_LOG_TRACE,
                       std::format("EGL (blit): fromTex id {}, image 0x{:x}, target {}", fromTex.texid, (uintptr_t)fromTex.image,
                                   fromTex.target == GL_TEXTURE_2D ? "GL_TEXTURE_2D" : "GL_TEXTURE_EXTERNAL_OES")));

    // then, get a rbo from our to buffer
    // if it has an attachment, use that

    EGLImageKHR rboImage = nullptr;
    GLuint      rboID = 0, fboID = 0;
    auto        toDma = to->dmabuf();

    if (!verifyDestinationDMABUF(toDma)) {
        backend->log(AQ_LOG_ERROR, "EGL (blit): failed to blit: destination dmabuf unsupported");
        return {};
    }

    {
        auto attachment = to->attachments.get(AQ_ATTACHMENT_DRM_RENDERER_DATA);
        if (attachment) {
            TRACE(backend->log(AQ_LOG_TRACE, "EGL (blit): To attachment found"));
            auto att = (CDRMRendererBufferAttachment*)attachment.get();
            rboImage = att->eglImage;
            fboID    = att->fbo;
            rboID    = att->rbo;
        }

        if (!rboImage) {
            backend->log(AQ_LOG_DEBUG, "EGL (blit): No attachment in to, creating a new image");

            rboImage = createEGLImage(toDma);
            if (rboImage == EGL_NO_IMAGE_KHR) {
                backend->log(AQ_LOG_ERROR, std::format("EGL (blit): createEGLImage failed: {}", eglGetError()));
                return {};
            }

            GLCALL(glGenRenderbuffers(1, &rboID));
            GLCALL(glBindRenderbuffer(GL_RENDERBUFFER, rboID));
            GLCALL(egl.glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, (GLeglImageOES)rboImage));
            GLCALL(glBindRenderbuffer(GL_RENDERBUFFER, 0));

            GLCALL(glGenFramebuffers(1, &fboID));
            GLCALL(glBindFramebuffer(GL_FRAMEBUFFER, fboID));
            GLCALL(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rboID));

            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                backend->log(AQ_LOG_ERROR, std::format("EGL (blit): glCheckFramebufferStatus failed: {}", glGetError()));
                return {};
            }

            // should never remove anything, but JIC. We'll leak an RBO and FBO if this removes anything.
            to->attachments.removeByType(AQ_ATTACHMENT_DRM_RENDERER_DATA);
            to->attachments.add(makeShared<CDRMRendererBufferAttachment>(self, to, rboImage, fboID, rboID, SGLTex{}));
        }
    }

    glFlush();

    TRACE(backend->log(AQ_LOG_TRACE, std::format("EGL (blit): rboImage 0x{:x}", (uintptr_t)rboImage)));

    GLCALL(glBindRenderbuffer(GL_RENDERBUFFER, rboID));
    GLCALL(glBindFramebuffer(GL_FRAMEBUFFER, fboID));

    TRACE(backend->log(AQ_LOG_TRACE, std::format("EGL (blit): fbo {} rbo {}", fboID, rboID)));

    glClearColor(0.77F, 0.F, 0.74F, 1.F);
    glClear(GL_COLOR_BUFFER_BIT);

    // done, let's render the texture to the rbo
    CBox renderBox = {{}, toDma.size};

    TRACE(backend->log(AQ_LOG_TRACE, std::format("EGL (blit): box size {}", renderBox.size())));

    float mtx[9];
    float base[9];
    float monitorProj[9];
    matrixIdentity(base);

    auto& SHADER = fromTex.target == GL_TEXTURE_2D ? gl.shader : gl.shaderExt;

    // KMS uses flipped y, we have to do FLIPPED_180
    matrixTranslate(base, toDma.size.x / 2.0, toDma.size.y / 2.0);
    matrixTransform(base, HYPRUTILS_TRANSFORM_FLIPPED_180);
    matrixTranslate(base, -toDma.size.x / 2.0, -toDma.size.y / 2.0);

    projectBox(mtx, renderBox, HYPRUTILS_TRANSFORM_FLIPPED_180, 0, base);

    matrixProjection(monitorProj, toDma.size.x, toDma.size.y, HYPRUTILS_TRANSFORM_FLIPPED_180);

    float glMtx[9];
    matrixMultiply(glMtx, monitorProj, mtx);

    GLCALL(glViewport(0, 0, toDma.size.x, toDma.size.y));

    GLCALL(glActiveTexture(GL_TEXTURE0));
    GLCALL(glBindTexture(fromTex.target, fromTex.texid));

    GLCALL(glTexParameteri(fromTex.target, GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    GLCALL(glTexParameteri(fromTex.target, GL_TEXTURE_MIN_FILTER, GL_NEAREST));

    GLCALL(glUseProgram(SHADER.program));
    GLCALL(glDisable(GL_BLEND));
    GLCALL(glDisable(GL_SCISSOR_TEST));

    matrixTranspose(glMtx, glMtx);
    GLCALL(glUniformMatrix3fv(SHADER.proj, 1, GL_FALSE, glMtx));

    GLCALL(glUniform1i(SHADER.tex, 0));

    GLCALL(glVertexAttribPointer(SHADER.posAttrib, 2, GL_FLOAT, GL_FALSE, 0, fullVerts));
    GLCALL(glVertexAttribPointer(SHADER.texAttrib, 2, GL_FLOAT, GL_FALSE, 0, fullVerts));

    GLCALL(glEnableVertexAttribArray(SHADER.posAttrib));
    GLCALL(glEnableVertexAttribArray(SHADER.texAttrib));

    GLCALL(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));

    GLCALL(glDisableVertexAttribArray(SHADER.posAttrib));
    GLCALL(glDisableVertexAttribArray(SHADER.texAttrib));

    GLCALL(glBindTexture(fromTex.target, 0));

    // rendered, cleanup
    glFlush();

    // get an explicit sync fd for the secondary gpu.
    // when we pass buffers between gpus we should always use explicit sync,
    // as implicit is not guaranteed at all
    int explicitFD = recreateBlitSync();

    GLCALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    GLCALL(glBindRenderbuffer(GL_RENDERBUFFER, 0));

    restoreEGL();

    return {.success = true, .syncFD = explicitFD == -1 ? std::nullopt : std::optional<int>{explicitFD}};
}

void CDRMRenderer::onBufferAttachmentDrop(CDRMRendererBufferAttachment* attachment) {
    setEGL();

    TRACE(backend->log(AQ_LOG_TRACE,
                       std::format("EGL (onBufferAttachmentDrop): dropping fbo {} rbo {} image 0x{:x}", attachment->fbo, attachment->rbo, (uintptr_t)attachment->eglImage)));

    if (attachment->tex.texid)
        GLCALL(glDeleteTextures(1, &attachment->tex.texid));
    if (attachment->rbo)
        GLCALL(glDeleteRenderbuffers(1, &attachment->rbo));
    if (attachment->fbo)
        GLCALL(glDeleteFramebuffers(1, &attachment->fbo));
    if (attachment->eglImage)
        egl.eglDestroyImageKHR(egl.display, attachment->eglImage);
    if (attachment->tex.image)
        egl.eglDestroyImageKHR(egl.display, attachment->tex.image);

    restoreEGL();
}

bool CDRMRenderer::verifyDestinationDMABUF(const SDMABUFAttrs& attrs) {
    for (auto const& fmt : formats) {
        if (fmt.drmFormat != attrs.format)
            continue;

        if (fmt.modifier != attrs.modifier)
            continue;

        return true;
    }

    backend->log(AQ_LOG_ERROR, "EGL (verifyDestinationDMABUF): FAIL, format is unsupported by EGL");
    return false;
}

CDRMRendererBufferAttachment::CDRMRendererBufferAttachment(Hyprutils::Memory::CWeakPointer<CDRMRenderer> renderer_, Hyprutils::Memory::CSharedPointer<IBuffer> buffer,
                                                           EGLImageKHR image, GLuint fbo_, GLuint rbo_, SGLTex tex_) :
    eglImage(image), fbo(fbo_), rbo(rbo_), renderer(renderer_), tex(tex_) {
    bufferDestroy = buffer->events.destroy.registerListener([this](std::any d) { renderer->onBufferAttachmentDrop(this); });
}
