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
#include <hyprutils/os/FileDescriptor.hpp>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;
using namespace Hyprutils::OS;

#define SP CSharedPointer
#define WP CWeakPointer

// macros
#define GLCALL(__CALL__)                                                                                                                                                           \
    {                                                                                                                                                                              \
        __CALL__;                                                                                                                                                                  \
        if (Aquamarine::isTrace()) {                                                                                                                                               \
            auto err = glGetError();                                                                                                                                               \
            if (err != GL_NO_ERROR) {                                                                                                                                              \
                backend->log(AQ_LOG_ERROR,                                                                                                                                         \
                             std::format("[GLES] Error in call at {}@{}: 0x{:x}", __LINE__,                                                                                        \
                                         ([]() constexpr -> std::string { return std::string(__FILE__).substr(std::string(__FILE__).find_last_of('/') + 1); })(), err));           \
            }                                                                                                                                                                      \
        }                                                                                                                                                                          \
    }

// static funcs
static WP<CBackend> gBackend;

// ------------------- shader utils

static GLuint compileShader(const GLuint& type, std::string src) {
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

static GLuint createProgram(const std::string& vert, const std::string& frag) {
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
#version 300 es
precision highp float;

uniform mat3 proj;

in vec2 pos;
in vec2 texcoord;

out vec2 v_texcoord;

void main() {
    gl_Position = vec4(proj * vec3(pos, 1.0), 1.0);
    v_texcoord = texcoord;
})#";

inline const std::string FRAG_SRC = R"#(
#version 300 es
precision highp float;

in vec2 v_texcoord;
out vec4 fragColor;

uniform sampler2D tex;

void main() {
    fragColor = texture(tex, v_texcoord);
})#";

inline const std::string FRAG_SRC_EXT = R"#(
#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;

in vec2 v_texcoord;
out vec4 fragColor;

uniform samplerExternalOES texture0;

void main() {
    fragColor = texture(texture0, v_texcoord);
})#";

// ------------------- egl stuff

static inline void loadGLProc(void* pProc, const char* name) {
    void* proc = (void*)eglGetProcAddress(name);
    if (!proc) {
        gBackend->log(AQ_LOG_ERROR, std::format("eglGetProcAddress({}) failed, the display driver doesn't support it", name));
        abort();
    }
    *(void**)pProc = proc;
}

static enum eBackendLogLevel eglLogToLevel(EGLint type) {
    switch (type) {
        case EGL_DEBUG_MSG_CRITICAL_KHR: return AQ_LOG_CRITICAL;
        case EGL_DEBUG_MSG_ERROR_KHR: return AQ_LOG_ERROR;
        case EGL_DEBUG_MSG_WARN_KHR: return AQ_LOG_WARNING;
        case EGL_DEBUG_MSG_INFO_KHR: return AQ_LOG_DEBUG;
        default: return AQ_LOG_DEBUG;
    }
}

static const char* eglErrorToString(EGLint error) {
    switch (error) {
        case EGL_SUCCESS: return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
        case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
        case EGL_BAD_DEVICE_EXT: return "EGL_BAD_DEVICE_EXT";
        case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
        case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
        case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
        case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
        case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
        default: return "Unknown";
    }
}

static void eglLog(EGLenum error, const char* command, EGLint type, EGLLabelKHR thread, EGLLabelKHR obj, const char* msg) {
    gBackend->log(eglLogToLevel(type), std::format("[EGL] Command {} errored out with {} (0x{}): {}", command, eglErrorToString(error), error, msg));
}

static bool drmDeviceHasName(const drmDevice* device, const std::string& name) {
    for (size_t i = 0; i < DRM_NODE_MAX; i++) {
        if (!(device->available_nodes & (1 << i)))
            continue;

        if (device->nodes[i] == name)
            return true;
    }
    return false;
}

// -------------------

