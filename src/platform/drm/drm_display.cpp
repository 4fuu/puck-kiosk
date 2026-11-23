#include "platform/drm/drm_display.h"

#include "core/log.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace pk {
namespace {

volatile sig_atomic_t g_quit_requested = 0;

void request_quit(int) {
    g_quit_requested = 1;
}

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

std::string system_error(int value) {
    return std::strerror(value);
}

std::string access_hint(int value) {
    if (value == EACCES || value == EPERM) {
        return " (add the user to the 'video' group or run as root)";
    }
    if (value == EBUSY) {
        return " (another process holds the device)";
    }
    return {};
}

const char* connector_type_name(std::uint32_t type) {
    switch (type) {
        case DRM_MODE_CONNECTOR_HDMIA: return "HDMI-A";
        case DRM_MODE_CONNECTOR_HDMIB: return "HDMI-B";
        case DRM_MODE_CONNECTOR_DisplayPort: return "DP";
        case DRM_MODE_CONNECTOR_eDP: return "eDP";
        case DRM_MODE_CONNECTOR_LVDS: return "LVDS";
        case DRM_MODE_CONNECTOR_DSI: return "DSI";
        case DRM_MODE_CONNECTOR_VGA: return "VGA";
        case DRM_MODE_CONNECTOR_DVID: return "DVI-D";
        case DRM_MODE_CONNECTOR_DVIA: return "DVI-A";
        default: return "connector";
    }
}

bool find_property(int fd, std::uint32_t object_id, std::uint32_t object_type, const char* name,
                   std::uint32_t* id, std::uint64_t* value) {
    drmModeObjectPropertiesPtr properties = drmModeObjectGetProperties(fd, object_id, object_type);
    if (!properties) {
        return false;
    }
    bool found = false;
    for (std::uint32_t i = 0; i < properties->count_props && !found; ++i) {
        drmModePropertyPtr property = drmModeGetProperty(fd, properties->props[i]);
        if (!property) {
            continue;
        }
        if (std::strcmp(property->name, name) == 0) {
            if (id) {
                *id = property->prop_id;
            }
            if (value) {
                *value = properties->prop_values[i];
            }
            found = true;
        }
        drmModeFreeProperty(property);
    }
    drmModeFreeObjectProperties(properties);
    return found;
}

std::uint32_t property_id(int fd, std::uint32_t object_id, std::uint32_t object_type, const char* name) {
    std::uint32_t id = 0;
    find_property(fd, object_id, object_type, name, &id, nullptr);
    return id;
}

bool find_crtc(int fd, const drmModeRes* resources, const drmModeConnector* connector,
               std::uint32_t* crtc_id, std::uint32_t* crtc_index) {
    drmModeEncoderPtr encoder = nullptr;
    if (connector->encoder_id) {
        encoder = drmModeGetEncoder(fd, connector->encoder_id);
    }
    for (int i = 0; i < connector->count_encoders && !encoder; ++i) {
        encoder = drmModeGetEncoder(fd, connector->encoders[i]);
    }
    if (!encoder) {
        return false;
    }

    std::uint32_t id = 0;
    std::uint32_t index = 0;
    if (encoder->crtc_id) {
        for (int i = 0; i < resources->count_crtcs; ++i) {
            if (resources->crtcs[i] == encoder->crtc_id) {
                id = encoder->crtc_id;
                index = static_cast<std::uint32_t>(i);
                break;
            }
        }
    }
    for (int i = 0; i < resources->count_crtcs && !id; ++i) {
        if (encoder->possible_crtcs & (1u << i)) {
            id = resources->crtcs[i];
            index = static_cast<std::uint32_t>(i);
        }
    }
    drmModeFreeEncoder(encoder);
    if (!id) {
        return false;
    }
    *crtc_id = id;
    *crtc_index = index;
    return true;
}

const drmModeModeInfo* choose_mode(const drmModeConnector* connector) {
    const drmModeModeInfo* best = nullptr;
    for (int i = 0; i < connector->count_modes; ++i) {
        const drmModeModeInfo& mode = connector->modes[i];
        if (mode.type & DRM_MODE_TYPE_PREFERRED) {
            return &mode;
        }
        const std::uint32_t area = static_cast<std::uint32_t>(mode.hdisplay) * mode.vdisplay;
        const std::uint32_t best_area =
            best ? static_cast<std::uint32_t>(best->hdisplay) * best->vdisplay : 0;
        if (!best || area > best_area || (area == best_area && mode.vrefresh > best->vrefresh)) {
            best = &mode;
        }
    }
    return best;
}

}  // namespace

