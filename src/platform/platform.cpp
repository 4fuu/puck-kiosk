#include "platform/platform.h"

#include "core/log.h"
#include "platform/curl_http.h"
#include "platform/file_secure_store.h"
#include "platform/headless/headless_display.h"
#include "platform/ix_websocket.h"

namespace pk {

std::unique_ptr<Platform> create_platform(const PlatformOptions& options, std::string* error) {
    auto platform = std::make_unique<Platform>();

    if (options.backend == "headless") {
        platform->display =
            std::make_unique<HeadlessDisplay>(options.width, options.height, options.output_path, options.frames);
        platform->input = std::make_unique<HeadlessInput>();
    } else {
        if (error) {
            *error = "platform backend '" + options.backend +
                     "' is not implemented yet; use --platform headless";
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

    const bool headless = options.backend == "headless";
    const bool wants_alsa = options.audio_backend == "alsa" || (options.audio_backend == "auto" && !headless);
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
    if (!platform->audio) {
        FileAudioOptions file;
        file.tone_hz = options.audio_tone_hz;
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
