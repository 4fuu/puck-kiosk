#include "platform/headless/headless_display.h"

#include "core/log.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace pk {
namespace {

const char* egl_error_string(EGLint error) {
    switch (error) {
        case EGL_SUCCESS: return "EGL_SUCCESS";
        case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
        case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
        case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
        case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
        case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
        case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
        case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
        case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
        case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
        case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
        case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
        case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
        case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
        case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
        default: return "EGL error";
    }
}

bool set_error(std::string* error, const std::string& message) {
    if (error) {
        *error = message;
    }
    return false;
}

}  // namespace

HeadlessDisplay::HeadlessDisplay(int width, int height, std::string output_path, int frames)
    : size_{width, height}, output_path_(std::move(output_path)), frames_(frames < 1 ? 1 : frames) {}

HeadlessDisplay::~HeadlessDisplay() {
    destroy();
}

bool HeadlessDisplay::initialize(std::string* error) {
    auto get_platform_display =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (get_platform_display) {
        egl_display_ = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    }
    if (!egl_display_) {
        egl_display_ = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    }
    if (egl_display_ == EGL_NO_DISPLAY) {
        return set_error(error, "EGL: no display (surfaceless and default both failed)");
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (!eglInitialize(static_cast<EGLDisplay>(egl_display_), &major, &minor)) {
        return set_error(error, std::string("eglInitialize failed: ") + egl_error_string(eglGetError()));
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        return set_error(error, "eglBindAPI(EGL_OPENGL_ES_API) failed");
    }

    const EGLint config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE,
    };
    EGLConfig config = nullptr;
    EGLint config_count = 0;
    if (!eglChooseConfig(static_cast<EGLDisplay>(egl_display_), config_attributes, &config, 1, &config_count) ||
        config_count == 0) {
        return set_error(error, "EGL: no EGL_OPENGL_ES3_BIT config");
    }
    egl_config_ = config;

    const EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE,
    };
    egl_context_ = eglCreateContext(static_cast<EGLDisplay>(egl_display_), config, EGL_NO_CONTEXT,
                                    context_attributes);
    if (egl_context_ == EGL_NO_CONTEXT) {
        return set_error(error, std::string("eglCreateContext failed: ") + egl_error_string(eglGetError()));
    }

    if (!eglMakeCurrent(static_cast<EGLDisplay>(egl_display_), EGL_NO_SURFACE, EGL_NO_SURFACE,
                        static_cast<EGLContext>(egl_context_))) {
        return set_error(error, std::string("eglMakeCurrent failed: ") + egl_error_string(eglGetError()));
    }

    glGenTextures(1, &color_texture_);
    glBindTexture(GL_TEXTURE_2D, color_texture_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, size_.width, size_.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenRenderbuffers(1, &stencil_buffer_);
    glBindRenderbuffer(GL_RENDERBUFFER, stencil_buffer_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, size_.width, size_.height);

    glGenFramebuffers(1, &framebuffer_);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_texture_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencil_buffer_);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        return set_error(error, "headless framebuffer is incomplete");
    }

    log::info("headless EGL %d.%d, GL %s, renderer %s", major, minor,
              reinterpret_cast<const char*>(glGetString(GL_VERSION)),
              reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    return true;
}

void HeadlessDisplay::make_current() {
    eglMakeCurrent(static_cast<EGLDisplay>(egl_display_), EGL_NO_SURFACE, EGL_NO_SURFACE,
                   static_cast<EGLContext>(egl_context_));
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glViewport(0, 0, size_.width, size_.height);
}

void HeadlessDisplay::swap() {
    glFinish();
    if (frame_ + 1 >= frames_ && !output_path_.empty()) {
        std::string error;
        if (!write_ppm(output_path_, &error)) {
            log::error("%s", error.c_str());
        } else {
            log::info("wrote %s (%dx%d)", output_path_.c_str(), size_.width, size_.height);
        }
    }
    ++frame_;
}

bool HeadlessDisplay::write_ppm(const std::string& path, std::string* error) const {
    if (path.empty()) {
        return set_error(error, "no output path for the headless frame");
    }
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size_.width) * size_.height * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_);
    glReadPixels(0, 0, size_.width, size_.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    std::ofstream output(path, std::ios::binary);
    if (!output) {
        return set_error(error, "cannot open " + path);
    }
    output << "P6\n" << size_.width << " " << size_.height << "\n255\n";
    for (int y = size_.height - 1; y >= 0; --y) {
        const auto* row = pixels.data() + static_cast<std::size_t>(y) * size_.width * 4;
        for (int x = 0; x < size_.width; ++x) {
            output.put(static_cast<char>(row[x * 4 + 0]));
            output.put(static_cast<char>(row[x * 4 + 1]));
            output.put(static_cast<char>(row[x * 4 + 2]));
        }
    }
    return true;
}

void HeadlessDisplay::destroy() {
    if (egl_display_ == EGL_NO_DISPLAY) {
        return;
    }
    auto display = static_cast<EGLDisplay>(egl_display_);
    if (framebuffer_) {
        glDeleteFramebuffers(1, &framebuffer_);
        framebuffer_ = 0;
    }
    if (stencil_buffer_) {
        glDeleteRenderbuffers(1, &stencil_buffer_);
        stencil_buffer_ = 0;
    }
    if (color_texture_) {
        glDeleteTextures(1, &color_texture_);
        color_texture_ = 0;
    }
    if (egl_context_ != EGL_NO_CONTEXT) {
        eglDestroyContext(display, static_cast<EGLContext>(egl_context_));
        egl_context_ = nullptr;
    }
    eglTerminate(display);
    egl_display_ = nullptr;
}

}  // namespace pk
