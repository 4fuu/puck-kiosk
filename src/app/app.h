#pragma once

#include "platform/platform.h"

#include <string>

namespace pk {

struct AppOptions {
    PlatformOptions platform;
    std::string font_path;
    bool enable_3d = true;
};

class App {
public:
    int run(const AppOptions& options);
};

}  // namespace pk