struct DrmDisplay::Impl {
    explicit Impl(std::string path) : requested_path(std::move(path)) {}

    std::string requested_path;
    std::string device_path;
    std::string connector_label;
    int fd = -1;
    bool master = false;
    bool atomic = false;
    bool modeset_done = false;

    std::uint32_t connector_id = 0;
    std::uint32_t crtc_id = 0;
    std::uint32_t crtc_index = 0;
    std::uint32_t plane_id = 0;
    drmModeModeInfo mode{};
    drmModeCrtcPtr saved_crtc = nullptr;
    std::uint32_t mode_blob = 0;

    std::uint32_t connector_crtc_property = 0;
    std::uint32_t crtc_mode_property = 0;
    std::uint32_t crtc_active_property = 0;
    std::uint32_t plane_fb_property = 0;
    std::uint32_t plane_crtc_property = 0;
    std::uint32_t plane_src_x_property = 0;
    std::uint32_t plane_src_y_property = 0;
    std::uint32_t plane_src_w_property = 0;
    std::uint32_t plane_src_h_property = 0;
    std::uint32_t plane_crtc_x_property = 0;
    std::uint32_t plane_crtc_y_property = 0;
    std::uint32_t plane_crtc_w_property = 0;
    std::uint32_t plane_crtc_h_property = 0;

    gbm_device* gbm = nullptr;
    gbm_surface* surface = nullptr;

    EGLDisplay egl_display = EGL_NO_DISPLAY;
    EGLContext egl_context = EGL_NO_CONTEXT;
    EGLSurface egl_surface = EGL_NO_SURFACE;

    struct Framebuffer {
        gbm_bo* bo = nullptr;
        std::uint32_t id = 0;
    };
    std::vector<Framebuffer> framebuffers;
    gbm_bo* scanout_bo = nullptr;

    bool flip_done = false;
    bool flip_pending = false;

    bool open_device(std::string* error);
    bool probe(int candidate_fd, std::string* why);
    bool setup_atomic();
    bool setup_gbm(std::string* error);
    bool setup_egl(std::string* error);
    std::uint32_t framebuffer_for(gbm_bo* bo, std::string* error);
    bool modeset(std::uint32_t framebuffer);
    bool flip(std::uint32_t framebuffer);
    bool wait_for_flip();
    void restore_crtc();
    void destroy();

    static void handle_page_flip(int, unsigned int, unsigned int, unsigned int, void* data) {
        static_cast<Impl*>(data)->flip_done = true;
    }
};

bool DrmDisplay::Impl::open_device(std::string* error) {
    std::vector<std::string> candidates;
    if (!requested_path.empty()) {
        candidates.push_back(requested_path);
    } else {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator("/dev/dri", ec)) {
            const std::string name = entry.path().filename().string();
            if (name.rfind("card", 0) == 0) {
                candidates.push_back(entry.path().string());
            }
        }
        std::sort(candidates.begin(), candidates.end());
    }
    if (candidates.empty()) {
        return set_error(error, "DRM/KMS: no /dev/dri/card* device found; is a KMS driver loaded?");
    }

    std::string first_failure;
    for (const std::string& path : candidates) {
        const int candidate_fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (candidate_fd < 0) {
            const int value = errno;
            if (first_failure.empty()) {
                first_failure =
                    "cannot open " + path + ": " + system_error(value) + access_hint(value);
            }
            continue;
        }
        std::string why;
        if (!probe(candidate_fd, &why)) {
            if (first_failure.empty()) {
                first_failure = path + ": " + why;
            }
            close(candidate_fd);
            continue;
        }
        fd = candidate_fd;
        device_path = path;
        return true;
    }
    return set_error(error, "DRM/KMS: no usable KMS device (" + first_failure + ")");
}

