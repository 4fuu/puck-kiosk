#include "app/app.h"

#include "core/log.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

namespace {

void print_help(const char* program) {
    std::cout << "puck-kiosk " << program << "\n\n"
              << "Usage: " << program << " [options]\n\n"
              << "Options:\n"
              << "  --headless              Use the headless EGL backend (default)\n"
              << "  --platform <name>       headless | drm | wayland | x11\n"
              << "  --width <px>            Surface width (default 1280)\n"
              << "  --height <px>           Surface height (default 720)\n"
              << "  --frames <n>            Render n frames (default 1)\n"
              << "  --out <path>            PPM output for the last headless frame\n"
              << "  --font <path>           TTF/OTF font used by the 2D canvas\n"
              << "  --no-3d                 Force the 2D Puck renderer\n"
              << "  --pair                  Pair with a phone, render the QR, then connect the actor\n"
              << "  --pair-timeout <s>      Seconds to wait for phone approval (default 600)\n"
              << "  --actor-seconds <s>     Observation window after pairing; a live call keeps running (default 60)\n"
              << "  --call                  Start a voice call after pairing (requires --pair)\n"
              << "  --mute-after <s>        Mute the call N seconds after it starts\n"
              << "  --end-after <s>         End the call N seconds after it starts\n"
              << "  --no-aec                Disable echo cancellation\n"
              << "  --voice-loopback        Run two local voice peers against each other and exit\n"
              << "  --voice-seconds <s>     Loopback duration in seconds (default 5)\n"
              << "  --voice-aec-smoke       Run the AEC echo-return-loss smoke test and exit\n"
              << "  --demo-call             Render the call screen once and exit\n"
              << "  --demo-call-state <s>   connecting|active|speaking|thinking|muted|ended|failed\n"
              << "  --demo-media <json|path> Render the media overlay once and exit; use null for closed\n"
              << "  --demo-media-index <n>  Item to show in the media demo (default 0)\n"
              << "  --audio <name>          auto | file | null | alsa\n"
              << "  --voice-out <path>      WAV sink for decoded call audio\n"
              << "  --voice-capture <path>  WAV of the synthetic capture signal\n"
              << "  --voice-in <path>       Mono 48 kHz WAV used as capture input instead of the tone\n"
              << "  --alsa-capture <dev>    ALSA capture device (default: default)\n"
              << "  --alsa-playback <dev>   ALSA playback device (default: default)\n"
              << "  -h, --help              Show this help\n";
}

bool parse_int(const char* value, int* out) {
    if (!value || *value == '\0') {
        return false;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed <= 0 || parsed > 100000) {
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

bool parse_nonnegative_int(const char* value, int* out) {
    if (!value || *value == '\0') {
        return false;
    }
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 0 || parsed > 100000) {
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    pk::AppOptions options;
    options.platform.backend = "headless";
    options.platform.width = 1280;
    options.platform.height = 720;
    options.platform.frames = 1;
    options.platform.output_path = "build/pairing.ppm";

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "-h" || argument == "--help") {
            print_help(argv[0]);
            return 0;
        }
        if (argument == "--headless") {
            options.platform.backend = "headless";
            continue;
        }
        if (argument == "--no-3d") {
            options.enable_3d = false;
            continue;
        }
        if (argument == "--pair") {
            options.pair = true;
            continue;
        }
        if (argument == "--call") {
            options.call = true;
            continue;
        }
        if (argument == "--no-aec") {
            options.voice_aec = false;
            continue;
        }
        if (argument == "--voice-loopback") {
            options.voice_loopback = true;
            continue;
        }
        if (argument == "--voice-aec-smoke") {
            options.voice_aec_smoke = true;
            continue;
        }
        if (argument == "--demo-call") {
            options.demo_call = true;
            continue;
        }
        if (argument == "--demo-call-state" && i + 1 < argc) {
            options.demo_call_state = argv[++i];
            continue;
        }
        if (argument == "--demo-media" && i + 1 < argc) {
            options.demo_media = argv[++i];
            continue;
        }
        if (argument == "--demo-media-index" && i + 1 < argc) {
            if (!parse_nonnegative_int(argv[++i], &options.demo_media_index)) {
                pk::log::error("invalid --demo-media-index");
                return 2;
            }
            continue;
        }
        if (argument == "--audio" && i + 1 < argc) {
            const std::string audio = argv[++i];
            if (audio != "auto" && audio != "file" && audio != "null" && audio != "alsa") {
                pk::log::error("invalid --audio value: %s", audio.c_str());
                return 2;
            }
            options.platform.audio_backend = audio;
            continue;
        }
        if (argument == "--voice-out" && i + 1 < argc) {
            options.platform.audio_playback_path = argv[++i];
            continue;
        }
        if (argument == "--voice-capture" && i + 1 < argc) {
            options.platform.audio_capture_path = argv[++i];
            continue;
        }
        if (argument == "--voice-in" && i + 1 < argc) {
            options.platform.audio_input_path = argv[++i];
            continue;
        }
        if (argument == "--alsa-capture" && i + 1 < argc) {
            options.platform.alsa_capture_device = argv[++i];
            continue;
        }
        if (argument == "--alsa-playback" && i + 1 < argc) {
            options.platform.alsa_playback_device = argv[++i];
            continue;
        }
        if (argument == "--mute-after" && i + 1 < argc) {
            if (!parse_nonnegative_int(argv[++i], &options.mute_after_seconds)) {
                pk::log::error("invalid --mute-after");
                return 2;
            }
            continue;
        }
        if (argument == "--end-after" && i + 1 < argc) {
            if (!parse_nonnegative_int(argv[++i], &options.end_after_seconds)) {
                pk::log::error("invalid --end-after");
                return 2;
            }
            continue;
        }
        if (argument == "--voice-seconds" && i + 1 < argc) {
            if (!parse_int(argv[++i], &options.voice_loopback_seconds)) {
                pk::log::error("invalid --voice-seconds");
                return 2;
            }
            continue;
        }
        if (argument == "--platform" && i + 1 < argc) {
            options.platform.backend = argv[++i];
            continue;
        }
        if (argument == "--width" && i + 1 < argc) {
            if (!parse_int(argv[++i], &options.platform.width)) {
                pk::log::error("invalid --width");
                return 2;
            }
            continue;
        }
        if (argument == "--height" && i + 1 < argc) {
            if (!parse_int(argv[++i], &options.platform.height)) {
                pk::log::error("invalid --height");
                return 2;
            }
            continue;
        }
        if (argument == "--frames" && i + 1 < argc) {
            if (!parse_int(argv[++i], &options.platform.frames)) {
                pk::log::error("invalid --frames");
                return 2;
            }
            continue;
        }
        if (argument == "--out" && i + 1 < argc) {
            options.platform.output_path = argv[++i];
            continue;
        }
        if (argument == "--pair-timeout" && i + 1 < argc) {
            if (!parse_int(argv[++i], &options.pair_timeout_seconds)) {
                pk::log::error("invalid --pair-timeout");
                return 2;
            }
            continue;
        }
        if (argument == "--actor-seconds" && i + 1 < argc) {
            if (!parse_nonnegative_int(argv[++i], &options.actor_seconds)) {
                pk::log::error("invalid --actor-seconds");
                return 2;
            }
            continue;
        }
        if (argument == "--font" && i + 1 < argc) {
            options.font_path = argv[++i];
            continue;
        }
        pk::log::error("unknown option: %s", argument.c_str());
        print_help(argv[0]);
        return 2;
    }

    if (!options.platform.output_path.empty()) {
        const std::filesystem::path output(options.platform.output_path);
        if (output.has_parent_path()) {
            std::filesystem::create_directories(output.parent_path());
        }
    }

    pk::log::info("backend=%s size=%dx%d frames=%d", options.platform.backend.c_str(),
                  options.platform.width, options.platform.height, options.platform.frames);
    return pk::App().run(options);
}
