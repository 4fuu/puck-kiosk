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

    std::string store_error;
    platform->secure_store = create_file_secure_store(&store_error);
    if (!platform->secure_store) {
        log::warn("%s; sessions will not be persisted", store_error.c_str());
    }

    return platform;
}

}  // namespace pk