bool DrmDisplay::Impl::probe(int candidate_fd, std::string* why) {
    drmModeResPtr resources = drmModeGetResources(candidate_fd);
    if (!resources) {
        *why = "not a KMS device";
        return false;
    }

    std::uint32_t chosen_connector = 0;
    std::uint32_t chosen_crtc = 0;
    std::uint32_t chosen_index = 0;
    std::string chosen_label;
    drmModeModeInfo chosen_mode{};
    bool connected_without_crtc = false;

    for (int i = 0; i < resources->count_connectors && !chosen_connector; ++i) {
        drmModeConnectorPtr connector = drmModeGetConnector(candidate_fd, resources->connectors[i]);
        if (!connector) {
            continue;
        }
        if (connector->connection == DRM_MODE_CONNECTED) {
            const drmModeModeInfo* mode =
                connector->count_modes > 0 ? choose_mode(connector) : nullptr;
            std::uint32_t crtc = 0;
            std::uint32_t index = 0;
            if (mode && find_crtc(candidate_fd, resources, connector, &crtc, &index)) {
                chosen_connector = connector->connector_id;
                chosen_crtc = crtc;
                chosen_index = index;
                chosen_mode = *mode;
                chosen_label = std::string(connector_type_name(connector->connector_type)) + "-" +
                               std::to_string(connector->connector_type_id);
            } else {
                connected_without_crtc = true;
            }
        }
        drmModeFreeConnector(connector);
    }
    drmModeFreeResources(resources);

    if (!chosen_connector) {
        *why = connected_without_crtc ? "connected connector has no mode or CRTC"
                                      : "no connected connector";
        return false;
    }
    connector_id = chosen_connector;
    crtc_id = chosen_crtc;
    crtc_index = chosen_index;
    connector_label = chosen_label;
    mode = chosen_mode;
    return true;
}

bool DrmDisplay::Impl::setup_atomic() {
    if (drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) != 0) {
        return false;
    }
    drmModePlaneResPtr planes = drmModeGetPlaneResources(fd);
    if (planes) {
        for (std::uint32_t i = 0; i < planes->count_planes && !plane_id; ++i) {
            drmModePlanePtr candidate = drmModeGetPlane(fd, planes->planes[i]);
            if (!candidate) {
                continue;
            }
            std::uint64_t type = 0;
            if ((candidate->possible_crtcs & (1u << crtc_index)) &&
                find_property(fd, candidate->plane_id, DRM_MODE_OBJECT_PLANE, "type", nullptr, &type) &&
                type == DRM_PLANE_TYPE_PRIMARY) {
                plane_id = candidate->plane_id;
            }
            drmModeFreePlane(candidate);
        }
        drmModeFreePlaneResources(planes);
    }
    if (!plane_id || drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0) {
        return false;
    }

    connector_crtc_property = property_id(fd, connector_id, DRM_MODE_OBJECT_CONNECTOR, "CRTC_ID");
    crtc_mode_property = property_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "MODE_ID");
    crtc_active_property = property_id(fd, crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE");
    plane_fb_property = property_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "FB_ID");
    plane_crtc_property = property_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_ID");
    plane_src_x_property = property_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_X");
    plane_src_y_property = property_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_Y");
    plane_src_w_property = property_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_W");
    plane_src_h_property = property_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "SRC_H");
    plane_crtc_x_property = property_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_X");
    plane_crtc_y_property = property_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_Y");
    plane_crtc_w_property = property_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_W");
    plane_crtc_h_property = property_id(fd, plane_id, DRM_MODE_OBJECT_PLANE, "CRTC_H");

    const bool complete = connector_crtc_property && crtc_mode_property && crtc_active_property &&
                          plane_fb_property && plane_crtc_property && plane_src_x_property &&
                          plane_src_y_property && plane_src_w_property && plane_src_h_property &&
                          plane_crtc_x_property && plane_crtc_y_property && plane_crtc_w_property &&
                          plane_crtc_h_property;
    if (!complete) {
        log::warn("drm: primary plane %u lacks atomic properties; using legacy page flips", plane_id);
        plane_id = 0;
        return false;
    }
    if (drmModeCreatePropertyBlob(fd, &mode, sizeof(mode), &mode_blob) != 0) {
        log::warn("drm: cannot create the mode property blob (%s); using legacy page flips",
                  system_error(errno).c_str());
        plane_id = 0;
        return false;
    }
    atomic = true;
    return true;
}

