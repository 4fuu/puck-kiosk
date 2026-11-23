#pragma once

#include "app/ui_input.h"
#include "platform/platform.h"

#include <string>
#include <vector>

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

    std::string demo_media;
    int demo_media_index = 0;

    std::string demo_plugin;
    std::string demo_passkey;
    bool demo_debug = false;

    std::vector<ScriptedInput> input_script;
    bool exit_after_script = false;
    bool dump_targets = false;

    std::string echo_cancellation = "on";

    bool voice_loopback = false;
    int voice_loopback_seconds = 5;

    bool voice_aec_smoke = false;
};

class App {
public:
    int run(const AppOptions& options);
};

}  // namespace pk