CDRMRenderer::SShader::~SShader() {
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

void CDRMRenderer::SShader::createVao() {
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
        glVertexAttribPointer(posAttrib, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }

    if (texAttrib != -1) {
        glGenBuffers(1, &shaderVboUv);
        glBindBuffer(GL_ARRAY_BUFFER, shaderVboUv);
        glBufferData(GL_ARRAY_BUFFER, sizeof(fullVerts), fullVerts, GL_STATIC_DRAW);
        glEnableVertexAttribArray(texAttrib);
        glVertexAttribPointer(texAttrib, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

EGLDeviceEXT CDRMRenderer::eglDeviceFromDRMFD(int drmFD) {
    EGLint nDevices = 0;
    if (!proc.eglQueryDevicesEXT(0, nullptr, &nDevices)) {
        backend->log(AQ_LOG_ERROR, "CDRMRenderer(drm): eglQueryDevicesEXT failed");
        return EGL_NO_DEVICE_EXT;
    }

    if (nDevices <= 0) {
        backend->log(AQ_LOG_ERROR, "CDRMRenderer(drm): no devices");
        return EGL_NO_DEVICE_EXT;
    }

    std::vector<EGLDeviceEXT> devices;
    devices.resize(nDevices);

    if (!proc.eglQueryDevicesEXT(nDevices, devices.data(), &nDevices)) {
        backend->log(AQ_LOG_ERROR, "CDRMRenderer(drm): eglQueryDevicesEXT failed (2)");
        return EGL_NO_DEVICE_EXT;
    }

    drmDevice* drmDev = nullptr;
    if (int ret = drmGetDevice(drmFD, &drmDev); ret < 0) {
        backend->log(AQ_LOG_ERROR, "CDRMRenderer(drm): drmGetDevice failed");
        drmFreeDevice(&drmDev);
        return EGL_NO_DEVICE_EXT;
    }

    for (auto const& d : devices) {
        auto devName = proc.eglQueryDeviceStringEXT(d, EGL_DRM_DEVICE_FILE_EXT);
        if (!devName)
            continue;

        if (drmDeviceHasName(drmDev, devName)) {
            backend->log(AQ_LOG_DEBUG, std::format("CDRMRenderer(drm): Using device {}", devName));
            drmFreeDevice(&drmDev);
            return d;
        }
    }

    drmFreeDevice(&drmDev);
    return EGL_NO_DEVICE_EXT;
}

std::optional<std::vector<std::pair<uint64_t, bool>>> CDRMRenderer::getModsForFormat(EGLint format) {
    // TODO: return std::expected when clang supports it

    EGLint len = 0;
    if (!proc.eglQueryDmaBufModifiersEXT(egl.display, format, 0, nullptr, nullptr, &len)) {
        backend->log(AQ_LOG_ERROR, std::format("EGL: eglQueryDmaBufModifiersEXT failed for format {}", fourccToName(format)));
        return std::nullopt;
    }

    if (len <= 0)
        return std::vector<std::pair<uint64_t, bool>>{};

    std::vector<uint64_t>   mods;
    std::vector<EGLBoolean> external;

    mods.resize(len);
    external.resize(len);

    proc.eglQueryDmaBufModifiersEXT(egl.display, format, len, mods.data(), external.data(), &len);

    std::vector<std::pair<uint64_t, bool>> result;
    result.reserve(mods.size());

    bool linearIsExternal = false;
    for (size_t i = 0; i < mods.size(); ++i) {
        if (external.at(i) && mods.at(i) == DRM_FORMAT_MOD_LINEAR)
            linearIsExternal = true;
        result.emplace_back(mods.at(i), external.at(i));
    }

    // if the driver doesn't mark linear as external, add it. It's allowed unless the driver says otherwise. (e.g. nvidia)
    if (!linearIsExternal && std::ranges::find(mods, DRM_FORMAT_MOD_LINEAR) == mods.end())
        result.emplace_back(DRM_FORMAT_MOD_LINEAR, true);

    return result;
}

void CDRMRenderer::useProgram(GLuint prog) {
    if (m_currentProgram == prog)
        return;

    GLCALL(glUseProgram(prog));
}

bool CDRMRenderer::initDRMFormats() {
    std::vector<EGLint> formats;

    EGLint              len = 0;
    proc.eglQueryDmaBufFormatsEXT(egl.display, 0, nullptr, &len);
    formats.resize(len);
    proc.eglQueryDmaBufFormatsEXT(egl.display, len, formats.data(), &len);

    if (formats.empty()) {
        backend->log(AQ_LOG_ERROR, "EGL: Failed to get formats");
        return false;
    }

    TRACE(backend->log(AQ_LOG_TRACE, "EGL: Supported formats:"));

    std::vector<SGLFormat> dmaFormats;
    dmaFormats.reserve(formats.size());

    for (auto const& fmt : formats) {
        std::vector<std::pair<uint64_t, bool>> mods;

        if (exts.EXT_image_dma_buf_import_modifiers) {
            auto ret = getModsForFormat(fmt);
            if (!ret.has_value())
                continue;

            mods = *ret;
        }

        hasModifiers = hasModifiers || !mods.empty();

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
    if (egl.display)
        eglMakeCurrent(egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    if (egl.display && egl.context != EGL_NO_CONTEXT && egl.context != nullptr)
        eglDestroyContext(egl.display, egl.context);

    if (egl.display)
        eglTerminate(egl.display);

    eglReleaseThread();
}

void CDRMRenderer::loadEGLAPI() {
    const std::string EGLEXTENSIONS = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);

    backend->log(AQ_LOG_DEBUG, std::format("Supported EGL client extensions: ({}) {}", std::count(EGLEXTENSIONS.begin(), EGLEXTENSIONS.end(), ' '), EGLEXTENSIONS));

    exts.KHR_display_reference = EGLEXTENSIONS.contains("KHR_display_reference");
    exts.EXT_platform_device   = EGLEXTENSIONS.contains("EXT_platform_device");
    exts.KHR_platform_gbm      = EGLEXTENSIONS.contains("KHR_platform_gbm");

    loadGLProc(&proc.eglGetPlatformDisplayEXT, "eglGetPlatformDisplayEXT");
    loadGLProc(&proc.eglCreateImageKHR, "eglCreateImageKHR");
    loadGLProc(&proc.eglDestroyImageKHR, "eglDestroyImageKHR");
    loadGLProc(&proc.eglQueryDmaBufFormatsEXT, "eglQueryDmaBufFormatsEXT");
    loadGLProc(&proc.eglQueryDmaBufModifiersEXT, "eglQueryDmaBufModifiersEXT");
    loadGLProc(&proc.glEGLImageTargetTexture2DOES, "glEGLImageTargetTexture2DOES");
    loadGLProc(&proc.glEGLImageTargetRenderbufferStorageOES, "glEGLImageTargetRenderbufferStorageOES");
    loadGLProc(&proc.eglDestroySyncKHR, "eglDestroySyncKHR");
    loadGLProc(&proc.eglWaitSyncKHR, "eglWaitSyncKHR");
    loadGLProc(&proc.eglCreateSyncKHR, "eglCreateSyncKHR");
    loadGLProc(&proc.eglDupNativeFenceFDANDROID, "eglDupNativeFenceFDANDROID");
    loadGLProc(&proc.glReadnPixelsEXT, "glReadnPixelsEXT");

    if (EGLEXTENSIONS.contains("EGL_EXT_device_base") || EGLEXTENSIONS.contains("EGL_EXT_device_enumeration"))
        loadGLProc(&proc.eglQueryDevicesEXT, "eglQueryDevicesEXT");

    if (EGLEXTENSIONS.contains("EGL_EXT_device_base") || EGLEXTENSIONS.contains("EGL_EXT_device_query"))
        loadGLProc(&proc.eglQueryDeviceStringEXT, "eglQueryDeviceStringEXT");

    if (EGLEXTENSIONS.contains("EGL_KHR_debug")) {
        loadGLProc(&proc.eglDebugMessageControlKHR, "eglDebugMessageControlKHR");
        static const EGLAttrib debugAttrs[] = {
            EGL_DEBUG_MSG_CRITICAL_KHR, EGL_TRUE, EGL_DEBUG_MSG_ERROR_KHR, EGL_TRUE, EGL_DEBUG_MSG_WARN_KHR, EGL_TRUE, EGL_DEBUG_MSG_INFO_KHR, EGL_TRUE, EGL_NONE,
        };
        proc.eglDebugMessageControlKHR(::eglLog, debugAttrs);
    }

    if (EGLEXTENSIONS.contains("EXT_platform_device")) {
        loadGLProc(&proc.eglQueryDevicesEXT, "eglQueryDevicesEXT");
        loadGLProc(&proc.eglQueryDeviceStringEXT, "eglQueryDeviceStringEXT");
    }

    RASSERT(eglBindAPI(EGL_OPENGL_ES_API) != EGL_FALSE, "Couldn't bind to EGL's opengl ES API. This means your gpu driver f'd up. This is not a Hyprland or Aquamarine issue.");
}

void CDRMRenderer::initContext() {
    RASSERT(egl.display != nullptr && egl.display != EGL_NO_DISPLAY, "CDRMRenderer: Can't create EGL context without display");

    EGLint major, minor;
    if (eglInitialize(egl.display, &major, &minor) == EGL_FALSE) {
        backend->log(AQ_LOG_ERROR, "CDRMRenderer: fail, eglInitialize failed");
        return;
    }

    std::string EGLEXTENSIONS = eglQueryString(egl.display, EGL_EXTENSIONS);

    exts.IMG_context_priority               = EGLEXTENSIONS.contains("IMG_context_priority");
    exts.EXT_create_context_robustness      = EGLEXTENSIONS.contains("EXT_create_context_robustness");
    exts.EXT_image_dma_buf_import           = EGLEXTENSIONS.contains("EXT_image_dma_buf_import");
    exts.EXT_image_dma_buf_import_modifiers = EGLEXTENSIONS.contains("EXT_image_dma_buf_import_modifiers");

    std::vector<EGLint> attrs;

    if (exts.IMG_context_priority) {
        backend->log(AQ_LOG_DEBUG, "CDRMRenderer: IMG_context_priority supported, requesting high");
        attrs.push_back(EGL_CONTEXT_PRIORITY_LEVEL_IMG);
        attrs.push_back(EGL_CONTEXT_PRIORITY_HIGH_IMG);
    }

    if (exts.EXT_create_context_robustness) {
        backend->log(AQ_LOG_DEBUG, "CDRMRenderer: EXT_create_context_robustness supported, requesting lose on reset");
        attrs.push_back(EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_EXT);
        attrs.push_back(EGL_LOSE_CONTEXT_ON_RESET_EXT);
    }

    attrs.push_back(EGL_CONTEXT_OPENGL_DEBUG);
    attrs.push_back(Aquamarine::isTrace() ? EGL_TRUE : EGL_FALSE);

    auto attrsNoVer = attrs;

    attrs.push_back(EGL_CONTEXT_MAJOR_VERSION);
    attrs.push_back(3);
    attrs.push_back(EGL_CONTEXT_MINOR_VERSION);
    attrs.push_back(2);

    attrs.push_back(EGL_NONE);

    egl.context = eglCreateContext(egl.display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs.data());
    if (egl.context == EGL_NO_CONTEXT) {
        backend->log(AQ_LOG_ERROR, "CDRMRenderer: eglCreateContext failed with GLES 3.2, retrying GLES 3.0");

        attrs = attrsNoVer;
        attrs.push_back(EGL_CONTEXT_MAJOR_VERSION);
        attrs.push_back(3);
        attrs.push_back(EGL_CONTEXT_MINOR_VERSION);
        attrs.push_back(0);

        attrs.push_back(EGL_NONE);

        egl.context = eglCreateContext(egl.display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs.data());
        if (egl.context == EGL_NO_CONTEXT) {
            backend->log(AQ_LOG_ERROR, "CDRMRenderer: Can't create renderer, eglCreateContext failed with both GLES 3.2 and GLES 3.0");
            return;
        }
    }

    if (exts.IMG_context_priority) {
        EGLint priority = EGL_CONTEXT_PRIORITY_MEDIUM_IMG;
        eglQueryContext(egl.display, egl.context, EGL_CONTEXT_PRIORITY_LEVEL_IMG, &priority);
        if (priority != EGL_CONTEXT_PRIORITY_HIGH_IMG)
            backend->log(AQ_LOG_DEBUG, "CDRMRenderer: Failed to get a high priority context");
        else
            backend->log(AQ_LOG_DEBUG, "CDRMRenderer: Got a high priority context");
    }

    CEglContextGuard eglContext(*this);

    EGLEXTENSIONS = (const char*)glGetString(GL_EXTENSIONS);

    std::string gpuName = "unknown";
    char*       drmName = drmGetDeviceNameFromFd2(drmFD);
    if (drmName != nullptr) {
        gpuName = std::string{drmName};
        free(drmName);
    }

    backend->log(AQ_LOG_DEBUG, std::format("Creating CDRMRenderer on gpu {}", gpuName));
    backend->log(AQ_LOG_DEBUG, std::format("Using: {}", (char*)glGetString(GL_VERSION)));
    backend->log(AQ_LOG_DEBUG, std::format("Vendor: {}", (char*)glGetString(GL_VENDOR)));
    backend->log(AQ_LOG_DEBUG, std::format("Renderer: {}", (char*)glGetString(GL_RENDERER)));
    backend->log(AQ_LOG_DEBUG, std::format("Supported context extensions: ({}) {}", std::count(EGLEXTENSIONS.begin(), EGLEXTENSIONS.end(), ' '), EGLEXTENSIONS));

    exts.EXT_read_format_bgra        = EGLEXTENSIONS.contains("GL_EXT_read_format_bgra");
    exts.EXT_texture_format_BGRA8888 = EGLEXTENSIONS.contains("GL_EXT_texture_format_BGRA8888");
}

void CDRMRenderer::initResources() {
    CEglContextGuard eglContext(*this);

    if (!exts.EXT_image_dma_buf_import || !initDRMFormats())
        backend->log(AQ_LOG_ERROR, "CDRMRenderer: initDRMFormats failed, dma-buf won't work");

    shader.program = createProgram(VERT_SRC, FRAG_SRC);
    if (shader.program == 0)
        backend->log(AQ_LOG_ERROR, "CDRMRenderer: texture shader failed");

    shader.proj      = glGetUniformLocation(shader.program, "proj");
    shader.posAttrib = glGetAttribLocation(shader.program, "pos");
    shader.texAttrib = glGetAttribLocation(shader.program, "texcoord");
    shader.tex       = glGetUniformLocation(shader.program, "tex");
    shader.createVao();

    shaderExt.program = createProgram(VERT_SRC, FRAG_SRC_EXT);
    if (shaderExt.program == 0)
        backend->log(AQ_LOG_ERROR, "CDRMRenderer: external texture shader failed");

    shaderExt.proj      = glGetUniformLocation(shaderExt.program, "proj");
    shaderExt.posAttrib = glGetAttribLocation(shaderExt.program, "pos");
    shaderExt.texAttrib = glGetAttribLocation(shaderExt.program, "texcoord");
    shaderExt.tex       = glGetUniformLocation(shaderExt.program, "tex");
    shaderExt.createVao();
}

SP<CDRMRenderer> CDRMRenderer::attempt(SP<CBackend> backend_, int drmFD) {
    SP<CDRMRenderer> renderer = SP<CDRMRenderer>(new CDRMRenderer());
    renderer->drmFD           = drmFD;
    renderer->backend         = backend_;
    gBackend                  = backend_;

    renderer->loadEGLAPI();

    if (!renderer->exts.EXT_platform_device) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer(drm): Can't create renderer, EGL doesn't support EXT_platform_device");
        return nullptr;
    }

    EGLDeviceEXT device = renderer->eglDeviceFromDRMFD(drmFD);
    if (device == EGL_NO_DEVICE_EXT) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer(drm): Can't create renderer, no matching devices found");
        return nullptr;
    }

    std::vector<EGLint> attrs;
    if (renderer->exts.KHR_display_reference) {
        attrs.push_back(EGL_TRACK_REFERENCES_KHR);
        attrs.push_back(EGL_TRUE);
    }

    attrs.push_back(EGL_NONE);

    renderer->egl.display = renderer->proc.eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, device, attrs.data());
    if (renderer->egl.display == EGL_NO_DISPLAY) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, eglGetPlatformDisplayEXT failed");
        return nullptr;
    }

    renderer->initContext();
    if (renderer->egl.context == nullptr || renderer->egl.context == EGL_NO_CONTEXT)
        return nullptr;

    renderer->initResources();

    return renderer;
}

SP<CDRMRenderer> CDRMRenderer::attempt(SP<CBackend> backend_, Hyprutils::Memory::CSharedPointer<CGBMAllocator> allocator_) {
    SP<CDRMRenderer> renderer = SP<CDRMRenderer>(new CDRMRenderer());
    renderer->drmFD           = allocator_->drmFD();
    renderer->backend         = backend_;
    gBackend                  = backend_;

    renderer->loadEGLAPI();

    if (!renderer->exts.KHR_platform_gbm) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer(gbm): Can't create renderer, EGL doesn't support KHR_platform_gbm");
        return nullptr;
    }

    std::vector<EGLint> attrs;
    if (renderer->exts.KHR_display_reference) {
        attrs.push_back(EGL_TRACK_REFERENCES_KHR);
        attrs.push_back(EGL_TRUE);
    }

    attrs.push_back(EGL_NONE);

    renderer->egl.display = renderer->proc.eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR, allocator_->gbmDevice, attrs.data());
    if (renderer->egl.display == EGL_NO_DISPLAY) {
        backend_->log(AQ_LOG_ERROR, "CDRMRenderer: fail, eglGetPlatformDisplayEXT failed");
        return nullptr;
    }

    renderer->initContext();
    if (renderer->egl.context == nullptr || renderer->egl.context == EGL_NO_CONTEXT)
        return nullptr;

    renderer->initResources();

    return renderer;
}