bool DrmDisplay::Impl::setup_gbm(std::string* error) {
    gbm = gbm_create_device(fd);
    if (!gbm) {
        return set_error(error, "GBM: gbm_create_device failed");
    }
    return true;
}

bool DrmDisplay::Impl::setup_egl(std::string* error) {
    auto get_platform_display =
        reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (get_platform_display) {
        egl_display = get_platform_display(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
    } else {
        egl_display = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(gbm));
    }
    if (egl_display == EGL_NO_DISPLAY) {
        return set_error(error, "EGL: no GBM display");
    }

    EGLint major = 0;
    EGLint minor = 0;
    if (!eglInitialize(egl_display, &major, &minor)) {
        return set_error(error, std::string("eglInitialize failed: ") + egl_error_string(eglGetError()));
    }
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        return set_error(error, "eglBindAPI(EGL_OPENGL_ES_API) failed");
    }

    // The GBM buffer format and the config's alpha size must agree, or
    // eglCreateWindowSurface fails with EGL_BAD_MATCH.
    const struct {
        std::uint32_t format;
        EGLint alpha_size;
    } candidates[] = {
        {GBM_FORMAT_XRGB8888, 0},
        {GBM_FORMAT_ARGB8888, 8},
    };
    EGLConfig config = nullptr;
    EGLint surface_error = EGL_SUCCESS;
    for (const auto& candidate : candidates) {
        surface = gbm_surface_create(gbm, mode.hdisplay, mode.vdisplay, candidate.format,
                                     GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
        if (!surface) {
            continue;
        }
        const EGLint config_attributes[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
            EGL_RED_SIZE, 8,
            EGL_GREEN_SIZE, 8,
            EGL_BLUE_SIZE, 8,
            EGL_ALPHA_SIZE, candidate.alpha_size,
            EGL_NONE,
        };
        EGLint config_count = 0;
        if (eglChooseConfig(egl_display, config_attributes, &config, 1, &config_count) &&
            config_count > 0) {
            const EGLint surface_attributes[] = {
                EGL_RENDER_BUFFER, EGL_BACK_BUFFER,
                EGL_NONE,
            };
            egl_surface = eglCreateWindowSurface(egl_display, config,
                                                 reinterpret_cast<EGLNativeWindowType>(surface),
                                                 surface_attributes);
            if (egl_surface != EGL_NO_SURFACE) {
                break;
            }
        }
        surface_error = eglGetError();
        gbm_surface_destroy(surface);
        surface = nullptr;
    }
    if (!surface || egl_surface == EGL_NO_SURFACE) {
        return set_error(error, "EGL: cannot create a " + std::to_string(mode.hdisplay) + "x" +
                                    std::to_string(mode.vdisplay) + " GBM window surface (" +
                                    egl_error_string(surface_error) + ")");
    }

    const EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 3,
        EGL_NONE,
    };
    egl_context = eglCreateContext(egl_display, config, EGL_NO_CONTEXT, context_attributes);
    if (egl_context == EGL_NO_CONTEXT) {
        return set_error(error, std::string("eglCreateContext failed: ") + egl_error_string(eglGetError()));
    }
    if (!eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context)) {
        return set_error(error, std::string("eglMakeCurrent failed: ") + egl_error_string(eglGetError()));
    }
    eglSwapInterval(egl_display, 0);

    log::info("drm EGL %d.%d, GL %s, renderer %s", major, minor,
              reinterpret_cast<const char*>(glGetString(GL_VERSION)),
              reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
    return true;
}

