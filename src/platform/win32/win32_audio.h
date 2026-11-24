#pragma once

#include "platform/audio.h"

#include <memory>
#include <string>

namespace pk {

// WASAPI shared-mode capture and render on the default endpoints. Returns
// nullptr with an error when COM or a device cannot be initialized; the
// platform factory then falls back to the file/null backend.
std::unique_ptr<AudioIO> create_wasapi_audio(std::string* error);

}  // namespace pk