CEglContextGuard::CEglContextGuard(const CDRMRenderer& renderer_) : renderer(renderer_) {
    savedEGLState.display = eglGetCurrentDisplay();
    savedEGLState.context = eglGetCurrentContext();
    savedEGLState.draw    = eglGetCurrentSurface(EGL_DRAW);
    savedEGLState.read    = eglGetCurrentSurface(EGL_READ);

    if (!eglMakeCurrent(renderer.egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, renderer.egl.context))
        renderer.backend->log(AQ_LOG_WARNING, "CDRMRenderer: setEGL eglMakeCurrent failed");
}

CEglContextGuard::~CEglContextGuard() {
    EGLDisplay dpy = savedEGLState.display ? savedEGLState.display : renderer.egl.display;

    // egl can't handle this
    if (dpy == EGL_NO_DISPLAY)
        return;

    if (!eglMakeCurrent(dpy, savedEGLState.draw, savedEGLState.read, savedEGLState.context))
        renderer.backend->log(AQ_LOG_WARNING, "CDRMRenderer: restoreEGL eglMakeCurrent failed");
}

EGLImageKHR CDRMRenderer::createEGLImage(const SDMABUFAttrs& attrs) {
    std::array<EGLint, 50> attribs;
    size_t                 idx = 0;

    attribs[idx++] = EGL_WIDTH;
    attribs[idx++] = attrs.size.x;
    attribs[idx++] = EGL_HEIGHT;
    attribs[idx++] = attrs.size.y;
    attribs[idx++] = EGL_LINUX_DRM_FOURCC_EXT;
    attribs[idx++] = attrs.format;

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
        attribs[idx++] = attrNames[i].fd;
        attribs[idx++] = attrs.fds[i];
        attribs[idx++] = attrNames[i].offset;
        attribs[idx++] = attrs.offsets[i];
        attribs[idx++] = attrNames[i].pitch;
        attribs[idx++] = attrs.strides[i];
        if (hasModifiers && attrs.modifier != DRM_FORMAT_MOD_INVALID) {
            attribs[idx++] = attrNames[i].modlo;
            attribs[idx++] = attrs.modifier & 0xFFFFFFFF;
            attribs[idx++] = attrNames[i].modhi;
            attribs[idx++] = attrs.modifier >> 32;
        }
    }

    attribs[idx++] = EGL_IMAGE_PRESERVED_KHR;
    attribs[idx++] = EGL_TRUE;

    attribs[idx++] = EGL_NONE;
    RASSERT(idx <= attribs.size(), "EGL: EGLCreateImageKHR: attribs array out of bounds.");

    EGLImageKHR image = proc.eglCreateImageKHR(egl.display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs.data());
    if (image == EGL_NO_IMAGE_KHR) {
        backend->log(AQ_LOG_ERROR, std::format("EGL: EGLCreateImageKHR failed: {}", eglGetError()));
        return EGL_NO_IMAGE_KHR;
    }

    return image;
}

