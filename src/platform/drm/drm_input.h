#pragma once

#include "platform/display.h"

#include <memory>

namespace pk {

// Touch/pointer/key input for the DRM backend. Devices are opened on the first
// poll, so a board without input hardware logs a warning instead of failing
// startup.
std::unique_ptr<InputSource> create_drm_input(const Display& display);

}  // namespace pk