std::uint32_t DrmDisplay::Impl::framebuffer_for(gbm_bo* bo, std::string* error) {
    for (const Framebuffer& cached : framebuffers) {
        if (cached.bo == bo) {
            return cached.id;
        }
    }
    std::uint32_t id = 0;
    const std::uint32_t width = gbm_bo_get_width(bo);
    const std::uint32_t height = gbm_bo_get_height(bo);
    const std::uint32_t stride = gbm_bo_get_stride(bo);
    const std::uint32_t handle = gbm_bo_get_handle(bo).u32;
    const std::uint32_t format = gbm_bo_get_format(bo);
    std::uint32_t handles[4] = {handle, 0, 0, 0};
    std::uint32_t pitches[4] = {stride, 0, 0, 0};
    std::uint32_t offsets[4] = {0, 0, 0, 0};
    if (drmModeAddFB2(fd, width, height, format, handles, pitches, offsets, &id, 0) != 0) {
        const int value = errno;
        if (drmModeAddFB(fd, width, height, 24, 32, stride, handle, &id) != 0) {
            set_error(error, "DRM: cannot add a framebuffer for the GBM buffer (" +
                                 system_error(value) + ")");
            return 0;
        }
    }
    framebuffers.push_back(Framebuffer{bo, id});
    return id;
}

bool DrmDisplay::Impl::modeset(std::uint32_t framebuffer) {
    if (atomic) {
        drmModeAtomicReqPtr request = drmModeAtomicAlloc();
        bool added = request != nullptr;
        if (added) {
            added =
                drmModeAtomicAddProperty(request, connector_id, connector_crtc_property, crtc_id) >= 0 &&
                drmModeAtomicAddProperty(request, crtc_id, crtc_mode_property, mode_blob) >= 0 &&
                drmModeAtomicAddProperty(request, crtc_id, crtc_active_property, 1) >= 0 &&
                drmModeAtomicAddProperty(request, plane_id, plane_fb_property, framebuffer) >= 0 &&
                drmModeAtomicAddProperty(request, plane_id, plane_crtc_property, crtc_id) >= 0 &&
                drmModeAtomicAddProperty(request, plane_id, plane_src_x_property, 0) >= 0 &&
                drmModeAtomicAddProperty(request, plane_id, plane_src_y_property, 0) >= 0 &&
                drmModeAtomicAddProperty(request, plane_id, plane_src_w_property,
                                         mode.hdisplay << 16) >= 0 &&
                drmModeAtomicAddProperty(request, plane_id, plane_src_h_property,
                                         mode.vdisplay << 16) >= 0 &&
                drmModeAtomicAddProperty(request, plane_id, plane_crtc_x_property, 0) >= 0 &&
                drmModeAtomicAddProperty(request, plane_id, plane_crtc_y_property, 0) >= 0 &&
                drmModeAtomicAddProperty(request, plane_id, plane_crtc_w_property, mode.hdisplay) >= 0 &&
                drmModeAtomicAddProperty(request, plane_id, plane_crtc_h_property, mode.vdisplay) >= 0;
        }
        bool committed = false;
        int commit_error = 0;
        if (added) {
            flip_done = false;
            flip_pending = true;
            committed = drmModeAtomicCommit(fd, request,
                                            DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_NONBLOCK |
                                                DRM_MODE_PAGE_FLIP_EVENT,
                                            this) == 0;
            commit_error = errno;
        }
        drmModeAtomicFree(request);
        if (committed) {
            return true;
        }
        log::warn("drm: atomic modeset failed (%s); falling back to legacy CRTC setup",
                  added ? system_error(commit_error).c_str() : "out of memory");
        flip_pending = false;
        atomic = false;
    }

    flip_done = false;
    flip_pending = false;
    if (drmModeSetCrtc(fd, crtc_id, framebuffer, 0, 0, &connector_id, 1, &mode) != 0) {
        log::error("drm: drmModeSetCrtc failed: %s", system_error(errno).c_str());
        return false;
    }
    flip_done = true;
    return true;
}