CGLTex CDRMRenderer::glTex(Hyprutils::Memory::CSharedPointer<IBuffer> buffa) {
    CGLTex      tex;

    const auto& dma = buffa->dmabuf();

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

    GLCALL(tex.bind());
    GLCALL(tex.setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE));
    GLCALL(tex.setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE));
    GLCALL(proc.glEGLImageTargetTexture2DOES(tex.target, tex.image));
    GLCALL(tex.unbind());

    return tex;
}

constexpr GLenum PIXEL_BUFFER_FORMAT = GL_RGBA;

void             CDRMRenderer::readBuffer(Hyprutils::Memory::CSharedPointer<IBuffer> buf, std::span<uint8_t> out) {
    CEglContextGuard eglContext(*this);
    auto             att = buf->attachments.get<CDRMRendererBufferAttachment>();
    if (!att) {
        att = makeShared<CDRMRendererBufferAttachment>(self, buf, nullptr, 0, 0, CGLTex{}, std::vector<uint8_t>());
        buf->attachments.add(att);
    }

    const auto& dma = buf->dmabuf();
    if (!att->eglImage) {
        att->eglImage = createEGLImage(dma);
        if (att->eglImage == EGL_NO_IMAGE_KHR) {
            backend->log(AQ_LOG_ERROR, std::format("EGL (readBuffer): createEGLImage failed: {}", eglGetError()));
            return;
        }

        GLCALL(glGenRenderbuffers(1, &att->rbo));
        GLCALL(glBindRenderbuffer(GL_RENDERBUFFER, att->rbo));
        GLCALL(proc.glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, (GLeglImageOES)att->eglImage));
        GLCALL(glBindRenderbuffer(GL_RENDERBUFFER, 0));

        GLCALL(glGenFramebuffers(1, &att->fbo));
        GLCALL(glBindFramebuffer(GL_FRAMEBUFFER, att->fbo));
        GLCALL(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, att->rbo));

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            backend->log(AQ_LOG_ERROR, std::format("EGL (readBuffer): glCheckFramebufferStatus failed: {}", glGetError()));
            return;
        }
    }

    GLCALL(glBindFramebuffer(GL_FRAMEBUFFER, att->fbo));
    GLCALL(proc.glReadnPixelsEXT(0, 0, dma.size.x, dma.size.y, GL_RGBA, GL_UNSIGNED_BYTE, out.size(), out.data()));

    GLCALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
}

