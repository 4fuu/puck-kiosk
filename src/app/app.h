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
    // Shared kiosk mode: do not restore a previous account, and leave the
    // temporary car session on hangup/exit so the next user starts at the QR.
    // Personal desktops keep the existing saved-session behavior by default.
    bool temporary_session = false;
    int pair_timeout_seconds = 600;
    // Test hook: after the confirmation code has been visible for this many
    // milliseconds, activate the same Try Again handler the button uses.
    int pair_retry_after_ms = -1;
    // Test hook: request Try Again when a Paired snapshot is consumed, before
    // the app would otherwise establish that account. It locks the
    // Retry/Paired ordering without injecting pointer events.
    bool pair_retry_on_paired = false;
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
