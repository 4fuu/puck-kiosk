#pragma once

#include "platform/audio.h"
#include "platform/display.h"
#include "platform/http.h"
#include "platform/secure_store.h"

#include <memory>
#include <string>

namespace pk {

struct PlatformOptions {
    std::string backend = "headless";
    int width = 1280;
    int height = 720;
    int frames = 1;
    std::string output_path;
    std::string audio_backend = "auto";  // auto | file | null | alsa
    std::string audio_input_path;
    std::string audio_capture_path;
    std::string audio_playback_path;
    double audio_tone_hz = 440.0;
    std::string alsa_capture_device = "default";
    std::string alsa_playback_device = "default";
};

struct Platform {
    std::unique_ptr<Display> display;
    std::unique_ptr<InputSource> input;
    std::unique_ptr<AudioIO> audio;
    std::unique_ptr<HttpClient> http;
    std::unique_ptr<WebSocketFactory> websockets;
    std::unique_ptr<SecureStore> secure_store;
};

// The only place that knows which OS/display/audio backends are compiled in.
// Add a new backend by extending this factory, not the app or renderer.
std::unique_ptr<Platform> create_platform(const PlatformOptions& options, std::string* error);

}  // namespace pk