void CDRMRenderer::waitOnSync(int fd) {
    TRACE(backend->log(AQ_LOG_TRACE, std::format("EGL (waitOnSync): attempting to wait on fd {}", fd)));

    std::array<EGLint, 3> attribs;
    int                   dupFd = fcntl(fd, F_DUPFD_CLOEXEC, 0);
    if (dupFd < 0) {
        backend->log(AQ_LOG_TRACE, "EGL (waitOnSync): failed to dup fd for wait");
        return;
    }

    attribs[0] = EGL_SYNC_NATIVE_FENCE_FD_ANDROID;
    attribs[1] = dupFd;
    attribs[2] = EGL_NONE;

    EGLSyncKHR sync = proc.eglCreateSyncKHR(egl.display, EGL_SYNC_NATIVE_FENCE_ANDROID, attribs.data());
    if (sync == EGL_NO_SYNC_KHR) {
        TRACE(backend->log(AQ_LOG_TRACE, "EGL (waitOnSync): failed to create an egl sync for explicit"));
        if (dupFd >= 0)
            close(dupFd);
        return;
    }

    // we got a sync, now we just tell egl to wait before sampling
    if (proc.eglWaitSyncKHR(egl.display, sync, 0) != EGL_TRUE) {
        if (proc.eglDestroySyncKHR(egl.display, sync) != EGL_TRUE)
            TRACE(backend->log(AQ_LOG_TRACE, "EGL (waitOnSync): failed to destroy sync"));

        TRACE(backend->log(AQ_LOG_TRACE, "EGL (waitOnSync): failed to wait on the sync object"));
        return;
    }

    if (proc.eglDestroySyncKHR(egl.display, sync) != EGL_TRUE)
        TRACE(backend->log(AQ_LOG_TRACE, "EGL (waitOnSync): failed to destroy sync"));
}

