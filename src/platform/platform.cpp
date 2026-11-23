#include "platform/platform.h"

#include "core/log.h"
#include "platform/headless/headless_display.h"

namespace pk {

std::unique_ptr<Platform> create_platform(const PlatformOptions& options, std::string* error) {
    auto platform = std::make_unique<Platform>();

    if (options.backend == "headless") {
        platform->display =
            std::make_unique<HeadlessDisplay>(options.width, options.height, options.output_path, options.frames);
        platform->input = std::make_unique<HeadlessInput>();
        return platform;
    }

    if (error) {
        *error = "platform backend '" + options.backend +
                 "' is not implemented yet; use --platform headless";
    }
    return nullptr;
}

}  // namespace pk
