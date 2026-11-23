#include "app/app.h"

#include "app/car_ui.h"
#include "core/log.h"
#include "puck/puck_view.h"
#include "render/renderer.h"

#include <chrono>
#include <memory>
#include <string>

namespace pk {

int App::run(const AppOptions& options) {
    std::string error;
    auto platform = create_platform(options.platform, &error);
    if (!platform) {
        log::error("%s", error.c_str());
        return 1;
    }

    if (!platform->display->initialize(&error)) {
        log::error("%s: %s", platform->display->backend_name(), error.c_str());
        return 1;
    }
    platform->display->make_current();

    Renderer renderer;
    if (!renderer.initialize(options.font_path, &error)) {
        log::error("%s", error.c_str());
        return 1;
    }

    auto puck = create_puck_view(options.enable_3d);
    log::info("Puck renderer: %s", puck->name());

    CarUi ui(*puck);
    const auto started = std::chrono::steady_clock::now();
    bool running = true;

    while (running && !platform->display->should_close()) {
        InputEvent event;
        while (platform->input && platform->input->poll(event)) {
            if (event.type == InputEvent::Type::Quit) {
                running = false;
            }
        }
        if (!running) {
            break;
        }

        platform->display->make_current();
        renderer.begin(platform->display->size(), platform->display->dpi_scale());
        const float elapsed =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - started).count();
        ui.draw(renderer.canvas(), platform->display->size(), elapsed);
        renderer.end();
        platform->display->swap();
    }

    return 0;
}

}  // namespace pk