int CDRMRenderer::recreateBlitSync() {
    TRACE(backend->log(AQ_LOG_TRACE, "EGL (recreateBlitSync): recreating blit sync"));

    if (egl.lastBlitSync) {
        TRACE(backend->log(AQ_LOG_TRACE, std::format("EGL (recreateBlitSync): cleaning up old sync (fd {})", egl.lastBlitSyncFD)));

        // cleanup last sync
        if (proc.eglDestroySyncKHR(egl.display, egl.lastBlitSync) != EGL_TRUE)
            TRACE(backend->log(AQ_LOG_TRACE, "EGL (recreateBlitSync): failed to destroy old sync"));

        if (egl.lastBlitSyncFD >= 0)
            close(egl.lastBlitSyncFD);

        egl.lastBlitSyncFD = -1;
        egl.lastBlitSync   = nullptr;
    }

    EGLSyncKHR sync = proc.eglCreateSyncKHR(egl.display, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
    if (sync == EGL_NO_SYNC_KHR) {
        TRACE(backend->log(AQ_LOG_TRACE, "EGL (recreateBlitSync): failed to create an egl sync for explicit"));
        return -1;
    }

    // we need to flush otherwise we might not get a valid fd
    glFlush();

    int fd = proc.eglDupNativeFenceFDANDROID(egl.display, sync);
    if (fd == EGL_NO_NATIVE_FENCE_FD_ANDROID) {
        TRACE(backend->log(AQ_LOG_TRACE, "EGL (recreateBlitSync): failed to dup egl fence fd"));
        if (proc.eglDestroySyncKHR(egl.display, sync) != EGL_TRUE)
            TRACE(backend->log(AQ_LOG_TRACE, "EGL (recreateBlitSync): failed to destroy new sync"));
        return -1;
    }

    egl.lastBlitSync   = sync;
    egl.lastBlitSyncFD = fd;

    TRACE(backend->log(AQ_LOG_TRACE, std::format("EGL (recreateBlitSync): success, new fence exported with fd {}", fd)));

    return fd;
}

void CDRMRenderer::clearBuffer(IBuffer* buf) {
    CEglContextGuard eglContext(*this);
    const auto&      dmabuf = buf->dmabuf();
    GLuint           rboID = 0, fboID = 0;

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
    GLCALL(proc.glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, (GLeglImageOES)rboImage));
    GLCALL(glBindRenderbuffer(GL_RENDERBUFFER, 0));

    GLCALL(glGenFramebuffers(1, &fboID));
    GLCALL(glBindFramebuffer(GL_FRAMEBUFFER, fboID));
    GLCALL(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rboID));

    GLCALL(glBindRenderbuffer(GL_RENDERBUFFER, rboID));
    GLCALL(glBindFramebuffer(GL_FRAMEBUFFER, fboID));

    TRACE(backend->log(AQ_LOG_TRACE, std::format("EGL (clear): fbo {} rbo {}", fboID, rboID)));

    glClearColor(0.F, 0.F, 0.F, 1.F);
    glClear(GL_COLOR_BUFFER_BIT);

    GLCALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    GLCALL(glBindRenderbuffer(GL_RENDERBUFFER, 0));

    glDeleteFramebuffers(1, &fboID);
    glDeleteRenderbuffers(1, &rboID);
    proc.eglDestroyImageKHR(egl.display, rboImage);
}