bool DrmDisplay::Impl::flip(std::uint32_t framebuffer) {
    if (atomic) {
        drmModeAtomicReqPtr request = drmModeAtomicAlloc();
        bool added = request != nullptr;
        if (added) {
            added = drmModeAtomicAddProperty(request, plane_id, plane_fb_property, framebuffer) >= 0;
        }
        bool committed = false;
        int commit_error = 0;
        if (added) {
            flip_done = false;
            flip_pending = true;
            committed = drmModeAtomicCommit(fd, request,
                                            DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT,
                                            this) == 0;
            commit_error = errno;
        }
        drmModeAtomicFree(request);
        if (committed) {
            return true;
        }
        log::warn("drm: atomic page flip failed (%s); falling back to legacy page flips",
                  added ? system_error(commit_error).c_str() : "out of memory");
        flip_pending = false;
        atomic = false;
    }

    flip_done = false;
    if (drmModePageFlip(fd, crtc_id, framebuffer, DRM_MODE_PAGE_FLIP_EVENT, this) != 0) {
        log::error("drm: drmModePageFlip failed: %s", system_error(errno).c_str());
        return false;
    }
    flip_pending = true;
    return true;
}

bool DrmDisplay::Impl::wait_for_flip() {
    if (!flip_pending) {
        return true;
    }
    drmEventContext context{};
    context.version = 2;
    context.page_flip_handler = &Impl::handle_page_flip;
    while (!flip_done) {
        if (g_quit_requested) {
            return false;
        }
        pollfd descriptor{fd, POLLIN, 0};
        const int ready = poll(&descriptor, 1, 1000);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            log::error("drm: poll on the DRM device failed: %s", system_error(errno).c_str());
            return false;
        }
        if (ready == 0) {
            log::error("drm: page flip did not complete within 1000 ms");
            return false;
        }
        if (drmHandleEvent(fd, &context) != 0) {
            log::error("drm: drmHandleEvent failed: %s", system_error(errno).c_str());
            return false;
        }
    }
    flip_pending = false;
    return true;
}

void DrmDisplay::Impl::restore_crtc() {
    if (!modeset_done) {
        return;
    }
    if (saved_crtc && saved_crtc->mode_valid && saved_crtc->buffer_id) {
        if (drmModeSetCrtc(fd, crtc_id, saved_crtc->buffer_id, saved_crtc->x, saved_crtc->y,
                           &connector_id, 1, &saved_crtc->mode) == 0) {
            log::info("drm: restored the previous CRTC state");
            return;
        }
    } else if (drmModeSetCrtc(fd, crtc_id, 0, 0, 0, nullptr, 0, nullptr) == 0) {
        log::info("drm: disabled the CRTC on shutdown");
        return;
    }
    log::warn("drm: could not restore the previous CRTC state: %s", system_error(errno).c_str());
}

void DrmDisplay::Impl::destroy() {
    if (egl_display != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl_surface != EGL_NO_SURFACE) {
            eglDestroySurface(egl_display, egl_surface);
            egl_surface = EGL_NO_SURFACE;
        }
        if (egl_context != EGL_NO_CONTEXT) {
            eglDestroyContext(egl_display, egl_context);
            egl_context = EGL_NO_CONTEXT;
        }
        eglTerminate(egl_display);
        egl_display = EGL_NO_DISPLAY;
    }
    if (fd >= 0) {
        restore_crtc();
        for (const Framebuffer& framebuffer : framebuffers) {
            if (framebuffer.id) {
                drmModeRmFB(fd, framebuffer.id);
            }
        }
        if (mode_blob) {
            drmModeDestroyPropertyBlob(fd, mode_blob);
            mode_blob = 0;
        }
        if (saved_crtc) {
            drmModeFreeCrtc(saved_crtc);
            saved_crtc = nullptr;
        }
    }
    framebuffers.clear();
    if (surface) {
        if (scanout_bo) {
            gbm_surface_release_buffer(surface, scanout_bo);
            scanout_bo = nullptr;
        }
        gbm_surface_destroy(surface);
        surface = nullptr;
    }
    if (gbm) {
        gbm_device_destroy(gbm);
        gbm = nullptr;
    }
    if (fd >= 0) {
        if (master) {
            drmDropMaster(fd);
            master = false;
        }
        close(fd);
        fd = -1;
    }
    modeset_done = false;
}

