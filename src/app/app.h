#pragma once

#include "platform/platform.h"

#include <string>

namespace pk {

struct AppOptions {
    PlatformOptions platform;
    std::string font_path;
    bool enable_3d = true;
    bool pair = false;
    int pair_timeout_seconds = 600;
    int actor_seconds = 60;

    bool call = false;
    bool voice_aec = true;
    int mute_after_seconds = -1;
    int end_after_seconds = -1;

    bool demo_call = false;
    std::string demo_call_state = "active";

    bool voice_loopback = false;
    int voice_loopback_seconds = 5;

    bool voice_aec_smoke = false;
};

class App {
public:
    int run(const AppOptions& options);
};

}  // namespace pk