CDRMRenderer::SBlitResult CDRMRenderer::blit(SP<IBuffer> from, SP<IBuffer> to, SP<CDRMRenderer> primaryRenderer, int waitFD) {
    CEglContextGuard eglContext(*this);

    if (from->dmabuf().size != to->dmabuf().size) {
        backend->log(AQ_LOG_ERROR, "EGL (blit): buffer sizes mismatched");
        return {};
    }

    if (waitFD >= 0 && !CFileDescriptor::isReadable(waitFD)) {
        // wait on a provided explicit fence
        waitOnSync(waitFD);
    }

    // firstly, get a texture from the from buffer
    // if it has an attachment, use that
    // both from and to have the same AQ_ATTACHMENT_DRM_RENDERER_DATA.
    // Those buffers always come from different swapchains, so it's OK.

    WP<CGLTex>         fromTex;
    const auto&        fromDma = from->dmabuf();
    std::span<uint8_t> intermediateBuf;
    {
        auto attachment = from->attachments.get<CDRMRendererBufferAttachment>();
        if (attachment) {
            TRACE(backend->log(AQ_LOG_TRACE, "EGL (blit): From attachment found"));
            fromTex         = attachment->tex;
            intermediateBuf = attachment->intermediateBuf;
        }

        if ((!fromTex || !fromTex->image) && intermediateBuf.empty()) {
            backend->log(AQ_LOG_DEBUG, "EGL (blit): No attachment in from, creating a new image");

            attachment = makeShared<CDRMRendererBufferAttachment>(self, from, nullptr, 0, 0, glTex(from), std::vector<uint8_t>());
            from->attachments.add(attachment);

            if (!attachment->tex->image && primaryRenderer) {
                backend->log(AQ_LOG_DEBUG, "EGL (blit): Failed to create image from source buffer directly, allocating intermediate buffer");
                static_assert(PIXEL_BUFFER_FORMAT == GL_RGBA); // If the pixel buffer format changes, the below size calculation probably needs to as well.
                attachment->intermediateBuf.resize(fromDma.size.x * fromDma.size.y * 4);
                intermediateBuf         = attachment->intermediateBuf;
                attachment->tex->target = GL_TEXTURE_2D;
                GLCALL(glGenTextures(1, &attachment->tex->texid));
            }

            fromTex = attachment->tex;
        }

        if (!intermediateBuf.empty() && primaryRenderer) {
            // Note: this might modify from's attachments
            primaryRenderer->readBuffer(from, intermediateBuf);
        }
    }

    TRACE(backend->log(AQ_LOG_TRACE,
                       std::format("EGL (blit): fromTex id {}, image 0x{:x}, target {}", fromTex->texid, (uintptr_t)fromTex->image,
                                   fromTex->target == GL_TEXTURE_2D ? "GL_TEXTURE_2D" : "GL_TEXTURE_EXTERNAL_OES")));

    // then, get a rbo from our to buffer
    // if it has an attachment, use that

    EGLImageKHR rboImage = nullptr;
    GLuint      rboID = 0, fboID = 0;
    const auto& toDma = to->dmabuf();

    if (!verifyDestinationDMABUF(toDma)) {
        backend->log(AQ_LOG_ERROR, "EGL (blit): failed to blit: destination dmabuf unsupported");
        return {};
    }

    {
        auto attachment = to->attachments.get<CDRMRendererBufferAttachment>();
        if (attachment) {
            TRACE(backend->log(AQ_LOG_TRACE, "EGL (blit): To attachment found"));
            rboImage = attachment->eglImage;
            fboID    = attachment->fbo;
            rboID    = attachment->rbo;
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
            GLCALL(proc.glEGLImageTargetRenderbufferStorageOES(GL_RENDERBUFFER, (GLeglImageOES)rboImage));
            GLCALL(glBindRenderbuffer(GL_RENDERBUFFER, 0));

            GLCALL(glGenFramebuffers(1, &fboID));
            GLCALL(glBindFramebuffer(GL_FRAMEBUFFER, fboID));
            GLCALL(glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rboID));

            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                backend->log(AQ_LOG_ERROR, std::format("EGL (blit): glCheckFramebufferStatus failed: {}", glGetError()));
                return {};
            }

            to->attachments.add(makeShared<CDRMRendererBufferAttachment>(self, to, rboImage, fboID, rboID, CGLTex{}, std::vector<uint8_t>()));
        }
    }

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

    auto& SHADER = fromTex->target == GL_TEXTURE_2D ? shader : shaderExt;

    // KMS uses flipped y, we have to do FLIPPED_180
    matrixTranslate(base, toDma.size.x / 2.0, toDma.size.y / 2.0);
    matrixTransform(base, HYPRUTILS_TRANSFORM_FLIPPED_180);
    matrixTranslate(base, -toDma.size.x / 2.0, -toDma.size.y / 2.0);

    projectBox(mtx, renderBox, HYPRUTILS_TRANSFORM_FLIPPED_180, 0, base);

    matrixProjection(monitorProj, toDma.size.x, toDma.size.y, HYPRUTILS_TRANSFORM_FLIPPED_180);

    float glMtx[9];
    matrixMultiply(glMtx, monitorProj, mtx);

    static Vector2D lastViewportSize = {-1, -1};
    if (lastViewportSize != toDma.size) {
        GLCALL(glViewport(0, 0, toDma.size.x, toDma.size.y));
        lastViewportSize = toDma.size;
    }

    GLCALL(glActiveTexture(GL_TEXTURE0));
    GLCALL(fromTex->bind());
    GLCALL(fromTex->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_NEAREST));
    GLCALL(fromTex->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_NEAREST));

    if (!intermediateBuf.empty())
        GLCALL(glTexImage2D(fromTex->target, 0, PIXEL_BUFFER_FORMAT, fromDma.size.x, fromDma.size.y, 0, PIXEL_BUFFER_FORMAT, GL_UNSIGNED_BYTE, intermediateBuf.data()));

    useProgram(SHADER.program);
    GLCALL(glDisable(GL_BLEND));
    GLCALL(glDisable(GL_SCISSOR_TEST));

    matrixTranspose(glMtx, glMtx);
    GLCALL(glUniformMatrix3fv(SHADER.proj, 1, GL_FALSE, glMtx));

    GLCALL(glUniform1i(SHADER.tex, 0));
    GLCALL(glBindVertexArray(SHADER.shaderVao));

    GLCALL(glDrawArrays(GL_TRIANGLE_STRIP, 0, 4));

    GLCALL(glBindVertexArray(0));
    GLCALL(fromTex->unbind());

    // get an explicit sync fd for the secondary gpu.
    // when we pass buffers between gpus we should always use explicit sync,
    // as implicit is not guaranteed at all
    int explicitFD = recreateBlitSync();

    GLCALL(glBindFramebuffer(GL_FRAMEBUFFER, 0));
    GLCALL(glBindRenderbuffer(GL_RENDERBUFFER, 0));

    return {.success = true, .syncFD = explicitFD == -1 ? std::nullopt : std::optional<int>{explicitFD}};
}

