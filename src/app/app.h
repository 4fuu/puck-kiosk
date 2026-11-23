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
};

class App {
public:
    int run(const AppOptions& options);
};

}  // namespace pk
