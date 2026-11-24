#include "platform/platform.h"

#include "core/log.h"
#include "platform/curl_http.h"
#include "platform/file_secure_store.h"
#include "platform/ix_websocket.h"
#if defined(PUCK_KIOSK_BACKEND_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include "platform/win32/win32_audio.h"
#include "platform/win32/win32_display.h"
#include "platform/win32/xinput_input.h"
#else
#include "platform/headless/headless_display.h"
#endif
#if defined(PUCK_KIOSK_BACKEND_DRM)
#include <cstdlib>

#include "platform/drm/drm_display.h"
#include "platform/drm/drm_input.h"
#endif

namespace pk {
namespace {

#if defined(PUCK_KIOSK_BACKEND_WIN32)
// A GUI-subsystem process starts with unset standard handles. Reopening them on
// the attached console makes the logger work, but a stream the shell redirected
// to a file or pipe must keep pointing there.
void reopen_console_stream(DWORD std_handle, const char* name, const char* mode, FILE* stream) {
    const HANDLE handle = GetStdHandle(std_handle);
    const DWORD type = handle && handle != INVALID_HANDLE_VALUE ? GetFileType(handle)
                                                               : FILE_TYPE_UNKNOWN;
    if (type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE) {
        return;
    }
#if defined(_MSC_VER)
    FILE* reopened = nullptr;
    freopen_s(&reopened, name, mode, stream);
    (void)reopened;
#else
    std::freopen(name, mode, stream);
#endif
}
#endif

}  // namespace

std::unique_ptr<HttpClient> create_http_client(std::string* error) {
    auto http = std::make_unique<CurlHttpClient>();
    if (!http->valid()) {
        if (error) {
            *error = "libcurl initialization failed";
        }
        return nullptr;
    }
    return http;
}

bool attach_process_console(int argc) {
#if defined(PUCK_KIOSK_BACKEND_WIN32)
    bool attached = false;
    if (argc > 1) {
        attached = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
        if (attached) {
            reopen_console_stream(STD_OUTPUT_HANDLE, "CONOUT$", "w", stdout);
            reopen_console_stream(STD_ERROR_HANDLE, "CONOUT$", "w", stderr);
            reopen_console_stream(STD_INPUT_HANDLE, "CONIN$", "r", stdin);
        }
    }
    // Wine models a shell console without a window, so the attach result is the
    // reliable signal there; a window is the user-visible result on Windows.
    return attached || GetConsoleWindow() != nullptr;
#else
    (void)argc;
    return false;
#endif
}

std::unique_ptr<Platform> create_platform(const PlatformOptions& options, std::string* error) {
    auto platform = std::make_unique<Platform>();

#if defined(PUCK_KIOSK_BACKEND_WIN32)
    const bool win32_visible = options.backend == "win32";
#else
    const bool win32_visible = false;
#endif
    if (options.render_scale > 0.0f && !win32_visible) {
        log::warn("--render-scale applies to the visible win32 window; rendering 1:1");
    }

#if defined(PUCK_KIOSK_BACKEND_WIN32)
    // A Windows build has WGL instead of EGL. The "headless" name keeps the
    // existing CLI and CI scripts working: it creates the same WGL context on
    // a hidden window and still writes the PPM frame.
    if (options.backend == "headless" || options.backend == "win32") {
        const bool visible = options.backend == "win32";
        // A hidden window only writes the PPM; keep it 1:1 so frame
        // comparisons against the Linux headless output stay valid.
        const float render_scale =
            visible ? (options.render_scale > 0.0f ? options.render_scale : 2.0f) : 1.0f;
        auto display = std::make_unique<Win32Display>(options.width, options.height, render_scale,
                                                      options.output_path, options.frames, visible,
                                                      visible && options.fullscreen);
        platform->input = std::make_unique<Win32CombinedInput>(*display);
        platform->display = std::move(display);
    } else
#else
    if (options.backend == "headless") {
        platform->display =
            std::make_unique<HeadlessDisplay>(options.width, options.height, options.output_path, options.frames);
        platform->input = std::make_unique<HeadlessInput>();
    } else
#endif
    if (options.backend == "drm") {
#if defined(PUCK_KIOSK_BACKEND_DRM)
        // An explicit card is needed on boards whose first DRM node is a
        // render-only GPU.
        const char* device = std::getenv("PUCK_KIOSK_DRM_DEVICE");
        platform->display = std::make_unique<DrmDisplay>(device ? device : "");
        platform->input = create_drm_input(*platform->display);
#else
        if (error) {
            *error = "platform backend 'drm' is not compiled in; rebuild with --backend=drm";
        }
        return nullptr;
#endif
    } else {
        if (error) {
            *error = "platform backend '" + options.backend +
                     "' is not implemented yet; use --platform headless";
#if defined(PUCK_KIOSK_BACKEND_WIN32)
            *error += " or --platform win32";
#endif
        }
        return nullptr;
    }

    std::string http_error;
    platform->http = create_http_client(&http_error);
    if (!platform->http) {
        log::error("%s; network features are disabled", http_error.c_str());
    }
    platform->websockets = std::make_unique<IxWebSocketFactory>();

#if defined(PUCK_KIOSK_BACKEND_WIN32) && !defined(PUCK_KIOSK_ENABLE_AEC)
    log::info("AEC: built without webrtc-audio-processing on Windows; echo cancellation is disabled");
#endif

    const bool headless = options.backend == "headless";
#if defined(PUCK_KIOSK_BACKEND_WIN32)
    const bool wants_wasapi = options.audio_backend == "wasapi" ||
                              (options.audio_backend == "auto" && !headless);
    if (wants_wasapi) {
        std::string audio_error;
        platform->audio = create_wasapi_audio(&audio_error);
        if (!platform->audio) {
            log::warn("%s; falling back to the file/null audio backend", audio_error.c_str());
        }
    }
    if (options.audio_backend == "alsa") {
        log::warn("--audio alsa is not available on Windows; using the file/null backend");
    }
#else
    const bool wants_alsa = options.audio_backend == "alsa" ||
                            (options.audio_backend == "auto" && !headless);
    if (options.audio_backend == "wasapi") {
        log::warn("--audio wasapi is only available on Windows; using the file/null backend");
    }
    if (wants_alsa) {
        std::string audio_error;
        AlsaAudioOptions alsa;
        alsa.capture_device = options.alsa_capture_device;
        alsa.playback_device = options.alsa_playback_device;
        platform->audio = create_alsa_audio(alsa, &audio_error);
        if (!platform->audio) {
            log::warn("%s; falling back to the file/null audio backend", audio_error.c_str());
        }
    }
#endif
    if (!platform->audio) {
        FileAudioOptions file;
        file.tone_hz = options.audio_tone_hz;
        file.input_path = options.audio_input_path;
        file.capture_path = options.audio_capture_path;
        file.playback_path = options.audio_playback_path;
        std::string audio_error;
        platform->audio = create_file_audio(file, &audio_error);
        if (!platform->audio) {
            log::warn("audio backend unavailable: %s", audio_error.c_str());
        }
    }
    if (platform->audio) {
        log::info("audio backend: %s", platform->audio->name());
    }

    std::string store_error;
    platform->secure_store = create_file_secure_store(&store_error);
    if (!platform->secure_store) {
        log::warn("%s; sessions will not be persisted", store_error.c_str());
    }

    return platform;
}

}  // namespace pk