void CDRMRenderer::onBufferAttachmentDrop(CDRMRendererBufferAttachment* attachment) {
    CEglContextGuard eglContext(*this);

    TRACE(backend->log(AQ_LOG_TRACE,
                       std::format("EGL (onBufferAttachmentDrop): dropping fbo {} rbo {} image 0x{:x}", attachment->fbo, attachment->rbo, (uintptr_t)attachment->eglImage)));

    if (attachment->tex && attachment->tex->texid)
        GLCALL(glDeleteTextures(1, &attachment->tex->texid));
    if (attachment->rbo)
        GLCALL(glDeleteRenderbuffers(1, &attachment->rbo));
    if (attachment->fbo)
        GLCALL(glDeleteFramebuffers(1, &attachment->fbo));
    if (attachment->eglImage)
        proc.eglDestroyImageKHR(egl.display, attachment->eglImage);
    if (attachment->tex && attachment->tex->image)
        proc.eglDestroyImageKHR(egl.display, attachment->tex->image);
}

bool CDRMRenderer::verifyDestinationDMABUF(const SDMABUFAttrs& attrs) {
    for (auto const& fmt : formats) {
        if (fmt.drmFormat != attrs.format)
            continue;

        if (fmt.modifier != attrs.modifier)
            continue;

        if (fmt.modifier != DRM_FORMAT_INVALID && fmt.external) {
            backend->log(AQ_LOG_ERROR, "EGL (verifyDestinationDMABUF): FAIL, format is external-only");
            return false;
        }

        return true;
    }

    backend->log(AQ_LOG_ERROR, "EGL (verifyDestinationDMABUF): FAIL, format is unsupported by EGL");
    return false;
}

constexpr std::optional<size_t> CGLTex::getCacheStateIndex(GLenum pname) {
    switch (pname) {
        case GL_TEXTURE_WRAP_S: return TEXTURE_PAR_WRAP_S;
        case GL_TEXTURE_WRAP_T: return TEXTURE_PAR_WRAP_T;
        case GL_TEXTURE_MAG_FILTER: return TEXTURE_PAR_MAG_FILTER;
        case GL_TEXTURE_MIN_FILTER: return TEXTURE_PAR_MIN_FILTER;
        default: return std::nullopt;
    }
}

void CGLTex::bind() {
    glBindTexture(target, texid);
}

void CGLTex::unbind() {
    glBindTexture(target, 0);
}

void CGLTex::setTexParameter(GLenum pname, GLint param) {
    const auto cacheIndex = getCacheStateIndex(pname);

    if (!cacheIndex) {
        glTexParameteri(target, pname, param);
        return;
    }

    const auto idx = cacheIndex.value();

    if (m_cachedStates[idx] == param)
        return;

    m_cachedStates[idx] = param;
    glTexParameteri(target, pname, param);
}

CDRMRendererBufferAttachment::CDRMRendererBufferAttachment(Hyprutils::Memory::CWeakPointer<CDRMRenderer> renderer_, Hyprutils::Memory::CSharedPointer<IBuffer> buffer,
                                                           EGLImageKHR image, GLuint fbo_, GLuint rbo_, CGLTex&& tex_, std::vector<uint8_t> intermediateBuf_) :
    eglImage(image), fbo(fbo_), rbo(rbo_), tex(makeUnique<CGLTex>(std::move(tex_))), intermediateBuf(intermediateBuf_), renderer(renderer_) {
    bufferDestroy = buffer->events.destroy.listen([this] { renderer->onBufferAttachmentDrop(this); });
}
