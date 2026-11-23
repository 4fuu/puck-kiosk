#include "puck/puck_view.h"

#include "core/log.h"
#include "puck/puck_2d.h"
#include "puck/puck_3d.h"

#include <memory>

namespace pk {

std::unique_ptr<PuckView> create_puck_view(bool enable_3d) {
#if defined(PUCK_KIOSK_ENABLE_3D)
    if (enable_3d) {
        auto puck = std::make_unique<Puck3D>();
        std::string error;
        if (puck->initialize(&error)) {
            return puck;
        }
        log::info("%s; using the 2D Puck", error.c_str());
    }
#else
    (void)enable_3d;
#endif
    auto puck = std::make_unique<Puck2D>();
    std::string error;
    if (!puck->initialize(&error)) {
        log::info("%s; using the 2D placeholder", error.c_str());
    }
    return puck;
}

}  // namespace pk
