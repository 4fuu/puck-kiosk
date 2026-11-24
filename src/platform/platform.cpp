#include "platform/platform.h"

#include "core/log.h"
#include "platform/curl_http.h"
#include "platform/file_secure_store.h"
#include "platform/ix_websocket.h"
#if !defined(PUCK_KIOSK_BACKEND_WIN32)
#include "platform/headless/headless_display.h"
#endif
#if defined(PUCK_KIOSK_BACKEND_DRM)
#include <cstdlib>

#include "platform/drm/drm_display.h"
#include "platform/drm/drm_input.h"
#endif
#if defined(PUCK_KIOSK_BACKEND_WIN32)
#include "platform/win32/win32_audio.h"
#include "platform/win32/win32_display.h"
#include "platform/win32/xinput_input.h"
#endif

namespace pk {

std::unique_ptr<Platform> create_platform(const PlatformOptions& options, std::string* error) {
    auto platform = std::make_unique<Platform>();

#if defined(PUCK_KIOSK_BACKEND_WIN32)
    // A Windows build has WGL instead of EGL. The "headless" name keeps the
    // existing CLI and CI scripts working: it creates the same WGL context on
    // a hidden window and still writes the PPM frame.
    if (options.backend == "headless" || options.backend == "win32") {
        const bool visible = options.backend == "win32";
        auto display = std::make_unique<Win32Display>(options.width, options.height,
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

    auto http = std::make_unique<CurlHttpClient>();
    if (http->valid()) {
        platform->http = std::move(http);
    } else {
        log::error("libcurl initialization failed; network features are disabled");
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