DrmDisplay::DrmDisplay(std::string device_path)
    : impl_(std::make_unique<Impl>(std::move(device_path))) {}

DrmDisplay::~DrmDisplay() {
    impl_->destroy();
}

bool DrmDisplay::initialize(std::string* error) {
    std::signal(SIGINT, request_quit);
    std::signal(SIGTERM, request_quit);

    if (!impl_->open_device(error)) {
        impl_->destroy();
        return false;
    }
    if (drmSetMaster(impl_->fd) != 0) {
        const int value = errno;
        set_error(error, "DRM/KMS: cannot become DRM master on " + impl_->device_path + ": " +
                             system_error(value) + access_hint(value));
        impl_->destroy();
        return false;
    }
    impl_->master = true;

    impl_->atomic = impl_->setup_atomic();
    if (!impl_->atomic) {
        log::info("drm: using legacy SetCrtc + page flips");
    }
    impl_->saved_crtc = drmModeGetCrtc(impl_->fd, impl_->crtc_id);

    if (!impl_->setup_gbm(error) || !impl_->setup_egl(error)) {
        impl_->destroy();
        return false;
    }

    size_ = Size{impl_->mode.hdisplay, impl_->mode.vdisplay};
    log::info("drm: %s %s %dx%d@%dHz, %s, GBM %s", impl_->device_path.c_str(),
              impl_->connector_label.c_str(), impl_->mode.hdisplay, impl_->mode.vdisplay,
              static_cast<int>(impl_->mode.vrefresh),
              impl_->atomic ? "atomic" : "legacy page flips",
              gbm_device_get_backend_name(impl_->gbm));
    return true;
}

void DrmDisplay::make_current() {
    if (!impl_ || impl_->egl_surface == EGL_NO_SURFACE) {
        return;
    }
    if (!eglMakeCurrent(impl_->egl_display, impl_->egl_surface, impl_->egl_surface, impl_->egl_context)) {
        fail(std::string("eglMakeCurrent failed: ") + egl_error_string(eglGetError()));
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, size_.width, size_.height);
}

void DrmDisplay::swap() {
    if (!impl_ || failed_ || impl_->egl_surface == EGL_NO_SURFACE) {
        return;
    }
    Impl& drm = *impl_;
    if (!eglSwapBuffers(drm.egl_display, drm.egl_surface)) {
        fail(std::string("eglSwapBuffers failed: ") + egl_error_string(eglGetError()));
        return;
    }
    gbm_bo* bo = gbm_surface_lock_front_buffer(drm.surface);
    if (!bo) {
        fail("GBM: gbm_surface_lock_front_buffer failed");
        return;
    }
    std::string error;
    const std::uint32_t framebuffer = drm.framebuffer_for(bo, &error);
    if (!framebuffer) {
        gbm_surface_release_buffer(drm.surface, bo);
        fail(error);
        return;
    }
    const bool presented = drm.modeset_done ? drm.flip(framebuffer) : drm.modeset(framebuffer);
    if (!presented) {
        gbm_surface_release_buffer(drm.surface, bo);
        failed_ = true;
        return;
    }
    drm.modeset_done = true;
    if (!drm.wait_for_flip()) {
        failed_ = true;
        return;
    }
    if (drm.scanout_bo) {
        gbm_surface_release_buffer(drm.surface, drm.scanout_bo);
    }
    drm.scanout_bo = bo;
}

bool DrmDisplay::should_close() const {
    return g_quit_requested != 0 || failed_ || !impl_;
}

void DrmDisplay::fail(const std::string& message) {
    log::error("%s", message.c_str());
    failed_ = true;
}

}  // namespace pk
